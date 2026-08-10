#!/usr/bin/env python3
"""P2-08 multi-reactor process test: run a concurrent business matrix against a
ChatServer started with threadNum N (argv[3]): concurrent registration, login,
direct chat, disconnect and reconnect must all succeed.

Exit code 0 iff MULTIREACTOR_ALL_PASS.
"""
import json
import socket
import sys
import time

FAIL = []


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name)
    else:
        print("FAIL %s %s" % (name, detail))
        FAIL.append(name)


class LineClient(object):
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall(json.dumps(obj, separators=(",", ":")).encode() + b"\n")

    def recv(self, timeout=5.0):
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            return json.loads(line.decode())
        except (socket.timeout, ValueError):
            return None

    def close(self):
        self.sock.close()


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6000
    thread_num = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    suffix = str(int(time.time() * 1000))[-8:]
    n = 8
    users = ["mu_%d_%s" % (i, suffix) for i in range(n)]
    clients = [LineClient(host, port) for _ in range(n)]
    ids = [0] * n

    try:
        # 并发注册（每连接一个用户）
        for i in range(n):
            clients[i].send({"msgid": 4, "name": users[i], "password": "pw"})
        for i in range(n):
            r = clients[i].recv()
            check("reg_%d" % i, r is not None and r.get("errno") == 0 and r.get("id", 0) > 0, str(r))
            if r:
                ids[i] = r["id"]

        # 并发登录
        for i in range(n):
            clients[i].send({"msgid": 1, "id": ids[i], "password": "pw"})
        for i in range(n):
            r = clients[i].recv()
            check("login_%d" % i, r is not None and r.get("errno") == 0, str(r))

        # 并发互聊（i -> (i+1)%n，目标在线）
        # 注意：多 Reactor 下 ACK 与转发出自不同 I/O loop，到达顺序不定，
        # 客户端必须按字段区分（errno 存在=ACK）而非依赖顺序。
        for i in range(n):
            clients[i].send({"msgid": 6, "id": ids[i], "toid": ids[(i + 1) % n],
                             "msg": "hi %d" % i, "time": "t"})
        for i in range(n):
            got_ack = False
            got_fwd = False
            for _ in range(2):
                r = clients[i].recv()
                if r is None:
                    break
                if "errno" in r:
                    got_ack = got_ack or (r["errno"] == 0)
                else:
                    got_fwd = got_fwd or (r.get("msgid") == 6)
            check("chat_%d" % i, got_ack and got_fwd, "ack=%s fwd=%s" % (got_ack, got_fwd))

        # 并发登出
        for i in range(n):
            clients[i].send({"msgid": 3, "id": ids[i]})
        for i in range(n):
            r = clients[i].recv()
            check("logout_%d" % i, r is not None and r.get("errno") == 0, str(r))

        # 并发断开
        for c in clients:
            c.close()

        # 重连并重新登录（会话已释放）
        clients2 = [LineClient(host, port) for _ in range(n)]
        for i in range(n):
            clients2[i].send({"msgid": 1, "id": ids[i], "password": "pw"})
        for i in range(n):
            r = clients2[i].recv()
            check("reconnect_%d" % i, r is not None and r.get("errno") == 0, str(r))
        for c in clients2:
            c.close()
    finally:
        for c in clients:
            c.close()

    if FAIL:
        print("MULTIREACTOR_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("MULTIREACTOR_ALL_PASS loops=%d users=%d" % (thread_num, n))
    sys.exit(0)


if __name__ == "__main__":
    main()
