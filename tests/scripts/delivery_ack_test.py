#!/usr/bin/env python3
"""P3-07 delivery & ACK process test against a real ChatServer (v1 <port>,
v2 <port+1000>/7000, dedicated migrated schema chat_p307, set up by
delivery_ack_test.sh).

Scenarios (docs/tasks/P3-07.md RED)：
- 在线投递：accept 后立即 claim 在线接收者；HOL 单在途；DELIVERY_ACK 放行下一 sequence；
- 重复 ACK：幂等无副作用（无回执、无重投）；
- 发送后断线（ACK 丢失）：断线 InFlight 回 Pending，重连重投同 message_id（客户端去重）；
- 他人 ACK：Bob ACK Carol 的消息被忽略（Carol 的 Delivery 不被终结）；
- 离线投递：登录立即 claim 名下 Pending；
- legacy implicit-ack（v1）：投递即确认，重连不重投。
Exit code 0 iff DELIVERY_ACK_ALL_PASS.
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

FAIL = []


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name)
    else:
        print("FAIL %s %s" % (name, detail))
        FAIL.append(name)


class V1Client(object):
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""

    def send(self, obj):
        self.sock.sendall(json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\n")

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

    def recv_nothing(self, t):
        self.sock.settimeout(t)
        try:
            chunk = self.sock.recv(4096)
            return chunk is None or chunk == b""
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

    def recv_nothing(self, t):
        self.sock.settimeout(t)
        try:
            chunk = self.sock.recv(4096)
            return chunk is None or chunk == b""
        except (socket.timeout, ConnectionResetError):
            return True

    def close(self):
        self.sock.close()


def is_delivery(r):
    # 投递格式（P3-07 冻结）：msgid=6/10 + message_id/conversation_id/sequence +
    # content（无 errno、无 duplicate——与 MESSAGE_ACCEPTED/旧回显区分）。
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


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    v1port = int(sys.argv[2]) if len(sys.argv) > 2 else 6000
    v2port = int(sys.argv[3]) if len(sys.argv) > 3 else 7000
    suffix = str(int(time.time() * 1000))[-8:]

    # ---- v2 场景：在线 HOL + ACK 放行 / 重复 ACK / 断线重投 / 他人 ACK / 离线补投 ----
    va = V2Client(host, v2port)
    vb = V2Client(host, v2port)
    vc = V2Client(host, v2port)
    try:
        aid = reg(va, "da_a_%s" % suffix)
        bid = reg(vb, "da_b_%s" % suffix)
        cid = reg(vc, "da_c_%s" % suffix)
        check("da_reg", aid > 0 and bid > 0 and cid > 0)
        check("da_login_a", login(va, aid))
        check("da_login_b", login(vb, bid))
        check("da_login_c", login(vc, cid))

        # S1 在线投递 + HOL + ACK 放行
        r = send_direct(va, aid, bid, "da1-%s" % suffix, "first")
        check("da_s1_accept_m1", r is not None, str(r))
        mid1, cid1 = r["message_id"], r["conversation_id"]
        d = vb.recv()
        check("da_s1_delivery_m1", is_delivery(d) and d["msgid"] == 6
              and d["message_id"] == mid1 and d["conversation_id"] == cid1
              and d["sequence"] == 1 and d["content"] == "first"
              and d["id"] == aid and d["toid"] == bid, str(d))

        r = send_direct(va, aid, bid, "da2-%s" % suffix, "second")
        check("da_s1_accept_m2", r is not None, str(r))
        mid2 = r["message_id"]
        # HOL：同 conversation 前序未确认，m2 不越过（不投递）
        check("da_s1_hol_blocks_m2", vb.recv_nothing(0.8))
        # ACK m1 放行 m2
        ack(vb, mid1)
        d = vb.recv()
        check("da_s1_ack_releases_m2", is_delivery(d) and d["message_id"] == mid2
              and d["sequence"] == 2, str(d))

        # S2 重复 ACK：无回执、无重复投递
        ack(vb, mid2)
        ack(vb, mid2)
        check("da_s2_duplicate_ack_no_side_effect", vb.recv_nothing(0.6))

        # S3 发送后断线（ACK 丢失）：重连重投同 message_id
        r = send_direct(va, aid, bid, "da3-%s" % suffix, "third")
        check("da_s3_accept_m3", r is not None, str(r))
        mid3 = r["message_id"]
        d = vb.recv()
        check("da_s3_delivery_m3", is_delivery(d) and d["message_id"] == mid3, str(d))
        vb.close()
        vb2 = V2Client(host, v2port)
        check("da_s3_relogin_b", login(vb2, bid))
        d = vb2.recv()
        check("da_s3_reconnect_replay_same_mid", is_delivery(d)
              and d["message_id"] == mid3 and d["sequence"] == 3, str(d))
        ack(vb2, mid3)

        # S4 他人 ACK（Bob ACK Carol 的消息）不越权：Carol 的 Delivery 不被终结
        r = send_direct(va, aid, cid, "da4-%s" % suffix, "for carol")
        check("da_s4_accept_m4", r is not None, str(r))
        mid4 = r["message_id"]
        d = vc.recv()
        check("da_s4_delivery_m4", is_delivery(d) and d["message_id"] == mid4, str(d))
        ack(vb2, mid4)  # 他人 ACK：静默忽略
        vc.close()
        vc2 = V2Client(host, v2port)
        check("da_s4_relogin_c", login(vc2, cid))
        d = vc2.recv()
        check("da_s4_foreign_ack_ignored", is_delivery(d) and d["message_id"] == mid4, str(d))
        ack(vc2, mid4)

        # S5 离线投递：登录立即 claim 名下 Pending
        vb2.close()
        r = send_direct(va, aid, bid, "da5-%s" % suffix, "offline test")
        check("da_s5_accept_offline", r is not None, str(r))
        mid5 = r["message_id"]
        vb3 = V2Client(host, v2port)
        check("da_s5_relogin_b", login(vb3, bid))
        d = vb3.recv()
        check("da_s5_offline_delivery_at_login", is_delivery(d) and d["message_id"] == mid5
              and d["sequence"] == 4, str(d))
        ack(vb3, mid5)
        check("da_s5_no_more_deliveries", vb3.recv_nothing(0.6))
    finally:
        for c in (va, vb, vc):
            c.close()

    # ---- v1 场景：legacy implicit-ack（投递即确认，重连不重投）----
    la = V1Client(host, v1port)
    lb = V1Client(host, v1port)
    try:
        la_id = reg(la, "da_l1_%s" % suffix)
        lb_id = reg(lb, "da_l2_%s" % suffix)
        check("da_legacy_reg", la_id > 0 and lb_id > 0)
        check("da_legacy_login_a", login(la, la_id))
        check("da_legacy_login_b", login(lb, lb_id))

        # 在线 legacy：发送者收旧格式回显 errno=0；接收者收投递（含 message_id）
        la.send({"msgid": 6, "id": la_id, "toid": lb_id, "content": "legacy hi"})
        r = la.recv()
        check("da_legacy_echo", r is not None and r.get("errno") == 0
              and r.get("msgid") == 6, str(r))
        d = lb.recv()
        check("da_legacy_delivery", is_delivery(d) and d["msgid"] == 6
              and d["content"] == "legacy hi" and d["id"] == la_id, str(d))
        # 断线重连：implicit-ack 已确认，不重投
        lb.close()
        lb2 = V1Client(host, v1port)
        check("da_legacy_relogin_b", login(lb2, lb_id))
        check("da_legacy_no_replay_after_reconnect", lb2.recv_nothing(0.6))

        # 离线 legacy：登录补投（同样 implicit-ack），再重连不重投
        lb2.close()
        la.send({"msgid": 6, "id": la_id, "toid": lb_id, "content": "legacy offline"})
        r = la.recv()
        check("da_legacy_offline_echo", r is not None and r.get("errno") == 0, str(r))
        lb3 = V1Client(host, v1port)
        check("da_legacy_offline_login", login(lb3, lb_id))
        d = lb3.recv()
        check("da_legacy_offline_delivery_at_login", is_delivery(d)
              and d["content"] == "legacy offline", str(d))
        lb3.close()
        lb4 = V1Client(host, v1port)
        check("da_legacy_offline_login2", login(lb4, lb_id))
        check("da_legacy_offline_no_replay", lb4.recv_nothing(0.6))
    finally:
        for c in (la, lb):
            c.close()

    if FAIL:
        print("DELIVERY_ACK_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("DELIVERY_ACK_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
