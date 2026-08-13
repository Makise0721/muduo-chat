#!/usr/bin/env python3
"""P2-08 multi-reactor process test: run a concurrent business matrix against a
ChatServer started with threadNum N (argv[3]): concurrent registration, login,
direct chat (P3-06 durable accept; P3-07 online delivery + legacy implicit-ack),
logout, disconnect and reconnect must all succeed.

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

    def recv_timeout(self, t):
        self.sock.settimeout(t)
        try:
            chunk = self.sock.recv(4096)
            if not chunk:
                return True
            self.buf += chunk
            return False
        except (socket.timeout, ConnectionResetError):
            return True

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

        # P3-06 迁移（B-11 在线直写退役）+ P3-07 投递：并发互聊（i -> (i+1)%n，
        # 目标在线）走 durable accept（executor 单 worker 串行）：legacy 命令
        # （无 client_message_id）收旧格式回显 errno=0；目标（在线）立即收到
        # 投递（msgid=6 + message_id + content，legacy implicit-ack 无需 ACK）。
        # 回显与投递经不同 loop 排队，到达顺序不确定——按形状配对断言。
        for i in range(n):
            clients[i].send({"msgid": 6, "id": ids[i], "toid": ids[(i + 1) % n],
                             "content": "hi %d" % i})
        for i in range(n):
            r1 = clients[i].recv()
            r2 = clients[i].recv()
            echo = r1 if (r1 is not None and "errno" in r1) else r2
            dlv = r2 if echo is r1 else r1
            check("chat_%d" % i, echo is not None and echo.get("errno") == 0, str(echo))
            check("chat_%d_delivery" % i, dlv is not None and dlv.get("msgid") == 6
                  and dlv.get("message_id", 0) > 0
                  and dlv.get("id") == ids[(i - 1) % n]
                  and dlv.get("content") == "hi %d" % ((i - 1) % n), str(dlv))

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
