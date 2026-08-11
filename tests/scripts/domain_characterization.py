#!/usr/bin/env python3
"""P2-00 domain characterization: run the full v1/v2 use-case matrix against a
live ChatServer (v1 on <port>, v2 hardcoded on <port+1000>/7000) and assert both
protocols produce the same behavior.

Matrix rows map to docs/DOMAIN_BEHAVIOR_MATRIX.md (B-01..B-23).
Exit code 0 iff MATRIX_ALL_PASS.
"""
import json
import socket
import struct
import sys
import time

V2_MAGIC = 0x4D434854  # "MCHT"
V2_VERSION = 2
V2_HEADER_LEN = 20
V2_CONTENT_TYPE_JSON = 1
V2_DEFAULT_MAX_BODY = 1024 * 1024


class V1Client(object):
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall(json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\n")

    def send_raw(self, data):
        self.sock.sendall(data if isinstance(data, bytes) else data.encode("utf-8"))

    def recv(self, timeout=5.0):
        self.sock.settimeout(timeout)
        try:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            return json.loads(line.decode("utf-8"))
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

    def send_payload_raw(self, body):
        self.sock.sendall(self.frame(body))

    def recv(self, timeout=5.0):
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


FAIL = []


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name)
    else:
        print("FAIL %s %s" % (name, detail))
        FAIL.append(name)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    v1port = int(sys.argv[2]) if len(sys.argv) > 2 else 6000
    v2port = int(sys.argv[3]) if len(sys.argv) > 3 else 7000

    suffix = str(int(time.time() * 1000))[-8:]
    A, B, C = "ua_%s" % suffix, "ub_%s" % suffix, "uc_%s" % suffix
    PWD = "pwd123"
    aid = bid = 0
    gid = 0

    v1 = V1Client(host, v1port)
    v2 = V2Client(host, v2port)
    v1b = V1Client(host, v1port)
    try:
        # B-01..B-03 协议防御：坏 JSON/无 msgid/未知 msgid 静默丢弃、连接保持
        for cl, tag in ((v1, "v1"), (v2, "v2")):
            if tag == "v1":
                cl.send_raw(b"not json\n")
            else:
                cl.send_payload_raw(b"not json")
            check("D1_%s bad json silent" % tag, cl.recv_timeout(0.4))
            cl.send({"a": 1})
            check("D2_%s no msgid silent" % tag, cl.recv_timeout(0.4))
            cl.send({"msgid": 99})
            check("D3_%s unknown msgid silent" % tag, cl.recv_timeout(0.4))

        def reg(name):
            return {"msgid": 4, "name": name, "password": PWD}

        def login(uid):
            return {"msgid": 1, "id": uid, "password": PWD}

        # B-04/B-05 注册与重名（v1/v2 对称）
        v1.send(reg(A))
        r = v1.recv()
        check("C1_v1 register ok", r is not None and r.get("msgid") == 5
              and r.get("errno") == 0 and r.get("id", 0) > 0, str(r))
        aid = r["id"]
        v1.send(reg(A))
        r = v1.recv()
        check("C2_v1 duplicate name", r is not None and r.get("msgid") == 5
              and r.get("errno") == 1, str(r))
        v2.send(reg(B))
        r = v2.recv()
        check("C1_v2 register ok", r is not None and r.get("msgid") == 5
              and r.get("errno") == 0 and r.get("id", 0) > 0, str(r))
        bid = r["id"]
        v2.send(reg(B))
        r = v2.recv()
        check("C2_v2 duplicate name", r is not None and r.get("msgid") == 5
              and r.get("errno") == 1, str(r))

        # B-06 登录成功：errno=0 + id/name + 空好友列表
        v1.send(login(aid))
        r = v1.recv()
        check("C3_v1 login ok", r is not None and r.get("msgid") == 2
              and r.get("errno") == 0 and r.get("name") == A
              and r.get("friends") == [] and r.get("friendDetails") == [], str(r))
        # B-07 错密码
        v2.send({"msgid": 1, "id": aid, "password": "WRONG"})
        r = v2.recv()
        check("C4_v2 wrong password", r is not None and r.get("msgid") == 2
              and r.get("errno") == 1, str(r))
        # B-08 重复登录（A 已在 v1 在线）→ 单会话约束
        v2.send(login(aid))
        r = v2.recv()
        check("C5_v2 duplicate login rejected", r is not None and r.get("msgid") == 2
              and r.get("errno") == 2, str(r))
        # B-10 登出（msgid=3 = LOGINOUT_MSG）与幂等
        v1.send({"msgid": 3, "id": aid})
        r = v1.recv()
        check("C6_v1 logout", r is not None and r.get("msgid") == 3
              and r.get("errno") == 0, str(r))
        v1.send({"msgid": 3, "id": aid})
        r = v1.recv()
        check("C7_v1 idempotent logout", r is not None and r.get("errno") == 0, str(r))

        # B-25 防御：已知 msgid 但必填字段缺失（{"msgid":1} 无 id/password、
        # {"msgid":4} 无 name）→ handler 全异常捕获 → 静默不响应、连接保持
        v1.send({"msgid": 1})
        check("E1_v1 login missing fields silent", v1.recv_timeout(0.4))
        v1.send({"msgid": 4})
        check("E2_v1 reg missing name silent", v1.recv_timeout(0.4))
        # 连接未断开：继续正常收发（A 重新登录成功）
        v1.send(login(aid))
        r = v1.recv()
        check("E3_v1 conn alive after missing fields", r is not None
              and r.get("errno") == 0, str(r))
        # B-13 超长单聊：整条 payload >500 → errno=1 "message too long"（决策
        # 落地，在线/离线一致；toid=aid 自聊，长度检查先于转发路径）
        v1.send({"msgid": 6, "id": aid, "toid": aid, "msg": "a" * 600, "time": "t"})
        r = v1.recv()
        check("E4_v1 overlong chat rejected", r is not None and r.get("errno") == 1
              and r.get("errmsg") == "message too long", str(r))

        # 准备离线场景：B 登录后登出
        v2.send(login(bid))
        r = v2.recv()
        check("C8_v2 login B", r is not None and r.get("errno") == 0, str(r))
        v2.send({"msgid": 3, "id": bid})
        r = v2.recv()
        check("C9_v2 logout B", r is not None and r.get("errno") == 0, str(r))

        # B-12 单聊离线：errno=0 表示已接受（B-13 超长消息行为依赖 MySQL
        # sql_mode，不锁进测试；见 docs/DOMAIN_BEHAVIOR_MATRIX.md）
        msg = {"msgid": 6, "id": aid, "toid": bid, "msg": "hello", "time": "2024-01-01 12:00:00"}
        v1.send(msg)
        r = v1.recv()
        check("D4_v1 offline chat accepted", r is not None and r.get("errno") == 0
              and r.get("toid") == bid, str(r))
        # B-09 登录补投：先 ACK 后离线消息，离线消息为原始 Command 副本
        v2.send(login(bid))
        r = v2.recv()
        check("D6_v2 login B again", r is not None and r.get("errno") == 0, str(r))
        r = v2.recv()
        check("D7_v2 offline delivery", r is not None and r.get("msgid") == 6
              and r.get("msg") == "hello" and "errno" not in r, str(r))
        # B-11 单聊在线：目标收原始（无 errno），发送者收副本 + errno=0
        v1.send(msg)
        r = v1.recv()
        check("D8_v1 sender ack online", r is not None and r.get("errno") == 0, str(r))
        r = v2.recv()
        check("D9_v2 recipient receives raw", r is not None and r.get("msgid") == 6
              and r.get("msg") == "hello" and "errno" not in r, str(r))

        # B-14 加好友：有向边成功/重复冲突
        v1.send({"msgid": 7, "id": aid, "friendid": bid})
        r = v1.recv()
        check("F1_v1 add friend", r is not None and r.get("errno") == 0, str(r))
        v1.send({"msgid": 7, "id": aid, "friendid": bid})
        r = v1.recv()
        check("F2_v1 duplicate friend", r is not None and r.get("errno") == 1, str(r))
        # B-06 好友列表：friends + friendDetails（B 在线 → state=online）
        v1.send({"msgid": 3, "id": aid})
        v1.recv()
        v1.send(login(aid))
        r = v1.recv()
        details = r.get("friendDetails") or []
        check("F3_v1 friend list", r is not None and r.get("errno") == 0
              and r.get("friends") == [B] and len(details) == 1
              and details[0].get("friendid") == bid and details[0].get("state") == "online",
              str(r))

        # B-15 建群：creator 入群
        v1.send({"msgid": 8, "id": aid, "groupname": "g" + suffix, "groupdesc": "d"})
        r = v1.recv()
        check("G1_v1 create group", r is not None and r.get("msgid") == 8
              and r.get("errno") == 0 and r.get("groupid", 0) > 0, str(r))
        gid = r["groupid"]
        # B-16 入群与重复加入
        v2.send({"msgid": 9, "id": bid, "groupid": gid})
        r = v2.recv()
        check("G2_v2 join group", r is not None and r.get("errno") == 0, str(r))
        v2.send({"msgid": 9, "id": bid, "groupid": gid})
        r = v2.recv()
        check("G3_v2 duplicate join", r is not None and r.get("errno") == 1, str(r))
        # B-17 群聊：在线成员收原始，发送者收确认
        gmsg = {"msgid": 10, "id": aid, "groupid": gid, "msg": "hi all", "time": "t2"}
        v1.send(gmsg)
        r = v1.recv()
        check("G4_v1 group ack", r is not None and r.get("errno") == 0, str(r))
        r = v2.recv()
        check("G5_v2 member receives group msg", r is not None and r.get("msgid") == 10
              and r.get("msg") == "hi all", str(r))
        # B-18 非成员可发群聊（现状：不校验发送者身份）
        v1b.send(reg(C))
        r = v1b.recv()
        cid = r.get("id", 0)
        v1b.send(login(cid))
        r = v1b.recv()
        check("G6_v1b C login", r is not None and r.get("errno") == 0, str(r))
        v1b.send(gmsg)
        r = v1b.recv()
        check("G7_v1b non-member group chat acked", r is not None and r.get("errno") == 0,
              str(r))
        # B-19 超长群聊（见 B-13 同款检查）：整条 payload >500 → errno=1 "message too long"（与单聊
        # E4 一致，长度检查先于成员查询/转发）
        v1.send({"msgid": 10, "id": aid, "groupid": gid, "msg": "a" * 600, "time": "t"})
        r = v1.recv()
        check("G8_v1 overlong group chat rejected", r is not None
              and r.get("errno") == 1 and r.get("errmsg") == "message too long", str(r))

        # B-20 断开释放会话：断开后重连登录成功
        v2.close()
        v2b = V2Client(host, v2port)
        v2b.send(login(bid))
        r = v2b.recv()
        check("H1_v2 reconnect after disconnect", r is not None and r.get("errno") == 0,
              str(r))
        v2b.close()

        # B-23 v2 帧上限：超 1MiB → 服务端关闭连接（v1 无对应限制）
        v2c = V2Client(host, v2port)
        try:
            v2c.send_payload_raw(b"x" * (V2_DEFAULT_MAX_BODY + 1))
        except (BrokenPipeError, ConnectionResetError):
            pass  # forceClose 可能发生在发送中，属期望结果
        check("T1_v2 oversize frame closes conn", v2c.recv() is None)
        v2c.close()
    finally:
        v1.close()
        v2.close()
        v1b.close()

    if FAIL:
        print("MATRIX_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("MATRIX_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
