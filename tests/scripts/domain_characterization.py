#!/usr/bin/env python3
"""P2-00/P3-06 domain characterization: run the full v1/v2 use-case matrix against a
live ChatServer (v1 on <port>, v2 hardcoded on <port+1000>/7000) and assert both
protocols produce the same behavior.

Matrix rows map to docs/specs/domain-behavior-matrix.md (B-01..B-27 plus P3-05
B-21 tightening checks I1-I6, plus P3-06 accept-path checks: MESSAGE_ACCEPTED
msgid=11, 幂等重试 duplicate=true, 错误码 101/103/105/106, legacy 旧格式回显).
P3-06 迁移（B-11/B-12/B-17 在线转发与离线入队退役）：断言"已接受"语义，
投递属 P3-07（登录补投暂无 Delivery）。
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

# P3-06 决策表第 6 行冻结：content 上限 16KB（UTF-8 字节）。
MAX_CONTENT_BYTES = 16 * 1024


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


def accepted(r):
    # P3-06：v2 命令 → MESSAGE_ACCEPTED（msgid=11，五字段，无 errno）。
    return (r is not None and r.get("msgid") == 11
            and "client_message_id" in r and "message_id" in r
            and "conversation_id" in r and "sequence" in r and "duplicate" in r)


def errresp(r, errno):
    # P3-06：稳定错误响应 msgid=13 + errno + errmsg。
    return r is not None and r.get("msgid") == 13 and r.get("errno") == errno


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
        # P3-06 决策表第 6 行：B-13 整条 payload>500 判定废止 → content 16KB
        # （UTF-8 字节）上限。v2（含 client_message_id）超限 → errno=105
        # （msgid=13 稳定错误）；legacy（缺 cmid）→ 旧格式回显 errno=1。
        overlong = "a" * (MAX_CONTENT_BYTES + 1)
        v1.send({"msgid": 6, "id": aid, "toid": bid, "client_message_id": "cm-long-" + suffix,
                 "content": overlong})
        r = v1.recv()
        check("E4_v1 overlong chat 105", errresp(r, 105)
              and r.get("errmsg") == "content too long", str(r))
        v1.send({"msgid": 6, "id": aid, "toid": bid, "content": overlong})
        r = v1.recv()
        check("E4b_v1 legacy overlong chat rejected", r is not None
              and r.get("errno") == 1 and r.get("errmsg") == "content too long", str(r))
        # 恰 16KB → 接受（MESSAGE_ACCEPTED；B-13 旧上限不支配新路径）
        v1.send({"msgid": 6, "id": aid, "toid": bid,
                 "client_message_id": "cm-bound-" + suffix,
                 "content": "b" * MAX_CONTENT_BYTES})
        r = v1.recv()
        check("E4c_v1 16KB boundary accepted", accepted(r) and not r["duplicate"], str(r))

        # 准备离线场景：B 登录后登出
        v2.send(login(bid))
        r = v2.recv()
        check("C8_v2 login B", r is not None and r.get("errno") == 0, str(r))
        v2.send({"msgid": 3, "id": bid})
        r = v2.recv()
        check("C9_v2 logout B", r is not None and r.get("errno") == 0, str(r))

        # B-12（P3-06 迁移）单聊离线：durable accept，MESSAGE_ACCEPTED 在事务
        # 提交后发出（不再写 OfflineMessage；投递属 P3-07）。
        msg = {"msgid": 6, "id": aid, "toid": bid, "content": "hello",
               "client_message_id": "cm-d4-" + suffix}
        v1.send(msg)
        r = v1.recv()
        check("D4_v1 offline chat accepted", accepted(r) and not r["duplicate"], str(r))
        mid4 = r["message_id"]
        cid4 = r["conversation_id"]
        seq4 = r["sequence"]
        # 故障点 1（spec §4）：accept 回复丢失 → 同 command 重试返回原 identity
        # （duplicate=true，同一 message_id/conversation_id/sequence）。
        v1.send(msg)
        r = v1.recv()
        check("D5_v1 retry same command duplicate", accepted(r) and r["duplicate"]
              and r["message_id"] == mid4 and r["conversation_id"] == cid4
              and r["sequence"] == seq4, str(r))
        # 同 key 不同 payload：IdempotencyConflict → 103（不得当作 duplicate=true）
        bad = dict(msg)
        bad["content"] = "different payload"
        v1.send(bad)
        r = v1.recv()
        check("D6_v1 same key different payload 103", errresp(r, 103), str(r))
        # legacy 命令（无 client_message_id）→ 旧格式回显 errno=0（决策表第 10 行）
        v1.send({"msgid": 6, "id": aid, "toid": bid, "content": "hello legacy"})
        r = v1.recv()
        check("D7_v1 legacy chat old-format echo", r is not None
              and r.get("errno") == 0 and r.get("toid") == bid and r.get("msgid") == 6, str(r))
        # M2（对抗审查）：旧字段别名 msg→content（spec §5.1）——无 content 无 cmid
        # 的旧客户端（msg 字段）→ legacy 通道 → 旧格式回显 errno=0（回显保留 msg）。
        v1.send({"msgid": 6, "id": aid, "toid": bid, "msg": "hello via msg field"})
        r = v1.recv()
        check("D7b_v1 legacy msg-field alias accepted", r is not None
              and r.get("errno") == 0 and r.get("toid") == bid and r.get("msgid") == 6
              and r.get("msg") == "hello via msg field", str(r))
        # B-09（P3-06 迁移）登录：先回 LOGIN_MSG_ACK；补投暂无 Delivery（P3-07），
        # 不产生任何离线消息。
        v2.send(login(bid))
        r = v2.recv()
        check("D8_v2 login B again", r is not None and r.get("errno") == 0, str(r))
        check("D9_v2 no delivery yet (P3-07)", v2.recv_timeout(0.5))

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
        # B-17（P3-06 迁移）群聊：accept 事务内快照成员（决策表第 8 行），
        # 发送者收 MESSAGE_ACCEPTED；在线转发退役（投递属 P3-07）。
        gmsg = {"msgid": 10, "id": aid, "groupid": gid, "content": "hi all",
                "client_message_id": "cm-g4-" + suffix}
        v1.send(gmsg)
        r = v1.recv()
        check("G4_v1 group chat accepted", accepted(r) and not r["duplicate"], str(r))
        # v2 线（BinaryFrame）上同一 accept 语义：B 成员经 v2 codec 接受群聊
        v2.send({"msgid": 10, "id": bid, "groupid": gid, "content": "hi from b",
                 "client_message_id": "cm-g4b-" + suffix})
        r = v2.recv()
        check("G4b_v2 group chat accepted (v2 codec)", accepted(r) and not r["duplicate"],
              str(r))
        check("G5_v2 no delivery yet (P3-07)", v2.recv_timeout(0.5))
        # B-18（P3-06 收紧落地）非成员发群聊 → 101（NotConversationMember），
        # 不再 errno=0（P3-05 后发送者身份取自 Session：payload id 必须与连接
        # 绑定用户一致，故用 cid）。
        v1b.send(reg(C))
        r = v1b.recv()
        cid = r.get("id", 0)
        v1b.send(login(cid))
        r = v1b.recv()
        check("G6_v1b C login", r is not None and r.get("errno") == 0, str(r))
        v1b.send({"msgid": 10, "id": cid, "groupid": gid, "content": "hi all",
                  "client_message_id": "cm-g7-" + suffix})
        r = v1b.recv()
        check("G7_v1b non-member group chat 101", errresp(r, 101), str(r))
        # legacy 群聊（无 cmid，成员 A）→ 旧格式回显 errno=0
        v1.send({"msgid": 10, "id": aid, "groupid": gid, "content": "hi legacy"})
        r = v1.recv()
        check("G8_v1 legacy group chat old-format echo", r is not None
              and r.get("errno") == 0 and r.get("msgid") == 10, str(r))
        # B-19（P3-06 收紧落地）超长群聊 → 105（v2）/ errno=1（legacy）
        v1.send({"msgid": 10, "id": aid, "groupid": gid, "content": overlong,
                 "client_message_id": "cm-g9-" + suffix})
        r = v1.recv()
        check("G9_v1 overlong group chat 105", errresp(r, 105), str(r))
        v1.send({"msgid": 10, "id": aid, "groupid": gid, "content": overlong})
        r = v1.recv()
        check("G9b_v1 legacy overlong group rejected", r is not None
              and r.get("errno") == 1 and r.get("errmsg") == "content too long", str(r))

        # P3-06 spec §2.4 NotFound：目标用户/群不存在 → 106（v2 稳定错误）
        v1.send({"msgid": 6, "id": aid, "toid": 99999991,
                 "client_message_id": "cm-j1-" + suffix, "content": "hi"})
        r = v1.recv()
        check("J1_v1 direct to missing user 106", errresp(r, 106), str(r))
        v1.send({"msgid": 10, "id": aid, "groupid": 99999991,
                 "client_message_id": "cm-j2-" + suffix, "content": "hi"})
        r = v1.recv()
        check("J2_v1 group chat to missing group 106", errresp(r, 106), str(r))

        # B-20 断开释放会话：断开后重连登录成功
        v2.close()
        v2b = V2Client(host, v2port)
        v2b.send(login(bid))
        r = v2b.recv()
        check("H1_v2 reconnect after disconnect", r is not None and r.get("errno") == 0,
              str(r))
        v2b.close()

        # P3-05 B-21 收紧：同一连接绑定一个认证 Session；消息主体只来自 Session，
        # 不信任 payload id。此时 v1 绑定 A（E3 登录后未登出），B 离线。
        v1.send(login(bid))
        r = v1.recv()
        check("I1_v1 switch user on same conn rejected", r is not None
              and r.get("msgid") == 2 and r.get("errno") == 2, str(r))
        v1.send(login(aid))
        r = v1.recv()
        check("I2_v1 same user relogin on same conn rejected", r is not None
              and r.get("msgid") == 2 and r.get("errno") == 2, str(r))
        # 未登录连接发单聊 → 明确拒绝（errno=1），不入 ledger
        v1c = V1Client(host, v1port)
        v1c.send({"msgid": 6, "id": bid, "toid": bid, "content": "unauth"})
        r = v1c.recv()
        check("I3_v1c unauthenticated chat rejected", r is not None
              and r.get("errno") == 1 and r.get("errmsg") == "please login first!", str(r))
        # A（v1）伪造 Bob id 发单聊 → 拒绝（P3-06 codec 在前：content 字段合法，
        # 会话检查拒绝；伪造消息不得进入 ledger）
        v1.send({"msgid": 6, "id": bid, "toid": bid, "content": "forged"})
        r = v1.recv()
        check("I4_v1 forged sender id rejected", r is not None
              and r.get("errno") == 1 and r.get("errmsg") == "invalid sender!", str(r))
        # 伪造消息不得产生 B 的 Delivery/离线投递：登录 B 后无任何消息
        v2c = V2Client(host, v2port)
        v2c.send(login(bid))
        r = v2c.recv()
        check("I5_v2c login B after forged chat", r is not None and r.get("errno") == 0,
              str(r))
        check("I5b_v2c no forged offline message", v2c.recv_timeout(0.5))
        # 伪造 id 群聊 → 拒绝
        v1.send({"msgid": 10, "id": bid, "groupid": gid, "content": "forged group"})
        r = v1.recv()
        check("I6_v1 forged group sender id rejected", r is not None
              and r.get("errno") == 1 and r.get("errmsg") == "invalid sender!", str(r))
        v1c.close()
        v2c.close()

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
