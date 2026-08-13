#!/usr/bin/env python3
"""P3-09 outbox crash recovery process test against a real ChatServer + MySQL
(dedicated schema chat_p309_$$, set up by outbox_crash_recovery_test.sh).

The harness owns every ChatServer process (spawn via `setarch x86_64 -R`,
precise pid `kill -9`, restart) so no broad pkill can kill an unrelated process;
the .sh owns schema migration, config files, env, log tee and cleanup.  Two
server instances share one schema (disjoint port pairs) for the real two-relay
lease competition; S1 is killed/restarted for the crash-recovery phases.

Scenarios (docs/tasks/P3-09.md 验证 节):
  B. kill/restart: S1 boots with a dormant relay (scan_interval 300000ms) so the
     kill is deterministically *before* the relay ever processes the event:
     A->B offline accept commits the OutboxEvent (unprocessed) -> kill -9 ->
     restart with an active relay -> periodic scan claims+processes the event
     (processed_at set) -> B logs in and receives exactly once.  Unique
     message_id conservation proves the replayed event produces no duplicate
     delivery.  (The sub-ms "processed 标记前 kill" micro-window is not
     process-deterministic; the unit test KillAfterProcessBeforeMarkedProcessed
     Replays covers it with an injected claim.)
  C. poison: one event's payload is corrupted via a legal SQL UPDATE (there is
     no server path to fabricate a poison event, and the OutboxEvent FK forbids
     a dangling aggregate_message_id) -> the relay must not crash, other events
     still get processed, and the poison stays queryable (processed_at IS NULL).
  A. two relays: S1 and S2 (same schema, different ports) run simultaneously
     and race-claim N unprocessed events; the single-row-lock claim UPDATE lets
     exactly one instance win each event (attempt_count==1), all events get
     processed, and recipients receive exactly once.

Any failure exits 1 (never skip).
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
HOST = ""
S1_V1 = 0
S1_V2 = 0
S2_V1 = 0
S2_V2 = 0
SERVER_BIN = ""
DB_NAME = ""
DB_PW = ""
CFG_S1_BOOT = ""
CFG_S1_ACT = ""
CFG_S2_ACT = ""
WORK = ""


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


def wait_server_ready(log_path, v2_port, proc, timeout=30.0):
    # 日志行确认启动无崩溃 + 有界轮询探测 V2 端口可连接（进程 harness 惯例，
    # 无固定 sleep 掩盖断言）。
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
        if log_ready and _port_ready(HOST, v2_port):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


# ---- SQL 辅助（真实 MySQL 断言，FAIL->exit 1 不 skip）----

def sql_rows(query):
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", DB_NAME,
                        "-e", query], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (query, p.stderr.strip()))
    return [ln.split("\t") for ln in p.stdout.splitlines()]


def sql_affected(query):
    # mysql -e 不打印 "rows affected" 状态行；用 ROW_COUNT() 显式读回，稳健。
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", DB_NAME,
                        "-e", query + "; SELECT ROW_COUNT();"], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (query, p.stderr.strip()))
    rows = [ln for ln in p.stdout.splitlines() if ln.strip()]
    return int(rows[0].strip()) if rows else 0


def event_state(mid):
    rows = sql_rows("SELECT attempt_count, (processed_at IS NULL) FROM OutboxEvent "
                    "WHERE aggregate_message_id=%s" % mid)
    if not rows:
        return None
    return {"attempt": int(rows[0][0]), "unprocessed": int(rows[0][1]) == 1}


def wait_until(timeout, fn, pause=0.3):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(pause)
    return False


class ServerHandle(object):
    """进程 harness：spawn（setarch -R）+ 就绪探测 + 精确 pid kill -9。"""

    def __init__(self, v1, v2):
        self.v1 = v1
        self.v2 = v2
        self.proc = None
        self.logfile = None

    def spawn(self, config, log_path):
        if self.logfile is not None:
            self.logfile.close()
        self.logfile = open(log_path, "w")
        cmd = ["setarch", "x86_64", "-R", SERVER_BIN, HOST, str(self.v1), "--config", config]
        self.proc = subprocess.Popen(cmd, stdout=self.logfile, stderr=subprocess.STDOUT,
                                     env=dict(os.environ))
        return self.proc

    def ready(self, log_path, timeout=30.0):
        return wait_server_ready(log_path, self.v2, self.proc, timeout)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def kill(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, signal.SIGKILL)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            print("FAIL oc_server_exit_timeout", flush=True)
            FAIL.append("oc_server_exit_timeout")
            return
        if self.logfile is not None:
            self.logfile.close()
            self.logfile = None

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                pass
        if self.logfile is not None:
            self.logfile.close()
            self.logfile = None


def expect_delivery(cli, name, mid, content, all_deliveries):
    d = cli.recv()
    check(name,
          is_delivery(d) and d["msgid"] == 6 and d["message_id"] == mid
          and d["content"] == content, str(d))
    if d is not None:
        all_deliveries.append(d)
    return d


def main():
    global HOST, S1_V1, S1_V2, S2_V1, S2_V2, SERVER_BIN, DB_NAME, DB_PW
    global CFG_S1_BOOT, CFG_S1_ACT, CFG_S2_ACT, WORK
    HOST = sys.argv[1]
    S1_V1 = int(sys.argv[2])
    S1_V2 = int(sys.argv[3])
    S2_V1 = int(sys.argv[4])
    S2_V2 = int(sys.argv[5])
    SERVER_BIN = sys.argv[6]
    DB_NAME = sys.argv[7]
    DB_PW = sys.argv[8]
    CFG_S1_BOOT = sys.argv[9]
    CFG_S1_ACT = sys.argv[10]
    CFG_S2_ACT = sys.argv[11]
    WORK = sys.argv[12]
    suffix = str(int(time.time() * 1000))[-8:]

    LOG_S1_BOOT = os.path.join(WORK, "s1_boot.log")
    LOG_S1_P2 = os.path.join(WORK, "s1_p2.log")
    LOG_S1_P3 = os.path.join(WORK, "s1_p3.log")
    LOG_S1_A = os.path.join(WORK, "s1_a.log")
    LOG_S2 = os.path.join(WORK, "s2.log")

    s1 = ServerHandle(S1_V1, S1_V2)
    s2 = ServerHandle(S2_V1, S2_V2)
    clients = []
    all_deliveries = []
    known = set()

    def new_client(port):
        c = V2Client(HOST, port)
        clients.append(c)
        return c

    try:
        # ================= Scenario B：dormant relay kill-before-processed + restart =================
        s1.spawn(CFG_S1_BOOT, LOG_S1_BOOT)
        check("oc_boot_server", s1.ready(LOG_S1_BOOT), log_tail(LOG_S1_BOOT))
        va = new_client(S1_V2)
        vb = new_client(S1_V2)
        aid = reg(va, "oc_a_%s" % suffix)
        bid = reg(vb, "oc_b_%s" % suffix)
        check("oc_reg_ab", aid > 0 and bid > 0)
        check("oc_login_a", login(va, aid))

        r = send_direct(va, aid, bid, "oc-1-%s" % suffix, "crash window message")
        check("oc_accept_before_kill", r is not None, str(r))
        mid_1 = r["message_id"]
        known.add(mid_1)
        # dormant relay（300s scan）：事件确定未被消费——kill 发生在 relay 前。
        st = event_state(mid_1)
        check("oc_event_unprocessed_before_kill",
              st is not None and st["attempt"] == 0 and st["unprocessed"], str(st))
        check("oc_s1_alive_pre_kill", s1.alive(), log_tail(LOG_S1_BOOT))
        s1.kill()

        # 重启：active relay 周期扫描 claim+处理（lost wakeup / kill-before-processed 恢复）。
        # 旧连接指向已 kill 进程：本 era 全部重连（死 socket 不跨 kill 复用）。
        s1.spawn(CFG_S1_ACT, LOG_S1_P2)
        check("oc_restart_server", s1.ready(LOG_S1_P2), log_tail(LOG_S1_P2))
        va = new_client(S1_V2)
        vb = new_client(S1_V2)
        processed = wait_until(30.0,
                               lambda: event_state(mid_1) is not None
                               and not event_state(mid_1)["unprocessed"])
        st = event_state(mid_1)
        check("oc_event_processed_after_restart", processed and st is not None, str(st))
        check("oc_event_attempt_once", st is not None and st["attempt"] == 1, str(st))

        check("oc_login_b", login(vb, bid))
        d = expect_delivery(vb, "oc_deliver_b_once", mid_1, "crash window message",
                            all_deliveries)
        ack(vb, mid_1)
        check("oc_b_no_redelivery", vb.recv_nothing(1.0))
        check("oc_s1_alive_post_b", s1.alive(), log_tail(LOG_S1_P2))

        # ================= Scenario C：poison event（合法 SQL UPDATE 破坏 payload）=================
        s1.kill()
        s1.spawn(CFG_S1_BOOT, LOG_S1_P3)
        check("oc_c_boot_server", s1.ready(LOG_S1_P3), log_tail(LOG_S1_P3))
        va = new_client(S1_V2)
        vd = new_client(S1_V2)
        ve = new_client(S1_V2)
        check("oc_login_a2", login(va, aid))
        did = reg(vd, "oc_d_%s" % suffix)
        eid = reg(ve, "oc_e_%s" % suffix)
        check("oc_reg_de", did > 0 and eid > 0)

        r2 = send_direct(va, aid, did, "oc-2-%s" % suffix, "poison target")
        check("oc_accept_m2", r2 is not None, str(r2))
        mid_2 = r2["message_id"]
        r3 = send_direct(va, aid, eid, "oc-3-%s" % suffix, "good neighbor")
        check("oc_accept_m3", r3 is not None, str(r3))
        mid_3 = r3["message_id"]
        known.add(mid_3)

        # poison 只能经合法 SQL 造出（服务器路径无造假入口；OutboxEvent FK 禁止
        # 悬空 aggregate_message_id）——破坏 payload 可解析性，dormant 窗口确定性。
        affected = sql_affected("UPDATE OutboxEvent SET payload='oc-corrupt-%s' "
                                "WHERE aggregate_message_id=%s AND processed_at IS NULL"
                                % (suffix, mid_2))
        check("oc_poison_corrupt_affected", affected == 1, "affected=%s" % affected)
        st2 = event_state(mid_2)
        st3 = event_state(mid_3)
        check("oc_poison_unprocessed_pre_restart",
              st2 is not None and st2["unprocessed"] and st3 is not None
              and st3["unprocessed"], "m2=%s m3=%s" % (st2, st3))
        s1.kill()

        # 重启 active：relay 不崩溃，E3 照常处理，E2 保持 poison（processed_at NULL）。
        s1.spawn(CFG_S1_ACT, LOG_S1_A)
        check("oc_c_restart_server", s1.ready(LOG_S1_A), log_tail(LOG_S1_A))
        good = wait_until(30.0,
                          lambda: event_state(mid_3) is not None
                          and not event_state(mid_3)["unprocessed"])
        st3 = event_state(mid_3)
        check("oc_good_event_processed", good and st3 is not None
              and st3["attempt"] == 1, str(st3))
        touched = wait_until(30.0,
                             lambda: event_state(mid_2) is not None
                             and event_state(mid_2)["attempt"] >= 1)
        st2 = event_state(mid_2)
        check("oc_poison_claimed_not_processed", touched and st2 is not None
              and st2["unprocessed"] and st2["attempt"] >= 1, str(st2))
        check("oc_s1_alive_after_poison", s1.alive(), log_tail(LOG_S1_A))

        # poison 未阻断后续消息投递：E（good 消息接收者）登录收到 M3 一次。
        va = new_client(S1_V2)
        ve = new_client(S1_V2)
        check("oc_login_e", login(ve, eid))
        d = expect_delivery(ve, "oc_deliver_e_once", mid_3, "good neighbor", all_deliveries)
        ack(ve, mid_3)
        check("oc_e_no_redelivery", ve.recv_nothing(1.0))

        # ================= Scenario A：双实例（两 relay）真实竞争同一批事件 =================
        s2.spawn(CFG_S2_ACT, LOG_S2)
        check("oc_s2_server", s2.ready(LOG_S2), log_tail(LOG_S2))
        vc = new_client(S1_V2)
        vc2 = new_client(S2_V2)
        cids = []
        for i in (1, 2, 3, 4):
            cids.append(reg(vc, "oc_c%d_%s" % (i, suffix)))
        check("oc_reg_c1234", all(c > 0 for c in cids))
        check("oc_login_a3", login(va, aid))

        race_mids = []
        for i, cid in enumerate(cids, start=4):
            r = send_direct(va, aid, cid, "oc-%d-%s" % (i, suffix), "race %d" % i)
            check("oc_accept_m%d" % i, r is not None, str(r))
            race_mids.append(r["message_id"])
        # 两 relay（claim_batch=2）同时扫描同一表，单条 UPDATE..LIMIT 行锁串行化：
        # 每个事件恰好一者得 lease（attempt_count==1）。
        all_done = wait_until(30.0,
                              lambda: all(event_state(m) is not None
                                          and not event_state(m)["unprocessed"]
                                          for m in race_mids))
        states = [event_state(m) for m in race_mids]
        check("oc_race_all_processed", all_done, str(states))
        check("oc_race_exactly_one_claim",
              all(st is not None and st["attempt"] == 1 for st in states), str(states))
        check("oc_s1_alive_post_a", s1.alive(), log_tail(LOG_S1_A))
        check("oc_s2_alive_post_a", s2.alive(), log_tail(LOG_S2))

        # 双 relay 竞争下投递去重守恒：C1 在 S1、C2 在 S2 各收一次。
        known.add(race_mids[0])
        known.add(race_mids[1])
        check("oc_login_c1_s1", login(vc, cids[0]))
        d = expect_delivery(vc, "oc_deliver_c1_once", race_mids[0], "race 4", all_deliveries)
        ack(vc, race_mids[0])
        check("oc_c1_no_redelivery", vc.recv_nothing(1.0))
        check("oc_login_c2_s2", login(vc2, cids[1]))
        d = expect_delivery(vc2, "oc_deliver_c2_once", race_mids[1], "race 5", all_deliveries)
        ack(vc2, race_mids[1])
        check("oc_c2_no_redelivery", vc2.recv_nothing(1.0))

        # ---- 最终 SQL：事件守恒 + poison 可查询 + 无未知 message_id ----
        seen = [x.get("message_id") for x in all_deliveries]
        check("oc_no_unknown_message_id", set(seen).issubset(known), str(seen))
        check("oc_deliveries_unique", len(seen) == len(set(seen)) == len(known), str(seen))
        poison_rows = sql_rows("SELECT COUNT(*) FROM OutboxEvent WHERE processed_at IS NULL")
        check("oc_poison_queryable", int(poison_rows[0][0]) == 1, str(poison_rows))
        st2f = event_state(mid_2)
        check("oc_poison_final_unprocessed", st2f is not None and st2f["unprocessed"], str(st2f))
        check("oc_s1_alive_final", s1.alive(), log_tail(LOG_S1_A))
        check("oc_s2_alive_final", s2.alive(), log_tail(LOG_S2))
    finally:
        if s2.proc is not None and s2.proc.poll() is None:
            os.kill(s2.proc.pid, signal.SIGKILL)
            try:
                s2.proc.wait(5)
            except subprocess.TimeoutExpired:
                pass
        s1.close()
        s2.close()
        for c in clients:
            c.close()

    if FAIL:
        print("OUTBOX_CRASH_RECOVERY_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("OUTBOX_CRASH_RECOVERY_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
