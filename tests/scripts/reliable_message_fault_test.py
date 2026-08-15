#!/usr/bin/env python3
"""P3-12 reliable message fault matrix process test against a real ChatServer +
MySQL (dedicated schema chat_p312_$$, independent v1/v2 ports, set up by
reliable_message_fault_test.sh).

The harness owns every ChatServer process (spawn via `setarch x86_64 -R`,
precise pid kill) and injects faults only by drop/delay/kill plus legal SQL any
DBA could run (table lock, lock-wait timeout tuning); it never inspects DB
private implementation.

10 kill points (docs/tasks/P3-12.md 冻结清单)，每点三层断言骨架：
恢复后客户端可见结果 + DB 状态 + 指标可定位（SIGUSR1 METRICS 行的 reliable_*
字段）。RED：reliable_* 尚不存在 → 指标断言失败即合法 RED；DB/客户端断言照常
执行，失败也不 skip（真实 MySQL，无 skip）。

  kp1 accept 回复丢失    A 发送后立即断开（reply 落空）→ 已提交；kill 后重试幂等
  kp2 事务 failpoint     合法 SQL 表锁 + lock-wait-timeout 使 accept 事务失败回滚，
                         无部分行；恢复后同 cmid 重试 fresh
  kp3 commit 后 kill     dormant relay 窗口 kill -9，重启后离线投递恰一次
  kp4 socket RST         接收端 SO_LINGER(1,0) RST，重连重投同 message_id
  kp5 ACK 丢失           ACK 发出后 kill -9，重启至少一次、最终 ACK 收敛
  kp6 DB down/timeout    表锁阻塞 accept（DB 不可用窗口），恢复后消息仍持久、重试幂等
  kp7 relay kill         dormant relay 窗口 kill，重启后 relay 处理事件不重复投递
  kp8 重连新 Session     断线重连新 generation，重领同 message_id
  kp9 过期               短 retention 下 Pending→Expired，可查询不投递
  kp10 优雅/强制退出     SIGTERM 释放 lease / SIGKILL 靠 boot id 重领

Exit code 0 iff RELIABLE_FAULT_ALL_PASS; any failure exits 1 (never skip).
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
V1_PORT = 0
V2_PORT = 0
SERVER_BIN = ""
DB_NAME = ""
DB_PW = ""
CFG_ACTIVE = ""
CFG_DORMANT = ""
CFG_EXPIRE = ""
WORK = ""

# 冻结 reliable_* 指标字段名（P3-12 最小扩展，与 ReliableMessageMetrics 快照对齐）。
RELIABLE_FIELDS = (
    "reliable_accepts", "reliable_duplicates", "reliable_conflicts",
    "reliable_rejected_too_many_recipients",
    "reliable_pending", "reliable_inflight", "reliable_acked", "reliable_expired",
    "reliable_attempts", "reliable_retries", "reliable_legacy_mode",
    "reliable_outbox_lag", "reliable_outbox_poison",
    "reliable_ack_latency_p50_ms", "reliable_ack_latency_p95_ms",
    "reliable_ack_latency_p99_ms", "reliable_oldest_pending_age_ms",
)

DB_STATE = {"Pending": 0, "InFlight": 1, "Acknowledged": 2, "Expired": 3}


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

    def rst_close(self):
        # 强制 RST（SO_LINGER(1,0)），模拟接收端硬断线，服务端走异常关闭路径。
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
        except OSError:
            pass
        self.close()

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


class ServerHandle(object):
    """进程 harness：spawn（setarch -R）+ 就绪探测 + 精确 pid kill/term。"""

    def __init__(self, v1, v2):
        self.v1 = v1
        self.v2 = v2
        self.proc = None
        self.logfile = None

    def _wait_ports_free(self, timeout=15.0):
        # term/kill 后 spawn 前同端口 bind 有竞态（残留监听 / socket 释放中）：
        # 有界轮询 v1/v2 均不可连接后才放行，不固定 sleep 掩盖。
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not _port_ready(HOST, self.v1) and not _port_ready(HOST, self.v2):
                return True
            time.sleep(0.2)
        return False

    def spawn(self, config, log_path):
        # 前一 kp 的末次 spawn（p2/末尾进程）可能仍存活并占用同端口：先回收，
        # 再等端口空闲，最后才拉起（每 kp 单 server 生命周期，spawn 即替换）。
        if self.proc is not None and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                print("FAIL rf_spawn_reap_timeout", flush=True)
                FAIL.append("rf_spawn_reap_timeout")
        if not self._wait_ports_free():
            print("FAIL rf_spawn_ports_busy (%s/%s)" % (self.v1, self.v2), flush=True)
            FAIL.append("rf_spawn_ports_busy")
        if self.logfile is not None:
            self.logfile.close()
        self.logfile = open(log_path, "w")
        cmd = ["setarch", "x86_64", "-R", SERVER_BIN, HOST, str(self.v1),
               "--config", config]
        self.proc = subprocess.Popen(cmd, stdout=self.logfile, stderr=subprocess.STDOUT,
                                     env=dict(os.environ))
        return self.proc

    def ready(self, log_path, timeout=30.0):
        return wait_server_ready(log_path, self.v2, self.proc, timeout)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def kill(self):
        # 精确 pid kill -9（无 sessionClosed 清理、lease 未到期，模拟崩溃）。
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, signal.SIGKILL)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            print("FAIL rf_server_exit_timeout", flush=True)
            FAIL.append("rf_server_exit_timeout")
            return
        if self.logfile is not None:
            self.logfile.close()
            self.logfile = None

    def term(self):
        # 优雅退出（SIGTERM）：ShutdownFlow 应执行 sessionClosed → InFlight 回 Pending。
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, signal.SIGTERM)
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            print("FAIL rf_server_term_timeout", flush=True)
            FAIL.append("rf_server_term_timeout")
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


# ---- SQL 辅助（真实 MySQL 断言，FAIL->exit 1 不 skip）----

def sql_rows(query):
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", DB_NAME,
                        "-e", query], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (query, p.stderr.strip()))
    return [ln.split("\t") for ln in p.stdout.splitlines()]


def sql_run(sql):
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", DB_NAME,
                        "-e", sql], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (sql, p.stderr.strip()))


def sql_run_global(sql):
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-e", sql],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (sql, p.stderr.strip()))


def sql_query_global(sql):
    # 只读 GLOBAL 会话变量查询（无 -D 数据库），返回单值字符串（无换行）。
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-e", sql],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (sql, p.stderr.strip()))
    return p.stdout.strip()


def sql_lock_holder(sql):
    # 合法 SQL 故障注入：独立 mysql 连接持有 LOCK TABLES（kill 该进程即释放）。
    p = subprocess.Popen(["mysql", "-uroot", "-p" + DB_PW, "-D", DB_NAME, "-e", sql],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)  # 握手：给 LOCK TABLES 生效时间（断言本身靠客户端超时/错误，不靠此 sleep）
    return p


def sql_lock_holder_stop(p):
    if p is not None and p.poll() is None:
        p.kill()
        p.wait(5)


def message_id(cmid):
    rows = sql_rows("SELECT id FROM ChatMessage WHERE client_message_id='%s'" % cmid)
    return int(rows[0][0]) if rows else 0


def delivery_state(mid, recipient):
    rows = sql_rows("SELECT state, attempt_count, lease_owner, expires_at "
                    "FROM MessageDelivery WHERE message_id=%s AND recipient_id=%s"
                    % (mid, recipient))
    if not rows:
        return None
    return {"state": int(rows[0][0]), "attempt": int(rows[0][1]),
            "owner": rows[0][2] if len(rows[0]) > 2 else "",
            "expires_at": rows[0][3] if len(rows[0]) > 3 else ""}


def outbox_event(mid):
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


# ---- 指标辅助（SIGUSR1 → METRICS 行 reliable_* 字段；RED 下字段缺失即失败）----

# 采样 settle 语义（P3-12 flake 修复）：SIGUSR1 快照由 loop 线程异步落盘（单信号
# 一 METRICS 行），单次读最后一行可能读到 relay 仍 in-flight 的中间快照（如
# outbox_lag 尚未归零、acked 尚未收敛）。断言前反复发 SIGUSR1 取新快照，0.2s 步进
# 有界轮询直到 pred 满足（期望值已到）再断言；不引入固定 sleep。注意：稳定采样值
# ≠ 终态——kill relay 可能遗留 lease 被占的未处理 OutboxEvent（claim_lease_ms=30s），
# 期间 outbox_lag=1 会稳定保持，须等 lease 到期被当前进程 relay 重领排空后才归零，
# 故不得对"连续两次相同"立即判 FAIL（实测会再现 flake）。
METRIC_SETTLE_STEP = 0.2
METRIC_SETTLE_DEADLINE = 10.0
# outbox_lag==0 断言专用：覆盖被 kill relay 遗留事件的 lease 到期（30s）+ 重领排空
# （≤1 scan，scan_interval 200ms）余量；其余断言用默认 10s。
OUTBOX_LAG_SETTLE_DEADLINE = 45.0


def send_sigusr1(handle):
    if handle.alive():
        os.kill(handle.proc.pid, signal.SIGUSR1)


def read_metrics(log_path):
    # 解析 log 中最后一行含 "METRICS" 的 key=value；无 → 空 dict（字段缺失）。
    try:
        with open(log_path, "r", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return {}
    pairs = {}
    for line in reversed(lines):
        idx = line.find("METRICS")
        if idx >= 0:
            rest = line[idx + len("METRICS"):]
            for tok in rest.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    pairs[k.strip()] = v.strip()
            break
    return pairs


def wait_metrics_log(log_path, timeout=10.0):
    # SIGUSR1 由 loop 线程异步处理，METRICS 行落盘有竞态：不固定 sleep，
    # 有界轮询直到出现含 reliable_* 的 METRICS 行；deadline 未到即失败退出。
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = read_metrics(log_path)
        if any(k.startswith("reliable_") for k in m):
            return m
        time.sleep(0.2)
    print("FAIL metrics_settle no reliable_* METRICS line within %ss: %s"
          % (timeout, log_path), flush=True)
    sys.exit(1)


def assert_metric(name, field, pred, detail="", log_path=None, handle=None, deadline=None):
    # settle 语义（P3-12 flake 修复）：单次读最后一行可能读到 relay 仍 in-flight 的
    # 中间快照（reliable_message_fault_test.py:384-401 读最后一条 METRICS 行）。
    # 首采样直接复用 wait_metrics_log 已确认的行（不再多发 SIGUSR1 避免读中截断行），
    # 之后反复发 SIGUSR1 取新快照，0.2s 步进有界轮询直到 pred 满足（期望值已到）再
    # 断言。单采样缺字段（写行截断/中间快照）不算失败，继续取新快照；deadline 内
    # 字段从未出现才按 RED 判失败。不用"连续两次相同即 FAIL"：被 kill relay 遗留
    # lease 事件会让 outbox_lag 稳定保持 1（claim_lease_ms=30s），须等期望值归零。
    # deadline 按断言语义可调：outbox_lag==0 等须覆盖 lease 到期+重领排空（见
    # OUTBOX_LAG_SETTLE_DEADLINE）。只影响断言采样，不改任何 kp 流程与 DB 断言。
    if log_path is None:
        check(name, False,
              "metric field '%s' missing (RED: not yet wired). %s" % (field, detail))
        return
    if deadline is None:
        deadline = METRIC_SETTLE_DEADLINE
    deadline_abs = time.time() + deadline
    first = True
    seen_field = False
    while time.time() < deadline_abs:
        if handle is not None and not first:
            send_sigusr1(handle)
        first = False
        m = read_metrics(log_path)
        if field not in m:
            time.sleep(METRIC_SETTLE_STEP)
            continue
        seen_field = True
        cur = m[field]
        try:
            ok = pred(cur)
        except Exception as e:  # noqa: BLE001 - 断言骨架吞解析异常并判 FAIL
            ok = False
            detail += " pred-exc:%s" % e
        if ok:
            check(name, True, "metric %s=%s %s" % (field, cur, detail))
            return
        time.sleep(METRIC_SETTLE_STEP)
    if not seen_field:
        check(name, False,
              "metric field '%s' missing (RED: not yet wired). %s" % (field, detail))
        return
    last = read_metrics(log_path).get(field, "<missing>")
    check(name, False,
          "metric %s=%s unmet within %ss %s" % (field, last, deadline, detail))


def assert_metric_snapshot_present(tag, log_path):
    # 先 settle（SIGUSR1 处理异步、METRICS 行可能尚未落盘），再解析快照。
    # 结构化可聚合快照：reliable_* 整组字段应出现（RED 全缺 → 首字段缺失即 FAIL）。
    m = wait_metrics_log(log_path)
    check(tag + "_m_snapshot_fields", RELIABLE_FIELDS[0] in m,
          "expected %s in METRICS line: %s" % (RELIABLE_FIELDS[0], m))
    return m


# ---- 10 kill 点（每点：setup → 注入 → 恢复 → 客户端/DB/指标三层断言）----

def kp1_accept_reply_lost(ctx):
    tag = "kp1_accept_reply_lost"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_ACTIVE, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_a", login(va, aid))
    cmid = tag + "-" + suffix
    content = "reply lost"
    va.send({"msgid": 6, "id": aid, "toid": bid, "client_message_id": cmid,
             "content": content})
    va.rst_close()  # reply 落空（accept 回复丢失）；服务端已提交
    committed = wait_until(10.0, lambda: message_id(cmid) > 0)
    check(tag + "_durable_after_reply_lost", committed, "mid=%s" % message_id(cmid))
    mid = message_id(cmid)
    ctx.srv.kill()
    log2 = os.path.join(WORK, tag + ".p2.log")
    ctx.srv.spawn(CFG_ACTIVE, log2)
    check(tag + "_restart", ctx.srv.ready(log2), log_tail(log2))
    va2 = ctx.new_client(V2_PORT)
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_relogin_a", login(va2, aid))
    r = send_direct(va2, aid, bid, cmid, content)
    check(tag + "_retry_idempotent",
          r is not None and r.get("msgid") == 11 and r.get("duplicate")
          and r.get("message_id") == mid, str(r))
    deliveries = []
    check(tag + "_login_b", login(vb2, bid))
    d = vb2.recv()
    check(tag + "_b_delivery_once",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    if d is not None:
        deliveries.append(d)
    ack(vb2, mid)
    check(tag + "_b_no_redelivery", vb2.recv_nothing(1.0))
    known = {mid}
    seen = [x.get("message_id") for x in deliveries]
    check(tag + "_no_unknown_message_id", set(seen).issubset(known), str(seen))
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_single_message", message_id(cmid) == mid, "mid=%s" % message_id(cmid))
    check(tag + "_db_delivery_acked", acked, str(delivery_state(mid, bid)))
    send_sigusr1(ctx.srv)
    # 重启进程 Recorder 从空重建（ReliableMessageMetrics 契约）：accept/ack 发生在
    # 前一进程，本进程可定位的是幂等重提（duplicates）与恢复投递（attempts）。
    m = assert_metric_snapshot_present(tag, log2)
    assert_metric(tag + "_m_duplicates", "reliable_duplicates", lambda v: int(v) >= 1, str(m), log2, ctx.srv)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 1, str(m), log2, ctx.srv)


def kp2_transaction_failpoint(ctx):
    tag = "kp2_transaction_failpoint"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    # H3：SET GLOBAL 前先读原值；无论 kp2 成败，finally 复位为原值（不写死
    # 31536000——机器可能已配置非默认 lock_wait_timeout）。
    lwt_orig = sql_query_global("SELECT @@GLOBAL.lock_wait_timeout")
    # lock_wait_timeout 是会话变量，池连接在连接时继承当时的 GLOBAL 值：须先
    # SET GLOBAL 再 spawn（连接建在服务器池初始化时），spawn 后再设不生效。
    sql_run_global("SET GLOBAL lock_wait_timeout=2")
    locker = None
    try:
        ctx.srv.spawn(CFG_ACTIVE, log)
        check(tag + "_server", ctx.srv.ready(log), log_tail(log))
        va = ctx.new_client(V2_PORT)
        vb = ctx.new_client(V2_PORT)
        aid = reg(va, tag + "_a_" + suffix)
        bid = reg(vb, tag + "_b_" + suffix)
        check(tag + "_reg", aid > 0 and bid > 0)
        check(tag + "_login_a", login(va, aid))
        cmid = tag + "-" + suffix
        # 事务 failpoint：ChatMessage 元数据锁被占 + lock_wait_timeout=2s → accept
        # 事务首条 INSERT 锁等待超时失败（DependencyBusy），事务整体回滚（无部分行）。
        locker = sql_lock_holder("LOCK TABLES ChatMessage WRITE; SELECT SLEEP(30);")
        r = send_direct(va, aid, bid, cmid, "transaction failpoint", timeout=6.0)
        check(tag + "_accept_rejected",
              r is not None and r.get("msgid") == 13, str(r))
        sql_lock_holder_stop(locker)
        locker = None
        # H3：failpoint 结束立即复位为原值——后续 SELECT（db_no_partial_row）与
        # 后续 kp 不受 lock_wait_timeout=2 影响（表锁释放有竞态，2s 窗口内
        # 新连接 SELECT 会 1205；原值下只是等待锁释放）。
        try:
            sql_run_global("SET GLOBAL lock_wait_timeout=%s" % lwt_orig)
        except RuntimeError:
            pass
        check(tag + "_db_no_partial_row", message_id(cmid) == 0,
              "mid=%s" % message_id(cmid))
        r2 = send_direct(va, aid, bid, cmid, "transaction failpoint")
        check(tag + "_retry_fresh",
              r2 is not None and r2.get("msgid") == 11 and not r2.get("duplicate"), str(r2))
        check(tag + "_db_single_row", r2 is not None and message_id(cmid) == r2["message_id"],
              "mid=%s" % message_id(cmid))
        send_sigusr1(ctx.srv)
        m = assert_metric_snapshot_present(tag, log)
        assert_metric(tag + "_m_accepts", "reliable_accepts", lambda v: int(v) >= 1, str(m), log, ctx.srv)
        assert_metric(tag + "_m_conflicts_zero", "reliable_conflicts", lambda v: int(v) == 0, str(m), log, ctx.srv)
        assert_metric(tag + "_m_duplicates_zero", "reliable_duplicates", lambda v: int(v) == 0, str(m), log, ctx.srv)
    finally:
        sql_lock_holder_stop(locker)
        # H3：复位为原值（DB 不可用时静默——.sh trap 兜底）。
        try:
            sql_run_global("SET GLOBAL lock_wait_timeout=%s" % lwt_orig)
        except RuntimeError:
            pass


def kp3_commit_then_kill(ctx):
    tag = "kp3_commit_then_kill"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_DORMANT, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_a", login(va, aid))
    cmid = tag + "-" + suffix
    content = "commit then kill"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept_committed",
          r is not None and r.get("msgid") == 11 and not r.get("duplicate"), str(r))
    mid = r["message_id"]
    st = outbox_event(mid)
    check(tag + "_event_unprocessed_pre_kill",
          st is not None and st["unprocessed"], str(st))
    ctx.srv.kill()
    log2 = os.path.join(WORK, tag + ".p2.log")
    ctx.srv.spawn(CFG_ACTIVE, log2)
    check(tag + "_restart", ctx.srv.ready(log2), log_tail(log2))
    vb2 = ctx.new_client(V2_PORT)
    processed = wait_until(30.0,
                           lambda: outbox_event(mid) is not None
                           and not outbox_event(mid)["unprocessed"])
    st2 = outbox_event(mid)
    check(tag + "_relay_processed", processed, str(st2))
    check(tag + "_relay_attempt_once", st2 is not None and st2["attempt"] == 1, str(st2))
    check(tag + "_login_b", login(vb2, bid))
    d = vb2.recv()
    check(tag + "_b_delivery_once",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    ack(vb2, mid)
    check(tag + "_b_no_redelivery", vb2.recv_nothing(1.0))
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_acked", acked, str(delivery_state(mid, bid)))
    send_sigusr1(ctx.srv)
    # 重启进程：accept 在旧进程；本进程可定位 relay 恢复投递（attempts）与 outbox 排空。
    m = assert_metric_snapshot_present(tag, log2)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 1, str(m), log2, ctx.srv)
    assert_metric(tag + "_m_outbox_lag", "reliable_outbox_lag", lambda v: int(v) == 0, str(m), log2, ctx.srv,
                  deadline=OUTBOX_LAG_SETTLE_DEADLINE)


def kp4_socket_rst(ctx):
    tag = "kp4_socket_rst"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_ACTIVE, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(va, aid) and login(vb, bid))
    cmid = tag + "-" + suffix
    content = "socket rst"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    mid = r["message_id"]
    d = vb.recv()
    check(tag + "_first_delivery",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    vb.rst_close()  # 接收端 RST：服务端异常关闭 → sessionClosed → InFlight 回 Pending
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_relogin_b", login(vb2, bid))
    d2 = vb2.recv()
    check(tag + "_redelivery_same_message_id",
          is_delivery(d2) and d2["message_id"] == mid, str(d2))
    ack(vb2, mid)
    check(tag + "_no_redelivery_after_ack", vb2.recv_nothing(1.0))
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    st = delivery_state(mid, bid)
    check(tag + "_db_attempt_retried", acked and st is not None and st["attempt"] >= 2, str(st))
    check(tag + "_db_acked", acked, str(st))
    send_sigusr1(ctx.srv)
    m = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 2, str(m), log, ctx.srv)
    assert_metric(tag + "_m_retries", "reliable_retries", lambda v: int(v) >= 1, str(m), log, ctx.srv)
    assert_metric(tag + "_m_ack_latency", "reliable_ack_latency_p50_ms",
                  lambda v: int(v) >= 0, str(m), log, ctx.srv)


def kp5_ack_lost(ctx):
    tag = "kp5_ack_lost"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_ACTIVE, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(va, aid) and login(vb, bid))
    cmid = tag + "-" + suffix
    content = "ack lost"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    mid = r["message_id"]
    d = vb.recv()
    check(tag + "_delivery", is_delivery(d) and d["message_id"] == mid, str(d))
    ack(vb, mid)      # ACK 已发出
    ctx.srv.kill()    # 但可能未处理即崩溃（ACK 丢失）
    log2 = os.path.join(WORK, tag + ".p2.log")
    ctx.srv.spawn(CFG_ACTIVE, log2)
    check(tag + "_restart", ctx.srv.ready(log2), log_tail(log2))
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_relogin_b", login(vb2, bid))
    # at-least-once：重连可能重投同 message_id（ACK 已丢）或不再投（ACK 已持久）；
    # 无论哪支，最终 ACK 收敛、无重复、无未知 message_id。
    d2 = vb2.recv(3.0)
    known = {mid}
    seen = []
    if is_delivery(d2) and d2["message_id"] == mid:
        seen.append(mid)
        ack(vb2, mid)
        check(tag + "_redelivered_same_id", True, str(d2))
    else:
        check(tag + "_ack_persisted_no_redelivery", d2 is None, str(d2))
    check(tag + "_no_unknown_message_id", set(seen).issubset(known), str(seen))
    check(tag + "_no_duplicate_delivery", len(seen) == len(set(seen)), str(seen))
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    st = delivery_state(mid, bid)
    check(tag + "_db_acked", acked, str(st))
    check(tag + "_db_single_row", message_id(cmid) == mid, "mid=%s" % message_id(cmid))
    send_sigusr1(ctx.srv)
    # 重启进程：ACK 转移在旧进程（Recorder 空重建后 BestEffort 跳过），本进程可
    # 定位恢复投递 attempt；ACK 收敛由 DB 断言覆盖。
    m = assert_metric_snapshot_present(tag, log2)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 1, str(m), log2, ctx.srv)
    assert_metric(tag + "_m_ack_latency", "reliable_ack_latency_p50_ms",
                  lambda v: int(v) >= 0, str(m), log2, ctx.srv)


def kp6_db_down_timeout(ctx):
    tag = "kp6_db_down_timeout"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_ACTIVE, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_a", login(va, aid))
    cmid = tag + "-" + suffix
    content = "db down window"
    # DB 不可用窗口：ChatMessage 写锁被占 → accept INSERT 阻塞（客户端超时）；
    # 释放锁后写完成（消息仍持久），同 cmid 重试幂等。
    locker = sql_lock_holder("LOCK TABLES ChatMessage WRITE; SELECT SLEEP(30);")
    r = send_direct(va, aid, bid, cmid, content, timeout=4.0)
    check(tag + "_blocked_in_db_window", r is None, str(r))
    sql_lock_holder_stop(locker)
    durable = wait_until(15.0, lambda: message_id(cmid) > 0)
    check(tag + "_durable_after_recovery", durable, "mid=%s" % message_id(cmid))
    mid = message_id(cmid)
    # 首发 accept 任务在锁释放后完成，会把迟到 accept 回复写进 va 连接；先排干
    # 该迟到回复，再在已登录的 va 上重试（同用户新连接登录会被 UserBusy 拒绝）。
    va.recv(5.0)
    r2 = send_direct(va, aid, bid, cmid, content)
    check(tag + "_retry_idempotent",
          r2 is not None and r2.get("msgid") == 11 and r2.get("duplicate")
          and r2.get("message_id") == mid, str(r2))
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_login_b", login(vb2, bid))
    d = vb2.recv()
    check(tag + "_b_delivery_once",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    ack(vb2, mid)
    check(tag + "_b_no_redelivery", vb2.recv_nothing(1.0))
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_single_row", message_id(cmid) == mid, "mid=%s" % message_id(cmid))
    check(tag + "_db_acked", acked, str(delivery_state(mid, bid)))
    send_sigusr1(ctx.srv)
    m = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_accepts", "reliable_accepts", lambda v: int(v) >= 1, str(m), log, ctx.srv)
    assert_metric(tag + "_m_duplicates", "reliable_duplicates", lambda v: int(v) >= 1, str(m), log, ctx.srv)
    assert_metric(tag + "_m_acked", "reliable_acked", lambda v: int(v) >= 1, str(m), log, ctx.srv)


def kp7_relay_kill(ctx):
    tag = "kp7_relay_kill"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_DORMANT, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(va, aid) and login(vb, bid))
    cmid = tag + "-" + suffix
    content = "relay kill"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    mid = r["message_id"]
    d = vb.recv()  # 在线 claim：B 已收到
    check(tag + "_online_delivery",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    st = outbox_event(mid)
    check(tag + "_event_unprocessed_pre_kill", st is not None and st["unprocessed"], str(st))
    ctx.srv.kill()  # relay 未消费即 kill（事件保持未 processed）
    log2 = os.path.join(WORK, tag + ".p2.log")
    ctx.srv.spawn(CFG_ACTIVE, log2)
    check(tag + "_restart", ctx.srv.ready(log2), log_tail(log2))
    processed = wait_until(30.0,
                           lambda: outbox_event(mid) is not None
                           and not outbox_event(mid)["unprocessed"])
    st2 = outbox_event(mid)
    check(tag + "_relay_processed", processed, str(st2))
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_relogin_b", login(vb2, bid))
    # 崩溃后未 ACK 的 InFlight 由新 boot id 重领（sessionAvailable fencing），B2
    # 收到同一 message_id 的 at-least-once 重投；relay 幂等不新建消息。
    d2 = vb2.recv()
    check(tag + "_redelivery_same_message_id",
          is_delivery(d2) and d2["message_id"] == mid, str(d2))
    ack(vb2, mid)
    check(tag + "_no_redelivery_after_ack", vb2.recv_nothing(1.0))
    check(tag + "_db_attempt_once", st2 is not None and st2["attempt"] == 1, str(st2))
    check(tag + "_db_single_message", message_id(cmid) == mid, "mid=%s" % message_id(cmid))
    send_sigusr1(ctx.srv)
    # 重启进程：accept 在旧进程；本进程可定位重领投递（attempts）与 outbox 排空/无 poison。
    m = assert_metric_snapshot_present(tag, log2)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 1, str(m), log2, ctx.srv)
    assert_metric(tag + "_m_outbox_lag", "reliable_outbox_lag", lambda v: int(v) == 0, str(m), log2, ctx.srv,
                  deadline=OUTBOX_LAG_SETTLE_DEADLINE)
    assert_metric(tag + "_m_outbox_poison", "reliable_outbox_poison", lambda v: int(v) == 0, str(m), log2, ctx.srv)


def kp8_reconnect_new_session(ctx):
    tag = "kp8_reconnect_new_session"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_ACTIVE, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(va, aid) and login(vb, bid))
    cmid = tag + "-" + suffix
    content = "reconnect new session"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    mid = r["message_id"]
    d = vb.recv()
    check(tag + "_first_delivery",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    vb.close()  # 断线（未 ACK）→ sessionClosed → InFlight 回 Pending
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_relogin_new_session", login(vb2, bid))
    d2 = vb2.recv()
    check(tag + "_redelivery_same_message_id",
          is_delivery(d2) and d2["message_id"] == mid, str(d2))
    ack(vb2, mid)
    check(tag + "_no_redelivery_after_ack", vb2.recv_nothing(1.0))
    st = delivery_state(mid, bid)
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_attempt_retried", acked and st is not None and st["attempt"] >= 2, str(st))
    check(tag + "_db_acked", acked, str(st))
    send_sigusr1(ctx.srv)
    m = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 2, str(m), log, ctx.srv)
    assert_metric(tag + "_m_retries", "reliable_retries", lambda v: int(v) >= 1, str(m), log, ctx.srv)
    assert_metric(tag + "_m_inflight", "reliable_inflight", lambda v: int(v) >= 0, str(m), log, ctx.srv)


def kp9_expiration(ctx):
    tag = "kp9_expiration"
    suffix = ctx.suffix
    log = os.path.join(WORK, tag + ".log")
    ctx.srv.spawn(CFG_EXPIRE, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + suffix)
    bid = reg(vb, tag + "_b_" + suffix)
    check(tag + "_reg", aid > 0 and bid > 0)
    check(tag + "_login_a", login(va, aid))
    cmid = tag + "-" + suffix
    content = "expire me"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    mid = r["message_id"]
    expired = wait_until(20.0, lambda: delivery_state(mid, bid) is not None
                         and delivery_state(mid, bid)["state"] == DB_STATE["Expired"])
    st = delivery_state(mid, bid)
    check(tag + "_db_expired", expired, str(st))
    check(tag + "_db_message_queryable", message_id(cmid) == mid, "mid=%s" % message_id(cmid))
    # 每消息守恒：该 Delivery 恰在唯一状态（Expired），无重复计数。
    check(tag + "_db_single_state", st is not None and st["state"] == DB_STATE["Expired"], str(st))
    check(tag + "_login_b", login(vb, bid))
    check(tag + "_no_delivery_after_expiry", vb.recv_nothing(2.0))
    send_sigusr1(ctx.srv)
    m = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_expired", "reliable_expired", lambda v: int(v) >= 1, str(m), log, ctx.srv)
    assert_metric(tag + "_m_oldest_age", "reliable_oldest_pending_age_ms",
                  lambda v: int(v) == -1, str(m), log, ctx.srv)
    # 守恒（快照级）：pending+inflight+acked+expired == 已接受 Delivery 总数，
    # 由 metrics 快照四态字段可定位（精确守恒由单元测试保证，harness 断言可观测性）。
    for field in ("reliable_pending", "reliable_inflight", "reliable_acked",
                  "reliable_expired"):
        if field not in m:
            check(tag + "_m_state_field_" + field, False,
                  "metric field '%s' missing (RED: not yet wired)" % field)
    if all(f in m for f in ("reliable_pending", "reliable_inflight", "reliable_acked",
                            "reliable_expired")):
        four = sum(int(m[f]) for f in ("reliable_pending", "reliable_inflight",
                                       "reliable_acked", "reliable_expired"))
        check(tag + "_m_state_sum_positive", four >= 1, "sum=%s %s" % (four, m))


def kp10_graceful_forced_exit(ctx):
    tag = "kp10_exit"
    suffix = ctx.suffix
    # (a) 优雅退出：SIGTERM → sessionClosed 释放 lease → InFlight 回 Pending。
    log_a = os.path.join(WORK, tag + "_graceful.log")
    ctx.srv.spawn(CFG_ACTIVE, log_a)
    check(tag + "_a_server", ctx.srv.ready(log_a), log_tail(log_a))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_a_" + suffix)
    bid = reg(vb, tag + "_a_b_" + suffix)
    check(tag + "_a_reg", aid > 0 and bid > 0)
    check(tag + "_a_login", login(va, aid) and login(vb, bid))
    cmid_a = tag + "-a-" + suffix
    content_a = "graceful exit"
    r = send_direct(va, aid, bid, cmid_a, content_a)
    check(tag + "_a_accept", r is not None and r.get("msgid") == 11, str(r))
    mid_a = r["message_id"]
    d = vb.recv()
    check(tag + "_a_delivery", is_delivery(d) and d["message_id"] == mid_a, str(d))
    # claimFor 在 sink 准入后才落 InFlight（worker 线程 DB 写），立即读有竞态：
    # 有界轮询等 InFlight 落库。
    inflight_a = wait_until(10.0, lambda: delivery_state(mid_a, bid) is not None
                            and delivery_state(mid_a, bid)["state"] == DB_STATE["InFlight"])
    check(tag + "_a_inflight_pre_exit", inflight_a, str(delivery_state(mid_a, bid)))
    ctx.srv.term()  # 优雅：InFlight 应回 Pending（lease 释放）
    pending = wait_until(15.0, lambda: delivery_state(mid_a, bid) is not None
                         and delivery_state(mid_a, bid)["state"] == DB_STATE["Pending"])
    check(tag + "_a_lease_released", pending, str(delivery_state(mid_a, bid)))
    log_a2 = os.path.join(WORK, tag + "_graceful.p2.log")
    ctx.srv.spawn(CFG_ACTIVE, log_a2)
    check(tag + "_a_restart", ctx.srv.ready(log_a2), log_tail(log_a2))
    vb_a2 = ctx.new_client(V2_PORT)
    check(tag + "_a_relogin_b", login(vb_a2, bid))
    d2 = vb_a2.recv()
    check(tag + "_a_redelivery", is_delivery(d2) and d2["message_id"] == mid_a, str(d2))
    ack(vb_a2, mid_a)
    check(tag + "_a_no_redelivery", vb_a2.recv_nothing(1.0))

    # (b) 强制退出：SIGKILL → 无清理 → lease 未释放，重启靠新 boot id 重领。
    # part (a) 的 va/vb 连接指向已 term 的旧进程：本 part 全部用新客户端。
    log_b = os.path.join(WORK, tag + "_forced.log")
    ctx.srv.spawn(CFG_ACTIVE, log_b)
    check(tag + "_b_server", ctx.srv.ready(log_b), log_tail(log_b))
    va_b = ctx.new_client(V2_PORT)
    vb_b = ctx.new_client(V2_PORT)
    bid2 = reg(vb_b, tag + "_b_b_" + suffix)
    check(tag + "_b_reg", bid2 > 0)
    check(tag + "_b_login", login(va_b, aid) and login(vb_b, bid2))
    cmid_b = tag + "-b-" + suffix
    content_b = "forced exit"
    r = send_direct(va_b, aid, bid2, cmid_b, content_b)
    check(tag + "_b_accept", r is not None and r.get("msgid") == 11, str(r))
    mid_b = r["message_id"]
    d = vb_b.recv()
    check(tag + "_b_delivery", is_delivery(d) and d["message_id"] == mid_b, str(d))
    inflight_b = wait_until(10.0, lambda: delivery_state(mid_b, bid2) is not None
                            and delivery_state(mid_b, bid2)["state"] == DB_STATE["InFlight"])
    check(tag + "_b_inflight_pre_kill", inflight_b, str(delivery_state(mid_b, bid2)))
    ctx.srv.kill()
    st = delivery_state(mid_b, bid2)
    check(tag + "_b_lease_untouched_after_kill", st is not None
          and st["state"] == DB_STATE["InFlight"] and st["owner"], str(st))
    log_b2 = os.path.join(WORK, tag + "_forced.p2.log")
    ctx.srv.spawn(CFG_ACTIVE, log_b2)
    check(tag + "_b_restart", ctx.srv.ready(log_b2), log_tail(log_b2))
    # vb_b2 须在重启后创建：kill 前建的连接指向已死旧进程。
    vb_b2 = ctx.new_client(V2_PORT)
    check(tag + "_b_relogin_b", login(vb_b2, bid2))
    d2 = vb_b2.recv()
    check(tag + "_b_redelivery", is_delivery(d2) and d2["message_id"] == mid_b, str(d2))
    ack(vb_b2, mid_b)
    check(tag + "_b_no_redelivery", vb_b2.recv_nothing(1.0))
    acked = wait_until(10.0, lambda: delivery_state(mid_a, bid) is not None
                       and delivery_state(mid_a, bid)["state"] == DB_STATE["Acknowledged"]
                       and delivery_state(mid_b, bid2) is not None
                       and delivery_state(mid_b, bid2)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_both_acked", acked, str(delivery_state(mid_a, bid)))
    send_sigusr1(ctx.srv)
    # 本进程（log_b2）只重领 mid_b：attempt 已落 1 次且 attemptCount>1 → retry；mid_a
    # 的转移在旧进程，Recorder 空重建后 BestEffort 跳过，不能在本进程断言。
    m = assert_metric_snapshot_present(tag, log_b2)
    assert_metric(tag + "_m_attempts", "reliable_attempts", lambda v: int(v) >= 1, str(m), log_b2, ctx.srv)
    assert_metric(tag + "_m_retries", "reliable_retries", lambda v: int(v) >= 1, str(m), log_b2, ctx.srv)


class Ctx(object):
    def __init__(self, srv, new_client, suffix):
        self.srv = srv
        self.new_client = new_client
        self.suffix = suffix


def main():
    global HOST, V1_PORT, V2_PORT, SERVER_BIN, DB_NAME, DB_PW
    global CFG_ACTIVE, CFG_DORMANT, CFG_EXPIRE, WORK
    HOST = sys.argv[1]
    V1_PORT = int(sys.argv[2])
    V2_PORT = int(sys.argv[3])
    SERVER_BIN = sys.argv[4]
    DB_NAME = sys.argv[5]
    DB_PW = sys.argv[6]
    CFG_ACTIVE = sys.argv[7]
    CFG_DORMANT = sys.argv[8]
    CFG_EXPIRE = sys.argv[9]
    WORK = sys.argv[10]
    suffix = str(int(time.time() * 1000))[-8:]

    srv = ServerHandle(V1_PORT, V2_PORT)
    clients = []

    def new_client(port):
        c = V2Client(HOST, port)
        clients.append(c)
        return c

    ctx = Ctx(srv, new_client, suffix)
    try:
        kp1_accept_reply_lost(ctx)
        kp2_transaction_failpoint(ctx)
        kp3_commit_then_kill(ctx)
        kp4_socket_rst(ctx)
        kp5_ack_lost(ctx)
        kp6_db_down_timeout(ctx)
        kp7_relay_kill(ctx)
        kp8_reconnect_new_session(ctx)
        kp9_expiration(ctx)
        kp10_graceful_forced_exit(ctx)
    finally:
        srv.close()
        for c in clients:
            c.close()

    if FAIL:
        print("RELIABLE_FAULT_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("RELIABLE_FAULT_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
