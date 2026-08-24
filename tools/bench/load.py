#!/usr/bin/env python3
"""P5-02 reliable load generator (v2 msgid 11/12 path).

Scenarios:
  direct  - N sender/recipient pairs, both online; sender v2 oneChat (msgid 6 +
            client_message_id), waits MESSAGE_ACCEPTED (msgid 11); recipient
            receives delivery and sends DELIVERY_ACK (msgid 12).
  group   - N senders, each in a pre-seeded group with online members; v2
            groupChat (msgid 10 + client_message_id), waits msgid 11; members
            receive delivery and ACK (msgid 12).
  hot     - N senders all target a single offline target (fixed id); v2 oneChat,
            waits msgid 11 (no delivery/ACK). Validates row-lock serialization
            on accept (P3-11).

One line per run:
  scenario=.. conns=N duration_ms=T ok=K msg_per_sec=P p50_ms=.. p95_ms=.. p99_ms=.. errors=E
"""
import json
import socket
import sys
import threading
import time

USAGE = ("usage: load.py <host> <port> <direct|group|hot> <conns> <duration_ms> "
         "[--target <id>] [--base <id>] [--password <pwd>]")
PASSWORD = "123456"
UNIQ = int(time.time() * 1000)  # per-process unique prefix for client_message_id


def send_line(sock, obj):
    sock.sendall(json.dumps(obj).encode() + b"\n")


def recv_line(sock, timeout):
    sock.settimeout(timeout)
    buf = b""
    while b"\n" not in buf:
        try:
            chunk = sock.recv(4096)
        except (socket.timeout, ConnectionResetError, OSError):
            return None
        if not chunk:
            return None
        buf += chunk
    line, _ = buf.split(b"\n", 1)
    try:
        return json.loads(line)
    except ValueError:
        return None


def login(sock, uid, timeout=10):
    send_line(sock, {"msgid": 1, "id": uid, "password": PASSWORD})
    r = recv_line(sock, timeout)
    return r is not None and r.get("errno") == 0


def _is_delivery(r, msgid):
    return (r is not None and r.get("msgid") == msgid
            and r.get("message_id", 0) > 0
            and "conversation_id" in r and "sequence" in r)


class DirectPair:
    """Sender (user base+2i) -> recipient (user base+2i+1), both online."""

    def __init__(self, host, port, sender_id, recipient_id, duration_ms, out, lock):
        self.host, self.port = host, port
        self.sender_id, self.recipient_id = sender_id, recipient_id
        self.duration_ms = duration_ms
        self.out, self.lock = out, lock

    def run(self):
        try:
            s = socket.create_connection((self.host, self.port), timeout=10)
        except OSError:
            with self.lock:
                self.out["errors"] += 1
            return
        if not login(s, self.sender_id):
            with self.lock:
                self.out["errors"] += 1
            s.close()
            return
        # recipient online + ACK thread (its own connection)
        r = socket.create_connection((self.host, self.port), timeout=10)
        if not login(r, self.recipient_id):
            with self.lock:
                self.out["errors"] += 1
            s.close()
            r.close()
            return
        stop = threading.Event()
        ack_thread = threading.Thread(target=self._ack_loop, args=(r, stop))
        ack_thread.start()

        lat = []
        ok = 0
        deadline = time.time() + self.duration_ms / 1000.0
        cmid = 0
        while time.time() < deadline:
            cmid += 1
            start = time.time()
            try:
                send_line(s, {"msgid": 6, "id": self.sender_id,
                              "toid": self.recipient_id,
                              "client_message_id": "p502-%d-%d-%d" % (UNIQ, self.sender_id, cmid),
                              "content": "bench payload"})
            except (socket.timeout, OSError):
                with self.lock:
                    self.out["errors"] += 1
                break
            ack = recv_line(s, 10)
            if ack is None or ack.get("msgid") != 11:
                with self.lock:
                    self.out["errors"] += 1
                break
            lat.append((time.time() - start) * 1000.0)
            ok += 1
        stop.set()
        ack_thread.join(timeout=5)
        s.close()
        r.close()
        with self.lock:
            self.out["ok"] += ok
            self.out["latencies"].extend(lat)

    def _ack_loop(self, sock, stop):
        while not stop.is_set():
            r = recv_line(sock, 1)
            if r is None:
                continue
            if _is_delivery(r, 6) or _is_delivery(r, 10):
                try:
                    send_line(sock, {"msgid": 12, "message_id": r["message_id"]})
                except (socket.timeout, OSError):
                    return


class GroupSender:
    """Sender (user id) -> pre-seeded group (groupid), members online ACK."""

    def __init__(self, host, port, sender_id, group_id, members, duration_ms,
                 out, lock):
        self.host, self.port = host, port
        self.sender_id, self.group_id, self.members = sender_id, group_id, members
        self.duration_ms = duration_ms
        self.out, self.lock = out, lock

    def run(self):
        try:
            s = socket.create_connection((self.host, self.port), timeout=10)
        except OSError:
            with self.lock:
                self.out["errors"] += 1
            return
        if not login(s, self.sender_id):
            with self.lock:
                self.out["errors"] += 1
            s.close()
            return
        ack_socks = []
        stop = threading.Event()
        threads = []
        for m in self.members:
            try:
                ms = socket.create_connection((self.host, self.port), timeout=10)
            except OSError:
                with self.lock:
                    self.out["errors"] += 1
                continue
            if not login(ms, m):
                with self.lock:
                    self.out["errors"] += 1
                ms.close()
                continue
            ack_socks.append(ms)
            t = threading.Thread(target=self._ack_loop, args=(ms, stop))
            t.start()
            threads.append(t)

        lat = []
        ok = 0
        deadline = time.time() + self.duration_ms / 1000.0
        cmid = 0
        while time.time() < deadline:
            cmid += 1
            start = time.time()
            try:
                send_line(s, {"msgid": 10, "id": self.sender_id,
                              "groupid": self.group_id,
                              "client_message_id": "p502g-%d-%d-%d" % (UNIQ, self.sender_id, cmid),
                              "content": "bench group payload"})
            except (socket.timeout, OSError):
                with self.lock:
                    self.out["errors"] += 1
                break
            # 群发送者也是成员：先收自身投递（msgid 10 带 message_id，需 ACK），
            # 再收 MESSAGE_ACCEPTED（msgid 11）。读到 11 才算 accept 完成。
            accepted = None
            end = time.time() + 10
            while time.time() < end and accepted is None:
                ack = recv_line(s, 3)
                if ack is None:
                    break
                if _is_delivery(ack, 10) or _is_delivery(ack, 6):
                    try:
                        send_line(s, {"msgid": 12, "message_id": ack["message_id"]})
                    except (socket.timeout, OSError):
                        break
                    continue
                if ack.get("msgid") == 11:
                    accepted = ack
                    break
            if accepted is None:
                with self.lock:
                    self.out["errors"] += 1
                break
            lat.append((time.time() - start) * 1000.0)
            ok += 1
        stop.set()
        for t in threads:
            t.join(timeout=5)
        s.close()
        for ms in ack_socks:
            ms.close()
        with self.lock:
            self.out["ok"] += ok
            self.out["latencies"].extend(lat)

    def _ack_loop(self, sock, stop):
        while not stop.is_set():
            r = recv_line(sock, 1)
            if r is None:
                continue
            if _is_delivery(r, 10) or _is_delivery(r, 6):
                try:
                    send_line(sock, {"msgid": 12, "message_id": r["message_id"]})
                except (socket.timeout, OSError):
                    return


class HotSender:
    """Sender (user id) -> single fixed offline target (target id)."""

    def __init__(self, host, port, sender_id, target_id, duration_ms, out, lock):
        self.host, self.port = host, port
        self.sender_id, self.target_id = sender_id, target_id
        self.duration_ms = duration_ms
        self.out, self.lock = out, lock

    def run(self):
        try:
            s = socket.create_connection((self.host, self.port), timeout=10)
        except OSError:
            with self.lock:
                self.out["errors"] += 1
            return
        if not login(s, self.sender_id):
            with self.lock:
                self.out["errors"] += 1
            s.close()
            return
        lat = []
        ok = 0
        deadline = time.time() + self.duration_ms / 1000.0
        cmid = 0
        while time.time() < deadline:
            cmid += 1
            start = time.time()
            try:
                send_line(s, {"msgid": 6, "id": self.sender_id,
                              "toid": self.target_id,
                              "client_message_id": "p502h-%d-%d-%d" % (UNIQ, self.sender_id, cmid),
                              "content": "bench hot payload"})
            except (socket.timeout, OSError):
                with self.lock:
                    self.out["errors"] += 1
                break
            ack = recv_line(s, 10)
            if ack is None or ack.get("msgid") != 11:
                with self.lock:
                    self.out["errors"] += 1
                break
            lat.append((time.time() - start) * 1000.0)
            ok += 1
        s.close()
        with self.lock:
            self.out["ok"] += ok
            self.out["latencies"].extend(lat)


def percentile(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    idx = int((len(values) - 1) * p)
    return values[idx]


def main():
    argv = sys.argv[1:]
    if len(argv) < 5:
        print(USAGE)
        return 2
    host, port = argv[0], int(argv[1])
    scenario, conns, duration_ms = argv[2], int(argv[3]), int(argv[4])
    target = 0
    base = 1
    for i in range(5, len(argv) - 1):
        if argv[i] == "--target":
            target = int(argv[i + 1])
        elif argv[i] == "--base":
            base = int(argv[i + 1])
    if scenario not in ("direct", "group", "hot"):
        print(USAGE)
        return 2
    if scenario == "hot" and target == 0:
        print("hot scenario requires --target <offline user id>")
        return 2

    out = {"ok": 0, "errors": 0, "latencies": []}
    lock = threading.Lock()
    workers = []
    if scenario == "direct":
        for i in range(conns):
            workers.append(DirectPair(host, port, base + 2 * i, base + 2 * i + 1,
                                      duration_ms, out, lock))
    elif scenario == "group":
        # group i (1-based): sender = base+i-1, members = sender + 3 online
        # (users 200+i, 300+i, 400+i pre-seeded as group members).
        for i in range(conns):
            gid = i + 1
            sid = base + i
            members = [200 + gid, 300 + gid, 400 + gid]
            workers.append(GroupSender(host, port, sid, gid, members,
                                       duration_ms, out, lock))
    else:
        for i in range(conns):
            workers.append(HotSender(host, port, base + i, target,
                                     duration_ms, out, lock))

    start = time.time()
    threads = [threading.Thread(target=w.run) for w in workers]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    # 吞吐口径 = 固定采集窗口（duration_ms）；登录在前、不计入窗口（卡冻结
    # 「每重复采集窗口」= duration）。elapsed 另记用于审计。
    window_s = duration_ms / 1000.0
    elapsed_ms = max(1.0, (time.time() - start) * 1000.0)

    lat = out["latencies"]
    print("scenario=%s conns=%d duration_ms=%d ok=%d msg_per_sec=%.1f "
          "p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f errors=%d"
          % (scenario, conns, int(elapsed_ms), out["ok"],
             out["ok"] / window_s,
             percentile(lat, 0.50), percentile(lat, 0.95), percentile(lat, 0.99),
             out["errors"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())