#!/usr/bin/env python3
"""P3-07 backpressure delivery process test.

B is a deliberately slow consumer.  The sender phase keeps B's socket unread
until an accepted-count barrier and the server's own Channel trace both prove
that B entered pressure.  Only then does the sole reader thread consume B's
socket.  The accepted-count barrier is not used to manufacture recovery: the
``events=4`` transition in the dedicated server log is pressure evidence, and
a later ``events=7``/``events=3`` transition is recovery evidence.

Every accepted message_id must arrive at least once; unknown ids fail and
duplicate deliveries are allowed.  The same reader owns B until the ordered
Channel recovery evidence is observed; no protocol command is used as a
liveness probe because msgid=3 has logout side effects.  Exit code 0 iff
BACKPRESSURE_ALL_PASS.
"""
import socket
import struct
import json
import sys
import time
import os
import re
import threading
import traceback

V2_MAGIC = 0x4D434854  # "MCHT"
V2_VERSION = 2
V2_HEADER_LEN = 20
V2_CONTENT_TYPE_JSON = 1

# 1200 conversations × 15.4KB ≈ 18.5MB outstanding > 16MB pauseReadBytes。
N_SENDERS = 1200
CONTENT = "x" * (15 * 1024)  # 15KB < 16KB content 上限
PRESSURE_ACCEPTED_MIN = 700
READER_TIMEOUT = 180.0

FAIL = []


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name)
    else:
        print("FAIL %s %s" % (name, detail))
        FAIL.append(name)


def _b_channel_events(log_path, v2port):
    """Return B's fd and (Channel event, connection-count) observations."""
    if not log_path:
        return None, []
    try:
        with open(log_path, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        return None, []
    connection = re.search(
        r"new connection \[ChatServerV2-[^\]\r\n]*:%d#1\]" % v2port,
        text,
    )
    if connection is None:
        return None, []
    initial = re.search(
        r"updateChannel:fd=(\d+) events=3 index=-1",
        text[connection.start():connection.start() + 4096],
    )
    if initial is None:
        return None, []
    fd = initial.group(1)
    events = []
    suffix = text[connection.start():]
    for match in re.finditer(
        r"updateChannel:fd=%s(?!\d) events=(\d+) index=[-\d]+" % re.escape(fd), suffix
    ):
        prefix = text[:connection.start() + match.start()]
        conn_ids = [int(x) for x in re.findall(r"ChatServerV2-[^#\r\n]+#(\d+)", prefix)]
        events.append((int(match.group(1)), max(conn_ids) if conn_ids else 0))
    return fd, events


def wait_for_pressure_barrier(log_path, v2port, timeout=30.0):
    """Wait for B's read-disabled (EPOLLOUT-only) transition."""
    deadline = time.time() + timeout
    if not log_path:
        return False, "P307_SERVER_LOG is unset"
    while time.time() < deadline:
        b_fd, events = _b_channel_events(log_path, v2port)
        for index, (event, connection_count) in enumerate(events):
            if event == 4:
                return True, "fd=%s events=4 event_index=%d connections=%d" % (
                    b_fd, index, connection_count)
        time.sleep(0.05)
    return False, "pressure barrier timeout"


def wait_for_pressure_recovery(log_path, v2port, timeout=30.0):
    """Wait for B's read-enabled transition after pressure."""
    deadline = time.time() + timeout
    if not log_path:
        return False, "P307_SERVER_LOG is unset"
    while time.time() < deadline:
        b_fd, events = _b_channel_events(log_path, v2port)
        pressure_index = next((index for index, (event, count) in enumerate(events)
                               if event == 4), None)
        if pressure_index is not None:
            after_pressure = events[pressure_index + 1:]
            recovered = next(((event, count) for event, count in after_pressure if event in (3, 7)), None)
            if recovered is not None:
                return True, "fd=%s events=4->%s connections=%d" % (
                    b_fd, recovered[0], recovered[1])
        time.sleep(0.05)
    return False, "pressure recovery evidence timeout"


def wait_for_live_connection(log_path, v2port, timeout=30.0):
    """Require recovery to finish at events=3 before client cleanup."""
    deadline = time.time() + timeout
    if not log_path:
        return False, "P307_SERVER_LOG is unset"
    while time.time() < deadline:
        b_fd, events = _b_channel_events(log_path, v2port)
        pressure_index = next((index for index, (event, count) in enumerate(events)
                               if event == 4), None)
        if pressure_index is not None:
            after_pressure = events[pressure_index + 1:]
            if any(event == 0 for event, count in after_pressure):
                return False, "fd=%s closed before cleanup" % b_fd
            recovered = next((index for index, (event, count) in enumerate(
                after_pressure) if event in (3, 7)), None)
            if recovered is not None and after_pressure[-1][0] == 3:
                return True, "fd=%s events=4->%s->3" % (
                    b_fd, after_pressure[recovered][0])
        time.sleep(0.05)
    return False, "connection recovery did not settle at events=3"


class V2Client(object):
    def __init__(self, host, port, small_recv=False, sock=None):
        self.sock = sock if sock is not None else socket.create_connection(
            (host, port), timeout=15)
        if small_recv:
            # 慢消费者：缩小内核接收缓冲，使服务端 reserve 预算（outstanding）
            # 逼近 16MB（默认内核缓冲会吸收约 2-6MB，达不到暂停阈值）。
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 131072)
        self.buf = b""
        self.last_recv_status = "none"

    @staticmethod
    def frame(body):
        head = struct.pack(">IBBHIHBB", V2_MAGIC, V2_VERSION, 0, V2_HEADER_LEN,
                           len(body), 0, V2_CONTENT_TYPE_JSON, 0)
        head += struct.pack(">I", 0)
        return head + body

    def send(self, obj):
        self.sock.sendall(self.frame(json.dumps(obj, separators=(",", ":")).encode("utf-8")))

    def recv(self, timeout=15.0):
        self.sock.settimeout(timeout)
        try:
            while len(self.buf) < V2_HEADER_LEN:
                chunk = self.sock.recv(65536)
                if not chunk:
                    self.last_recv_status = "eof"
                    raise EOFError("peer closed before frame header")
                self.buf += chunk
            body_len = struct.unpack(">I", self.buf[8:12])[0]
            while len(self.buf) < V2_HEADER_LEN + body_len:
                chunk = self.sock.recv(65536)
                if not chunk:
                    self.last_recv_status = "eof"
                    raise EOFError("peer closed before frame body")
                self.buf += chunk
            body = self.buf[V2_HEADER_LEN:V2_HEADER_LEN + body_len]
            self.buf = self.buf[V2_HEADER_LEN + body_len:]
            result = json.loads(body.decode("utf-8"))
            self.last_recv_status = "message"
            return result
        except socket.timeout:
            self.last_recv_status = "timeout"
            return None
        except (ValueError, ConnectionResetError, OSError) as error:
            self.last_recv_status = type(error).__name__
            raise

    def close(self):
        self.sock.close()


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    v2port = int(sys.argv[3]) if len(sys.argv) > 3 else 7000
    suffix = str(int(time.time() * 1000))[-8:]

    b = V2Client(host, v2port, small_recv=True)  # B 是慢消费者
    senders = []
    received = []
    received_ids = set()
    send_done = threading.Event()
    accepted_barrier = threading.Event()
    sent_ids = set()
    barrier_state = {"ok": False, "detail": "not started"}
    recovery_state = {"ok": False, "detail": "not attempted"}
    live_state = {"ok": False, "detail": "not attempted"}
    reader_state = {"done": False, "error": ""}
    reader_thread = None

    def reader_owner():
        try:
            # B's socket has exactly one recv owner after login.  The accepted
            # barrier and server event trace must precede the first read.
            if not accepted_barrier.wait(READER_TIMEOUT):
                barrier_state["detail"] = "accepted-count barrier timeout"
                return
            ok, detail = wait_for_pressure_barrier(
                os.environ.get("P307_SERVER_LOG"), v2port)
            barrier_state["ok"] = ok
            barrier_state["detail"] = "accepted>=%d %s" % (PRESSURE_ACCEPTED_MIN, detail)
            if not ok:
                print("READER barrier-failed %s" % detail, flush=True)
                return
            print("READER barrier-ok %s" % detail, flush=True)
            drain_deadline = None
            recovery_checked = False
            while True:
                r = b.recv(0.5)
                if r is not None:
                    received.append(r)
                    if "message_id" in r:
                        received_ids.add(r.get("message_id"))

                if (send_done.is_set() and not recovery_checked
                        and sent_ids.issubset(received_ids)):
                    recovery_state["ok"], recovery_state["detail"] = wait_for_pressure_recovery(
                        os.environ.get("P307_SERVER_LOG"), v2port)
                    recovery_checked = True

                if (send_done.is_set() and recovery_checked and recovery_state["ok"]
                        and sent_ids.issubset(received_ids)):
                    live_state["ok"], live_state["detail"] = wait_for_live_connection(
                        os.environ.get("P307_SERVER_LOG"), v2port)
                    break

                if send_done.is_set():
                    if drain_deadline is None:
                        drain_deadline = time.time() + READER_TIMEOUT
                    if time.time() >= drain_deadline:
                        break

            print("READER drain-exit received=%d distinct=%d" %
                  (len(received), len(received_ids)), flush=True)
            if not recovery_checked:
                recovery_state["ok"], recovery_state["detail"] = wait_for_pressure_recovery(
                    os.environ.get("P307_SERVER_LOG"), v2port)
        except BaseException as e:
            reader_state["error"] = repr(e)
            traceback.print_exc()
        finally:
            reader_state["done"] = True
            print("READER done=%s live=%s error=%s" %
                  (reader_state["done"], live_state, reader_state["error"]), flush=True)

    try:
        b.send({"msgid": 4, "name": "bp_b_%s" % suffix, "password": "pwd"})
        r = b.recv()
        bid = r["id"] if r is not None and r.get("errno") == 0 else 0
        check("bp_reg_b", bid > 0)
        b.send({"msgid": 1, "id": bid, "password": "pwd"})
        r = b.recv()
        check("bp_login_b", r is not None and r.get("errno") == 0, str(r))
        reader_thread = threading.Thread(target=reader_owner, daemon=True)
        reader_thread.start()

        sent = []
        accepted_encoded_bytes = 0
        t0 = time.time()
        for i in range(N_SENDERS):
            s = V2Client(host, v2port)
            name = "bp_s%d_%s" % (i, suffix)
            s.send({"msgid": 4, "name": name, "password": "pwd"})
            r = s.recv()
            sid = r["id"] if r is not None and r.get("errno") == 0 else 0
            s.send({"msgid": 1, "id": sid, "password": "pwd"})
            r = s.recv()
            if r is None or r.get("errno") != 0:
                check("bp_sender_login_%d" % i, False, str(r))
                break
            s.send({"msgid": 6, "id": sid, "toid": bid,
                    "client_message_id": "bp-%d-%s" % (i, suffix), "content": CONTENT})
            r = s.recv()
            if r is None or r.get("msgid") != 11 or r.get("duplicate"):
                check("bp_accept_%d" % i, False, str(r))
                break
            sent.append(r["message_id"])
            sent_ids.add(r["message_id"])
            delivery_payload = {
                "msgid": 6,
                "id": sid,
                "toid": bid,
                "content": CONTENT,
                "message_id": r["message_id"],
                "conversation_id": r["conversation_id"],
                "sequence": r["sequence"],
            }
            accepted_encoded_bytes += V2_HEADER_LEN + len(
                json.dumps(delivery_payload, separators=(",", ":")).encode("utf-8"))
            senders.append(s)
            if len(sent) == PRESSURE_ACCEPTED_MIN:
                accepted_barrier.set()
                print("bp_pressure_accept_barrier accepted=%d encoded_bytes=%d" %
                      (len(sent), accepted_encoded_bytes), flush=True)
            if i in (0, N_SENDERS // 2, N_SENDERS - 1):
                print("bp_progress %d/1200 (%.1fs)" % (i + 1, time.time() - t0))
        check("bp_all_accepted", len(sent) == N_SENDERS, "got %d" % len(sent))
        print("bp_send_phase_secs %.1f" % (time.time() - t0))

        send_done.set()
        if reader_thread is not None:
            reader_thread.join(READER_TIMEOUT + 30.0)
        check("bp_pressure_barrier", barrier_state["ok"], barrier_state["detail"])
        check("bp_reader_done", reader_state["done"] and not reader_state["error"],
              reader_state["error"] or "reader not done")

        deliveries = [r for r in received if "message_id" in r]
        mids = [r.get("message_id") for r in deliveries]
        distinct = set(mids)
        sent_set = set(sent)
        check("bp_all_delivered_at_least_once", sent_set.issubset(distinct),
              "missing=%d received=%d" % (len(sent_set - distinct), len(received)))
        check("bp_no_unknown_message_id", distinct.issubset(sent_set),
              str(distinct - sent_set))
        check("bp_distinct_count", len(distinct) == N_SENDERS, "distinct=%d" % len(distinct))
        bad_shape = [r for r in deliveries
                     if r.get("msgid") != 6 or r.get("content") != CONTENT
                     or "conversation_id" not in r or "sequence" not in r]
        check("bp_delivery_shape", not bad_shape, str(bad_shape[:1]))
        check("bp_pressure_recovery", recovery_state["ok"], recovery_state["detail"])
        check("bp_conn_alive_after_drain", live_state["ok"], live_state["detail"])
    finally:
        send_done.set()
        if reader_thread is not None:
            reader_thread.join(5.0)
        b.close()
        for s in senders:
            s.close()

    if FAIL:
        print("BACKPRESSURE_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("BACKPRESSURE_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
