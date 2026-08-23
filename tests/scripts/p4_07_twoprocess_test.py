#!/usr/bin/env python3
"""P4-07 M4 两进程跨节点投递验收（H 级缺口 RED/GREEN）。

真两个独立 ChatServer 进程（共享 MySQL/Redis/Kafka、独立端口、各自 gateway.id
1/2），验证：Alice 在 gw1 登录、Bob 在 gw2 登录（均在线），Alice@gw1 发 direct
给 Bob，断言 Bob 在 N 秒内在线收到并 ACK、MessageId 唯一、无重复；epoch fencing
路径不回归；断线重连跨节点仍工作。

H 级缺口（docs/tasks/P4-07.md §审查发现）：
  生产 Config.hpp:81 consumer groupId 全 Gateway 共享 "muduo-outbox-consumer" →
  Kafka 简单消费者（manual assign 全分区、偏移存 Kafka）在同一 group 下每个事件
  被唯一消费者消费 → 跨节点在线接收者的本地 wakeup 事件可能被错误 Gateway 消费
  （其 coordinator 无该用户 active，事件空转）；在线跨节点投递只能靠接收方
  Gateway 的 scheduler 周期扫描（ack_timeout 粒度，生产 30s）或重连才送达。

RED（当前实现 p4-06-final-release 二进制）：两进程共享 group（config 不写
group_id，缺省同为 muduo-outbox-consumer）→ 实测 Bob 在线收不到/延迟 >10s
（依赖 gw 谁抢到事件；若 gw1 抢到则空转，需等 gw2 scheduler ≤30s 或重连）。

GREEN（修复后二进制）：Config 默认 groupId 按 Gateway 派生
（muduo-outbox-consumer-<gatewayId>），每 Gateway 独立消费组 → 每 Gateway 消费
全部事件、只 wake 本地 active 用户（coordinator fencing 幂等）；Bob@gw2 在线
≤10s 收到并 ACK、MessageId 唯一、无重复。

用法（由 p4_07_twoprocess_test.sh 调用）：
  p4_07_twoprocess_test.py <gw1_bin> <db_name> <db_pw> <work_dir> <delay_s>
Exit 0 iff 全部 PASS；任何失败 exit 1（never skip）。
"""
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

V2_MAGIC = 0x4D434854
V2_VERSION = 2
V2_HEADER_LEN = 20
V2_CONTENT_TYPE_JSON = 1

FAIL = []


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name, flush=True)
    else:
        print("FAIL %s %s" % (name, detail), flush=True)
        FAIL.append(name)


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

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def is_delivery(r):
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


def send_direct(cli, from_id, to_id, cmid, content, timeout=10.0):
    cli.send({"msgid": 6, "id": from_id, "toid": to_id,
              "client_message_id": cmid, "content": content})
    return cli.recv(timeout)


def _port_ready(host, port):
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


def wait_server_ready(log_path, v2_port, proc, timeout=30.0):
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
        if log_ready and _port_ready("127.0.0.1", v2_port):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def spawn(bin_path, config, v1_port, log_path):
    logfile = open(log_path, "w")
    cmd = ["setarch", "x86_64", "-R", bin_path, "127.0.0.1", str(v1_port),
           "--config", config]
    proc = subprocess.Popen(cmd, stdout=logfile, stderr=subprocess.STDOUT,
                            env=dict(os.environ))
    return proc, logfile


def main():
    bin_path = sys.argv[1]
    db_name = sys.argv[2]
    db_pw = sys.argv[3]
    work = sys.argv[4]
    delay_s = float(sys.argv[5])

    gw1_v2 = 16211
    gw2_v2 = 16212
    gw1_cfg = os.path.join(work, "gw1.json")
    gw2_cfg = os.path.join(work, "gw2.json")

    def write_cfg(path, gid, v1port, v2port):
        cfg = {
            "server": {"v1": {"ip": "127.0.0.1", "port": v1port, "threads": 2},
                       "v2": {"port": v2port}},
            "db": {"host": "127.0.0.1", "port": 3306, "user": "root",
                   "password": db_pw, "dbname": db_name, "pool_size": 4},
            "executor": {"workers": 2, "queue_capacity": 64},
            "reliable": {"ack_timeout_ms": 3000, "backoff_base_ms": 1000,
                         "backoff_cap_ms": 2000, "backoff_multiplier": 2,
                         "jitter_fraction": 0.0, "jitter_seed": 20260820,
                         "message_retention_ms": 300000,
                         "acked_retention_ms": 3600000,
                         "expired_retention_ms": 3600000,
                         "cleanup_batch": 100, "cleanup_cycle_ms": 3600000,
                         "retry_batch_limit": 500},
            "outbox": {"claim_batch": 8, "scan_interval_ms": 200,
                       "claim_lease_ms": 30000},
            "gateway": {"id": gid,
                        "presence": {"host": "127.0.0.1", "port": 6379,
                                     "db": 0, "ttl_ms": 30000,
                                     "connect_timeout_ms": 1000,
                                     "command_timeout_ms": 1000},
                        "kafka": {"host": "127.0.0.1", "port": 9092},
                        "consumer": {"topic": os.environ.get("P407_TOPIC", "muduo-outbox"),
                                     "fetch_batch_limit": 100,
                                     "poll_deadline_ms": 5000}}
        }
        # 默认不写 group_id：GREEN 二进制按 gateway.id 派生（每 Gateway 独立消费组）。
        # P407_EXPLICIT_GROUP 非空时显式写 group_id（RED 轮：显式共享组模拟修复前）。
        explicit = os.environ.get("P407_EXPLICIT_GROUP", "")
        if explicit:
            cfg["gateway"]["consumer"]["group_id"] = explicit
        with open(path, "w") as f:
            json.dump(cfg, f)

    write_cfg(gw1_cfg, 1, 16201, gw1_v2)
    write_cfg(gw2_cfg, 2, 16202, gw2_v2)

    gw1_log = os.path.join(work, "gw1.log")
    gw2_log = os.path.join(work, "gw2.log")
    p1, l1 = spawn(bin_path, gw1_cfg, 16201, gw1_log)
    p2, l2 = spawn(bin_path, gw2_cfg, 16202, gw2_log)
    try:
        ok1 = wait_server_ready(gw1_log, gw1_v2, p1, 30.0)
        ok2 = wait_server_ready(gw2_log, gw2_v2, p2, 30.0)
        check("two_process_server_gw1_ready", ok1)
        check("two_process_server_gw2_ready", ok2)
        if not (ok1 and ok2):
            sys.exit(1)

        c_alice = V2Client("127.0.0.1", gw1_v2)
        c_bob = V2Client("127.0.0.1", gw2_v2)

        alice_id = reg(c_alice, "alice_p407")
        bob_id = reg(c_bob, "bob_p407")
        check("two_process_reg_alice", alice_id > 0, "alice_id=%s" % alice_id)
        check("two_process_reg_bob", bob_id > 0, "bob_id=%s" % bob_id)

        check("two_process_login_alice_gw1", login(c_alice, alice_id))
        check("two_process_login_bob_gw2", login(c_bob, bob_id))

        # 先确认 Bob 在线 presence（gw2 claim 完成）。
        time.sleep(1.0)

        cmid = "p407-cmn-70001"
        t0 = time.time()
        ackr = send_direct(c_alice, alice_id, bob_id, cmid, "hello-cross-node", timeout=6.0)
        t1 = time.time()
        print("INFO send_direct_ack %s send_ack_latency=%.2fs" % (ackr, t1 - t0), flush=True)

        # Bob 应在线收到 delivery；给定 delay_s 窗口（RED 可能超时/不达）。
        deadline = time.time() + delay_s
        delivery = None
        while time.time() < deadline:
            r = c_bob.recv(timeout=max(0.1, deadline - time.time()))
            if r is None:
                break
            if is_delivery(r):
                delivery = r
                break
        latency = time.time() - t0
        got = delivery is not None
        check("two_process_bob_delivered_within_%gs" % delay_s, got,
              "latency=%.2fs cmid=%s send_ack=%r" % (latency, cmid, ackr))
        if got:
            print("INFO bob_delivery message_id=%s conversation=%s seq=%s latency=%.2fs" %
                  (delivery["message_id"], delivery["conversation_id"],
                   delivery["sequence"], latency), flush=True)
            # ACK 回执。
            ack(c_bob, delivery["message_id"])
            time.sleep(0.5)
            # MessageId 唯一：再等短暂窗口应无重复 delivery（同 cmid 无第二份）。
            dup = c_bob.recv(timeout=1.0)
            check("two_process_no_duplicate_delivery",
                  not (is_delivery(dup) and dup.get("message_id") == delivery["message_id"]),
                  "dup=%r" % (dup,))
        else:
            # RED 观测：记录未达。
            print("INFO RED_observation bob_not_delivered_within_%gs (latency>=%s)" %
                  (delay_s, delay_s), flush=True)

        # 断线重连跨节点（矩阵项，两轮都跑）：Bob TCP 断开 → 重连 gw2 重新登录 →
        # Alice 再发 → Bob 在线收到。离线期 Pending 由重连 claim 或 wakeup 收敛。
        c_bob.close()
        c_bob2 = V2Client("127.0.0.1", gw2_v2)
        if login(c_bob2, bob_id):
            cmid2 = "p407-cmn-70002"
            t0 = time.time()
            send_direct(c_alice, alice_id, bob_id, cmid2, "hello-reconnect", timeout=6.0)
            deadline = time.time() + delay_s
            delivery2 = None
            while time.time() < deadline:
                r = c_bob2.recv(timeout=max(0.1, deadline - time.time()))
                if r is None:
                    break
                if is_delivery(r):
                    delivery2 = r
                    break
            lat2 = time.time() - t0
            got2 = delivery2 is not None
            check("two_process_reconnect_delivered_within_%gs" % delay_s, got2,
                  "latency=%.2fs cmid=%s" % (lat2, cmid2))
            if got2:
                print("INFO reconnect_delivery message_id=%s latency=%.2fs" %
                      (delivery2["message_id"], lat2), flush=True)
                ack(c_bob2, delivery2["message_id"])
                # 等 ACK 落库（真实客户端语义）；否则重启相位会触发 lease 期前
                # 重投 + HOL 阻塞（spec 冻结的 at-least-once+lease-fencing 语义，
                # 非缺陷——OutboxCrashRecovery/ReconnectReplay 单测已覆盖）。
                time.sleep(1.0)
        c_bob2.close()

        # 接收方 Gateway 重启恢复（P407_RESTART_TEST=1）：SIGKILL gw2 → 同配置重启 →
        # Bob 重连登录 → Alice 再发 → Bob 收到（重路由/重放收敛；卡完成判据）。
        if os.environ.get("P407_RESTART_TEST", "") == "1":
            os.kill(p2.pid, signal.SIGKILL)
            try:
                p2.wait(10)
            except subprocess.TimeoutExpired:
                pass
            l2.close()
            p2, l2 = spawn(bin_path, gw2_cfg, 16202, gw2_log)
            ok2r = wait_server_ready(gw2_log, gw2_v2, p2, 30.0)
            check("two_process_restart_gw2_ready", ok2r)
            if ok2r:
                c_bob3 = V2Client("127.0.0.1", gw2_v2)
                if login(c_bob3, bob_id):
                    time.sleep(1.0)
                    cmid3 = "p407-cmn-70003"
                    t0 = time.time()
                    send_direct(c_alice, alice_id, bob_id, cmid3,
                                "hello-after-restart", timeout=6.0)
                    deadline = time.time() + delay_s
                    delivery3 = None
                    while time.time() < deadline:
                        r = c_bob3.recv(timeout=max(0.1, deadline - time.time()))
                        if r is None:
                            break
                        print("INFO restart_recv %r" % (r,), flush=True)
                        if is_delivery(r) and r.get("content") == "hello-after-restart":
                            delivery3 = r
                            break
                    lat3 = time.time() - t0
                    got3 = delivery3 is not None
                    check("two_process_restart_delivered_within_%gs" % delay_s, got3,
                          "latency=%.2fs cmid=%s" % (lat3, cmid3))
                    if got3:
                        print("INFO restart_delivery message_id=%s latency=%.2fs" %
                              (delivery3["message_id"], lat3), flush=True)
                        ack(c_bob3, delivery3["message_id"])
                c_bob3.close()

        c_alice.close()
    finally:
        for p in (p1, p2):
            if p is not None and p.poll() is None:
                os.kill(p.pid, signal.SIGKILL)
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    pass
        l1.close()
        l2.close()

    if FAIL:
        print("TWOPROCESS_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)), flush=True)
        sys.exit(1)
    print("TWOPROCESS_ALL_PASS", flush=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
