#!/usr/bin/env python3
"""P3-08 kill/restart reconnect replay process test against a real ChatServer
(dedicated schema chat_p308_$$, independent v1/v2 ports, set up by
reconnect_replay_test.sh).

The harness owns the ChatServer process (spawn via `setarch x86_64 -R`, kill -9,
restart) so the exact pid is captured and no broad pkill can kill an unrelated
process; the .sh owns schema migration, config, env, log tee and cleanup.

Scenarios (docs/tasks/P3-08.md 验证 节):
- 在线投递不 ACK：A->B 单聊（client_message_id）被 B 收到（msgid=6, message_id），
  故意不 ACK；
- kill -9（无 sessionClosed 清理，lease 未到期）：进程实例 boot id 使跨进程
  "uid:gen" owner 必不同，重启后 B 重新登录立即重领重投；
- 重启后重投：B 收到同一 message_id 同内容（at-least-once，客户端按 message_id
  去重）；无未知 message_id；
- B ACK 后无再次投递；
- offline C：kill 前 A->C（C 离线）的 Pending 行重启后 C 登录收到。
Exit code 0 iff RECONNECT_REPLAY_ALL_PASS; any failure exits 1 (never skip).
"""
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

V2_MAGIC = 0x4D434854  # "MCHT"
V2_VERSION = 2
V2_HEADER_LEN = 20
V2_CONTENT_TYPE_JSON = 1

FAIL = []
SERVER = None
HOST = ""
V1_PORT = 0
V2_PORT = 0


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name, flush=True)
    else:
        print("FAIL %s %s" % (name, detail), flush=True)
        FAIL.append(name)


def log_tail(log_path):
    try:
        with open(log_path, "r", errors="replace") as f:
            lines = f.read().splitlines()
        return "\n".join(lines[-15:])
    except OSError:
        return "<no server log>"


class V2Client(object):
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""

    @staticmethod
    def frame(body):
        head = struct.pack(">IBBHIHBB", V2_MAGIC, V2_VERSION, 0, V2_HEADER_LEN,
                           len(body), 0, V2_CONTENT_TYPE_JSON, 0)
        head += struct.pack(">I", 0)
        return head + body

    def send(self, obj):
        self.sock.sendall(self.frame(json.dumps(obj, separators=(",", ":")).encode("utf-8")))

    def recv(self, timeout=10.0):
        self.sock.settimeout(timeout)
        try:
            while len(self.buf) < V2_HEADER_LEN:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
            body_len = struct.unpack(">I", self.buf[8:12])[0]
            while len(self.buf) < V2_HEADER_LEN + body_len:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
            body = self.buf[V2_HEADER_LEN:V2_HEADER_LEN + body_len]
            self.buf = self.buf[V2_HEADER_LEN + body_len:]
            return json.loads(body.decode("utf-8"))
        except (socket.timeout, ValueError, ConnectionResetError):
            return None

    def recv_nothing(self, t):
        self.sock.settimeout(t)
        try:
            chunk = self.sock.recv(4096)
            return chunk is None or chunk == b""
        except (socket.timeout, ConnectionResetError):
            return True

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def is_delivery(r):
    # 投递格式（P3-07 冻结）：msgid=6/10 + message_id/conversation_id/sequence +
    # content（无 errno、无 duplicate）。
    return (r is not None and r.get("message_id", 0) > 0
            and "conversation_id" in r and "sequence" in r and "content" in r)


def login(cli, uid):
    cli.send({"msgid": 1, "id": uid, "password": "pwd"})
    r = cli.recv()
    return r is not None and r.get("errno") == 0


def reg(cli, name):
    cli.send({"msgid": 4, "name": name, "password": "pwd"})
    r = cli.recv()
    return r.get("id", 0) if r is not None and r.get("errno") == 0 else 0


def ack(cli, message_id):
    cli.send({"msgid": 12, "message_id": message_id})


def send_direct(cli, from_id, to_id, cmid, content):
    cli.send({"msgid": 6, "id": from_id, "toid": to_id,
              "client_message_id": cmid, "content": content})
    r = cli.recv()
    return r if r is not None and r.get("msgid") == 11 and not r.get("duplicate") else None


def _port_ready(host, port):
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


def wait_server_ready(log_path, timeout=30.0):
    # main.cpp 打印 "Server started" 早于 v2Server.start()（V2 listen bind），
    # 仅靠日志行判就绪会与 V2 端口绑定竞态（P3-08 Gate B，ASan 下偶发
    # ConnectionRefusedError）。改为：日志行确认启动无崩溃 + 有界轮询探测
    # V2_PORT 真正可连接，两者都满足才算就绪。探测是进程 harness 惯例，
    # 不引入固定 sleep 掩盖断言。
    deadline = time.time() + timeout
    log_ready = False
    while time.time() < deadline:
        if not log_ready:
            try:
                with open(log_path, "r", errors="replace") as f:
                    if "Server started" in f.read():
                        log_ready = True
            except OSError:
                pass
        if log_ready and _port_ready(HOST, V2_PORT):
            return True
        if SERVER is not None and SERVER.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def spawn_server(server_bin, config_path, log_path):
    global SERVER
    log = open(log_path, "w")
    cmd = ["setarch", "x86_64", "-R", server_bin, HOST, str(V1_PORT),
           "--config", config_path]
    env = dict(os.environ)
    SERVER = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
    return SERVER, log


def kill_server(proc, log_path):
    if proc is None or proc.poll() is not None:
        return
    os.kill(proc.pid, signal.SIGKILL)
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        print("FAIL rr_server_exit %s" % log_path, flush=True)
        FAIL.append("rr_server_exit")
        return
    check("rr_server_killed", proc.returncode is not None, "rc=%s" % proc.returncode)


def main():
    global HOST, V1_PORT, V2_PORT
    HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    V1_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 17001
    V2_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else V1_PORT + 1000
    server_bin = sys.argv[4]
    config_path = sys.argv[5]
    log_path = sys.argv[6]
    phase2_log = log_path + ".phase2"
    suffix = str(int(time.time() * 1000))[-8:]

    proc = None
    log = None
    clients = []
    all_deliveries = []
    try:
        proc, log = spawn_server(server_bin, config_path, log_path)
        check("rr_start_server", wait_server_ready(log_path), log_tail(log_path))

        # ---- Phase 1：注册登录；A->C 离线消息；A->B 在线投递但故意不 ACK ----
        va = V2Client(HOST, V2_PORT)
        vb = V2Client(HOST, V2_PORT)
        vc = V2Client(HOST, V2_PORT)
        clients += [va, vb, vc]
        aid = reg(va, "rr_a_%s" % suffix)
        bid = reg(vb, "rr_b_%s" % suffix)
        cid = reg(vc, "rr_c_%s" % suffix)
        check("rr_reg_abc", aid > 0 and bid > 0 and cid > 0)
        check("rr_login_a", login(va, aid))
        check("rr_login_b", login(vb, bid))

        r = send_direct(va, aid, cid, "rr-c-%s" % suffix, "offline for c")
        check("rr_accept_offline_c", r is not None, str(r))
        mid_c = r["message_id"]

        r = send_direct(va, aid, bid, "rr-b-%s" % suffix, "killme message")
        check("rr_accept_b", r is not None, str(r))
        mid_1 = r["message_id"]
        d = vb.recv()
        check("rr_delivery_b_first",
              is_delivery(d) and d["msgid"] == 6 and d["message_id"] == mid_1
              and d["content"] == "killme message", str(d))
        all_deliveries.append(d)
        # 故意不 ACK。

        # ---- kill -9（精确 pid；无 sessionClosed 清理，lease 未到期）----
        check("rr_server_alive", proc is not None and proc.poll() is None,
              log_tail(log_path))
        kill_server(proc, log_path)
        if log is not None:
            log.close()
            log = None

        # ---- Phase 2：重启；B 重登重投同 message_id；ACK 后无再投；C 补投 ----
        proc, log = spawn_server(server_bin, config_path, phase2_log)
        check("rr_restart_server", wait_server_ready(phase2_log), log_tail(phase2_log))

        vb2 = V2Client(HOST, V2_PORT)
        vc2 = V2Client(HOST, V2_PORT)
        clients += [vb2, vc2]
        check("rr_relogin_b", login(vb2, bid))
        d = vb2.recv()
        check("rr_replay_same_message_id",
              is_delivery(d) and d["message_id"] == mid_1, str(d))
        check("rr_replay_same_content",
              is_delivery(d) and d["content"] == "killme message", str(d))
        all_deliveries.append(d)

        ack(vb2, mid_1)
        check("rr_no_redelivery_after_ack", vb2.recv_nothing(1.0))

        check("rr_login_c", login(vc2, cid))
        d = vc2.recv()
        check("rr_offline_c_delivered",
              is_delivery(d) and d["message_id"] == mid_c
              and d["content"] == "offline for c", str(d))
        all_deliveries.append(d)
        ack(vc2, mid_c)
        check("rr_c_no_more", vc2.recv_nothing(1.0))

        # 无未知 message_id：整个测试收到的全部投递 message_id 均为已知集合子集。
        known = {mid_1, mid_c}
        seen = [x.get("message_id") for x in all_deliveries]
        check("rr_no_unknown_message_id", set(seen).issubset(known), str(seen))
    finally:
        if proc is not None and proc.poll() is None:
            os.kill(proc.pid, signal.SIGKILL)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                pass
        if log is not None:
            log.close()
        for c in clients:
            c.close()

    if FAIL:
        print("RECONNECT_REPLAY_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("RECONNECT_REPLAY_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
