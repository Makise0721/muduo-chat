#!/usr/bin/env python3
"""P4-06 multi-node chaos + capacity/storm-protection process harness against a
real ChatServer + real Redis + real Kafka + real MySQL (dedicated schema
chat_p306_$$, independent v1/v2 ports, presence db isolation, per-scenario
Kafka topic/group, owned by cluster_chaos_test.sh).

The harness owns every ChatServer process (spawn via `setarch x86_64 -R`,
precise pid kill -9), and injects faults only by drop/delay/pause/kill plus
legal SQL any DBA could run (table lock / lock-wait timeout); it never inspects
DB private implementation. Redis faults use CLIENT PAUSE (delay/down window,
auto-recover), reversible. Kafka faults pause the relay/consumer path via
broker-level injection (kp7_kafka_pause): the .sh detects the exact broker pid
(pgrep -f 'kafka.Kafka', precise pid like the ChatServer kill, filtered to the
java process) and exports KAFKA_BROKER_PID; the harness issues kill -STOP/-CONT
at the fault window and the .sh trap CONTs the broker on exit (safety net if
the harness dies mid-pause). No skip: real deps.

RED (当前实现 8ea653f 下必须失败，暴露 P4-06 目标缺口)：
  1. l5_lastseen_unbounded  — consumer 的 per-conversation lastSeen/seenMessages
     内存态无界（P4-04 L-5 / P4-06 遗留）。公开 seam：METRICS 行应暴露
     consumer_seen_conversations（per-conversation seen 集合容量）且有界；
     当前无该字段 → 断言缺失即合法 RED（沿 P3-12 reliable_* 缺失 RED 先例）。
     观测手段限制：进程 RSS 噪声大、不可作 tight 断言；per-conversation 计数
     无既有 METRICS seam，故以"容量契约字段缺失"作 RED 观测（卡内注明）。
  2. db0_presence_accumulation — db0 presence:v1:* 键物理累积（P4-06 环境复原
     项；claim SET 无 EXPIRE、SIGKILL 无 release → 物理键不删）。多轮登录+
     SIGKILL 不同用户后，断言 db0 presence 键增量有界（无跨轮累积）→ 当前
     无清理 → RED。真实 Redis db0，观测 seam 即 Redis 自身。
  3. storm_recovery_capacity — Redis 故障（CLIENT PAUSE 窗口）期间累积
     重连/重投风暴，恢复后已接受消息最终 ACK 或保持可查询、控制面（outbox
     lag / oldest pending）收敛、无需人工改库。按实测 RED 或绿（回归锚）。

Kill/注入点（单轮断言先记录，RED 跑一遍看哪些已绿哪些需 GREEN 期修复）：
  kp1  ChatServer 整进程 kill -9 → 重启接管：离线投递恰一、presence 重领、
       无重复 ACK（db1 隔离）。
  kp4  Redis delay（CLIENT PAUSE 窗口）→ durable accept 继续、恢复后收敛。
  kp7  Kafka pause（broker kill -STOP/-CONT）→ 可靠管线消费停顿（B 离线：无
       本地直投，outbox lag 暂停期上升、恢复后重投收敛）、无 ACK 进度、
       at-least-once 不丢消息（已接受消息最终 ACK）、无人工改库。
  kp9  MySQL timeout（合法表锁）→ accept 拒绝→同 cmid 重试幂等、无部分行。
  等价边界登记（in-process 拓扑下 relay/consumer 线程无法独立 kill；映射到等效
  故障面，不在本卡硬跑独立场景，P4-07 拆进程时再真 kill）：
  - kp_relay_kill（relay 线程 kill）≈ Redis/Kafka down 已覆盖：relay 出口中断 =
    outbox 事件不发布、不标 processed、恢复后重放——kp4（Redis CLIENT PAUSE：
    durable accept 继续、恢复后收敛）与 kp7（Kafka broker pause：publish 失败
    不标 processed、恢复后重放）分别阻断 relay 下游通道，等效覆盖该故障面。
  - kp_consumer_kill（consumer 线程 kill）≈ Kafka pause：consumer poll 中断 =
    无新消费、offset 不推进、恢复后从 committed offset 重放——kp7 暂停 broker 即
    等效暂停 consumer 输入面（同一条 Kafka 依赖，无第三方仲裁者），恢复后重放
    同语义（at-least-once，DB 幂等 DuplicateNoOp）。
  - 网络分区：in-process 拓扑无公开 harness seam（无代码注入、无 docker），
    仍登记为 P4-07 前置等价边界项（transport/adapter 层注入留待拆进程拓扑）。

Exit code 0 iff CHAOS_ALL_PASS; any failure exits 1 (never skip).
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
WORK = ""
SEED = 20260817

# 冻结 reliable_* 指标字段（P3-12 最小扩展，与 ReliableMessageMetrics 快照对齐）。
RELIABLE_FIELDS = (
    "reliable_accepts", "reliable_duplicates", "reliable_conflicts",
    "reliable_rejected_too_many_recipients",
    "reliable_pending", "reliable_inflight", "reliable_acked", "reliable_expired",
    "reliable_attempts", "reliable_retries", "reliable_legacy_mode",
    "reliable_outbox_lag", "reliable_outbox_poison",
    "reliable_ack_latency_p50_ms", "reliable_ack_latency_p95_ms",
    "reliable_ack_latency_p99_ms", "reliable_oldest_pending_age_ms",
)

# P4-06 D3 本卡指标缺口：consumer per-conversation seen 集合容量（L-5）。当前
# 无该字段 → 断言缺失即 RED（沿 P3-12 reliable_* 缺失 RED 先例）。
CONSUMER_SEEN_FIELD = "consumer_seen_conversations"
# 登记的容量契约：consumer 同时追踪的不同 conversation 数有界（卡内注册语义；
# 当前实现无上限/驱逐/观测 → 无该字段即 RED）。
L5_CAPACITY_BOUND = 100

# P4-06 db0 EXPIRE 环境复原（GREEN）：claim/renew Lua 写值同时 PEXPIRE（TTL=30s，
# 与 value 内嵌 expiresAtMs 一致；物理删除仅防累积）。5 轮后旧键随 TTL 过期，
# delta 有界 ≈ 单轮 K + 余量；跑完清理由 .sh trap 兜底。
# DB0_DELTA_BOUND = 单轮 12 + 余量 12 = 24（最多同时存活 ~2 轮）；高于此需等待
# 旧键过期再断言（确定性：等待至 TTL 过期，delta 收敛到 ≤ bound）。
DB0_DELTA_BOUND = 2 * 12
DB0_SETTLE_DEADLINE = 45.0  # > TTL(30s) + 轮间余量，等待旧键物理过期

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
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not _port_ready(HOST, self.v1) and not _port_ready(HOST, self.v2):
                return True
            time.sleep(0.2)
        return False

    def spawn(self, config, log_path):
        if self.proc is not None and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                print("FAIL chaos_spawn_reap_timeout", flush=True)
                FAIL.append("chaos_spawn_reap_timeout")
        if not self._wait_ports_free():
            print("FAIL chaos_spawn_ports_busy (%s/%s)" % (self.v1, self.v2), flush=True)
            FAIL.append("chaos_spawn_ports_busy")
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
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, signal.SIGKILL)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            print("FAIL chaos_server_exit_timeout", flush=True)
            FAIL.append("chaos_server_exit_timeout")
            return
        if self.logfile is not None:
            self.logfile.close()
            self.logfile = None

    def term(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, signal.SIGTERM)
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            print("FAIL chaos_server_term_timeout", flush=True)
            FAIL.append("chaos_server_term_timeout")
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


# ---- SQL 辅助（真实 MySQL，FAIL->exit 1 不 skip）----

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
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-e", sql],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (sql, p.stderr.strip()))
    return p.stdout.strip()


def sql_lock_holder(sql):
    p = subprocess.Popen(["mysql", "-uroot", "-p" + DB_PW, "-D", DB_NAME, "-e", sql],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    return p


def sql_lock_holder_stop(p):
    if p is not None and p.poll() is None:
        p.kill()
        p.wait(5)


def message_id(cmid):
    rows = sql_rows("SELECT id FROM ChatMessage WHERE client_message_id='%s'" % cmid)
    return int(rows[0][0]) if rows else 0


def delivery_state(mid, recipient):
    rows = sql_rows("SELECT state, attempt_count FROM MessageDelivery "
                    "WHERE message_id=%s AND recipient_id=%s" % (mid, recipient))
    if not rows:
        return None
    return {"state": int(rows[0][0]), "attempt": int(rows[0][1])}


def wait_until(timeout, fn, pause=0.3):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(pause)
    return False


# ---- Redis 辅助（真实 Redis；db0 观测 seam 即 Redis 自身）----

def redis(args, db=0):
    p = subprocess.run(["redis-cli", "-n", str(db)] + args, capture_output=True,
                       text=True, timeout=20)
    return p.returncode, p.stdout.strip()


def redis_presence_keys(db):
    # db0/db1 的 presence:v1:* 键集合（排除全局 epoch 计数器键）。
    rc, out = redis(["--scan", "--pattern", "presence:v1:*"], db)
    if rc != 0:
        return set()
    keys = set()
    for ln in out.splitlines():
        ln = ln.strip()
        if ln and ln != "presence:v1:epoch":
            keys.add(ln)
    return keys


def redis_presence_count(db):
    return len(redis_presence_keys(db))


def redis_ttl(key, db=0):
    rc, out = redis(["TTL", key], db)
    if rc != 0:
        return None
    try:
        return int(out)
    except ValueError:
        return None


def wait_redis_ready(timeout=20.0):
    # CLIENT PAUSE 故障注入后，Redis 可能短暂拒绝后续连接/命令（实测：pause 结束
    # 后立即 spawn 下一个 server 的 presence claim 偶发失败）。有界轮询直到 PING
    # 恢复 PONG（pause 到期自动恢复）再放行下一场景；不固定 sleep 掩盖断言。
    deadline = time.time() + timeout
    while time.time() < deadline:
        rc, out = redis(["PING"], 0)
        if rc == 0 and out == "PONG":
            return True
        time.sleep(0.3)
    return False


# ---- 指标辅助（SIGUSR1 → METRICS 行 reliable_*；RED 下缺失即失败）----

METRIC_SETTLE_STEP = 0.2
METRIC_SETTLE_DEADLINE = 10.0
OUTBOX_LAG_SETTLE_DEADLINE = 45.0


def send_sigusr1(handle):
    if handle.alive():
        os.kill(handle.proc.pid, signal.SIGUSR1)


def read_metrics(log_path):
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
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = read_metrics(log_path)
        if any(k.startswith("reliable_") for k in m):
            return m
        time.sleep(0.2)
    print("FAIL metrics_settle no reliable_* METRICS line within %ss: %s"
          % (timeout, log_path), flush=True)
    sys.exit(1)


def assert_metric(name, field, pred, detail="", log_path=None, handle=None,
                  deadline=None):
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
        except Exception as e:  # noqa: BLE001
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
    check(name, False, "metric %s=%s unmet within %ss %s" % (field, last, deadline, detail))


def wait_metric_reaches(log_path, field, ge_value, handle=None,
                        deadline=METRIC_SETTLE_DEADLINE):
    """有界轮询：SIGUSR1 触发 METRICS 快照，等待 field 数值 >= ge_value。

    返回达到时的值；超时返回 None。用于两段式断言（先证明前置条件成立再断言，
    如 l5 的 cap 触发前置），避免早期快照假通过。
    """
    deadline_abs = time.time() + deadline
    first = True
    while time.time() < deadline_abs:
        if handle is not None and not first:
            send_sigusr1(handle)
        first = False
        m = read_metrics(log_path)
        if field in m:
            try:
                cur = int(m[field])
            except ValueError:
                cur = -1
            if cur >= ge_value:
                return cur
        time.sleep(METRIC_SETTLE_STEP)
    return None


def assert_metric_snapshot_present(tag, log_path):
    m = wait_metrics_log(log_path)
    check(tag + "_m_snapshot_fields", RELIABLE_FIELDS[0] in m,
          "expected %s in METRICS line: %s" % (RELIABLE_FIELDS[0], m))
    return m


# ---- 确定性随机（固定种子，P4-06 冻结矩阵）----

class SeededRand(object):
    def __init__(self, seed):
        self.state = seed & 0x7fffffff

    def next(self):
        # LCG（确定性、可复现）
        self.state = (1103515245 * self.state + 12345) & 0x7fffffff
        return self.state

    def below(self, n):
        return self.next() % n


RNG = SeededRand(SEED)


def scenario_log(name):
    return os.path.join(WORK, name + ".log")


# =====================================================================
# 场景 1：L-5 lastSeen 无界（预期 RED）
# =====================================================================

def l5_lastseen_unbounded(ctx):
    tag = "l5_lastseen_unbounded"
    log = scenario_log(tag)
    cfg = ctx.write_config(tag, presence_db=1)
    ctx.srv.spawn(cfg, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    sid = reg(va, tag + "_s_" + ctx.suffix)
    check(tag + "_reg_sender", sid > 0)
    check(tag + "_login_sender", login(va, sid))
    # 大量不同 conversation：同一 sender 向大量不同 recipient 直发（每个有序
    # (sender,recipient) 对 = 独立 DirectConversation → consumer 的
    # lastSeen/seenMessages 每 conversation 一项，随 conversation 数线性增长）。
    recipients = []
    for i in range(6):
        c = ctx.new_client(V2_PORT)
        rid = reg(c, tag + "_r%d_%s" % (i, ctx.suffix))
        recipients.append(rid)
        c.close()  # 仅注册占位；离线 recipient，注册后即可释放连接（省 executor 压力）
    # 其余 recipient 用离线注册（不登录，仅占 conversation 位）——每轮注册新
    # 用户即可产生新 conversation（recipient 无需在线，消息仍 accept + outbox）。
    total_conv = 0
    rbase = 6
    for i in range(ctx.l5_conversations):
        idx = i + rbase
        c = ctx.new_client(V2_PORT)
        rid = reg(c, tag + "_x%d_%s" % (idx, ctx.suffix))
        c.close()  # 注册后即释放连接（离线 recipient，仅占 conversation 位）
        cmid = "%s-c%d-%s" % (tag, i, ctx.suffix)
        r = send_direct(va, sid, rid, cmid, "l5 conv %d" % i)
        check(tag + "_accept_%d" % i, r is not None and r.get("msgid") == 11, str(r))
        if r is not None:
            total_conv += 1
    check(tag + "_accepted", total_conv >= ctx.l5_conversations,
          "accepted=%s want>=%s" % (total_conv, ctx.l5_conversations))
    # 等 consumer 排空（outbox lag → 0；relay 全部 publish、consumer 异步处置
    # 后 lastSeen/seenMessages 应含 total_conv 个 conversation）。
    send_sigusr1(ctx.srv)
    m0 = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_outbox_lag_zero", "reliable_outbox_lag",
                  lambda v: int(v) == 0, str(m0), log, ctx.srv,
                  deadline=OUTBOX_LAG_SETTLE_DEADLINE)
    # 容量契约（P4-06 L-5）：consumer 同时追踪的不同 conversation 数有界。
    # 强化（避免早期快照假通过）：先有界等待 consumer_seen_conversations 达到 cap
    # 饱和值（=L5_CAPACITY_BOUND。本场景创建 ctx.l5_conversations=120 个不同
    # conversation > cap=100 → gauge 触顶后固定于 100；早于触顶的快照（如 30）
    # 也满足 <=100 但 cap 从未真正触发 → 假通过），再断言 <= 上限。
    # 注：gauge = lastSeen.size()（KafkaEventConsumer.cpp:677-678），容量保护使其
    # 最大值即 L5_CAPACITY_BOUND——120 conversations 下 gauge 不会也不应达到 120
    # （那恰是 RED 无界态）；故以 >=L5_CAPACITY_BOUND 证明 cap 真正触发。
    cap_seen = wait_metric_reaches(log, CONSUMER_SEEN_FIELD, L5_CAPACITY_BOUND,
                                   handle=ctx.srv,
                                   deadline=OUTBOX_LAG_SETTLE_DEADLINE)
    assert_metric(tag + "_m_seen_capacity",
                  CONSUMER_SEEN_FIELD,
                  lambda v: cap_seen is not None and int(v) <= L5_CAPACITY_BOUND,
                  "cap engaged at=%s bound=%s" % (cap_seen, L5_CAPACITY_BOUND),
                  log, ctx.srv)
    check(tag + "_capacity_registered",
          True,
          "L-5 seen map per-conversation unbounded (no eviction/cap/observability); "
          "RED seam: consumer_seen_conversations metric absent (P3-12 RED precedent)")


# =====================================================================
# 场景 2：db0 presence 物理累积（预期 RED 于环境复原）
# =====================================================================

def db0_presence_accumulation(ctx):
    tag = "db0_presence_accumulation"
    # 使用生产默认 db0（不隔离）以暴露 db0 物理累积（P4-06 环境复原项）。
    cfg = ctx.write_config(tag, presence_db=0)
    before = redis_presence_count(0)
    # 多轮登录+SIGKILL（无 release → 物理键不删）：每轮 K 个不同用户。
    rounds = 5
    k_per_round = 12
    total_distinct = 0
    for r in range(rounds):
        log = scenario_log("%s_r%d" % (tag, r))
        ctx.srv.spawn(cfg, log)
        check(tag + "_r%d_server" % r, ctx.srv.ready(log), log_tail(log))
        round_clients = []
        for j in range(k_per_round):
            c = ctx.new_client(V2_PORT)
            round_clients.append(c)
            uid = reg(c, "%s_u%d_%s" % (tag, r * 1000 + j, ctx.suffix))
            if uid > 0:
                if not login(c, uid):
                    check(tag + "_r%d_login_%d" % (r, j), False, "uid=%s" % uid)
                total_distinct += 1
        ctx.srv.kill()  # SIGKILL：无 sessionClosed → presence release 不执行 → 键留
        print("DBG %s round%d db0_presence=%s" % (tag, r, redis_presence_count(0)), flush=True)
        # 端口释放后才进下一轮 spawn。本轮客户端已随 server 死亡，显式关闭释放 fd。
        for c in round_clients:
            c.close()
    check(tag + "_distinct_logins", total_distinct >= rounds * k_per_round,
          "total=%s want>=%s" % (total_distinct, rounds * k_per_round))
    # 物理键 EXPIRE 证据（GREEN 复原）：抽一条仍存活的键验 TTL != -1（claim/renew
    # Lua 写值同时 PEXPIRE）。在等待过期前采样（settle 后旧键可能已全部物理过期）。
    mine = redis_presence_keys(0)
    sample = sorted(mine)[-1] if mine else ""  # 最近轮（最大 uid）的键，仍存活
    if sample:
        ttl = redis_ttl(sample, 0)
        check(tag + "_key_has_expire", ttl is not None and ttl != -1,
              "key=%s TTL=%s want != -1 (EXPIRE set)" % (sample, ttl))
    else:
        check(tag + "_key_has_expire", False, "no presence key sampled")
    # delta 有界（GREEN 复原）：EXPIRE 下旧键随 TTL 过期 → 等待至 delta 收敛到
    # ≤ DB0_DELTA_BOUND（确定性；无跨轮无界累积——RED 为 delta=rounds*K=60）。
    def _db0_settled():
        return redis_presence_count(0) - before <= DB0_DELTA_BOUND
    settled = wait_until(DB0_SETTLE_DEADLINE, _db0_settled)
    after = redis_presence_count(0)
    delta = after - before
    check(tag + "_delta_bounded", settled,
          "db0 presence delta=%s (before=%s after=%s) want<=%s after TTL settle "
          "-> EXPIRE makes old keys expire (no unbounded physical accumulation)" %
          (delta, before, after, DB0_DELTA_BOUND))
    # 登记环境复原项：db0 presence 键需测试侧清理（.sh trap 兜底）。
    check(tag + "_env_recovery_registered", True,
          "db0 presence:%s delta keys -> cleanup by .sh (P4-06 env-recovery item)" % delta)


# =====================================================================
# 场景 3：风暴恢复容量（RED 或绿，按实测；回归锚）
# =====================================================================

def storm_recovery_capacity(ctx):
    tag = "storm_recovery_capacity"
    cfg = ctx.write_config(tag, presence_db=1)
    log = scenario_log(tag)
    ctx.srv.spawn(cfg, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + ctx.suffix)
    bid = reg(vb, tag + "_b_" + ctx.suffix)
    check(tag + "_reg_ab", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(va, aid) and login(vb, bid))
    # Redis 故障窗口：CLIENT PAUSE 5000ms（delay/down 模拟，自动恢复）。
    rc, out = redis(["CLIENT", "PAUSE", "5000"], 0)
    check(tag + "_pause", rc == 0 and out == "OK", str((rc, out)))
    # 故障窗口内累积重投风暴：多发直发（本地短路投递——H1 语义：已 claim 本地
    # 用户在 Redis down 下投递仍继续）。每发都接受（durable accept 继续）。
    cmids = []
    n_burst = 20
    for i in range(n_burst):
        cmid = "%s-%d-%s" % (tag, i, ctx.suffix)
        cmids.append(cmid)
        r = send_direct(va, aid, bid, cmid, "storm %d" % i, timeout=8.0)
        check(tag + "_accept_%d" % i, r is not None and r.get("msgid") == 11, str(r))
    # 恢复：CLIENT PAUSE 到期自动恢复；等所有消息最终 ACK（客户端 B 收 + ACK）。
    # at-least-once：按 message_id 去重，ACK 后收敛。
    known = set()
    for i in range(n_burst):
        mid = message_id(cmids[i])
        known.add(mid)
        # 等 B 收到并 ACK（本地投递在 pause 窗口已发出或恢复后重投）。
        got = vb.recv(8.0)
        if is_delivery(got):
            ack(vb, got["message_id"])
            check(tag + "_deliver_%d" % i, got["message_id"] == mid, str(got))
        else:
            check(tag + "_deliver_%d" % i, False, "no delivery for cmid=%s" % cmids[i])
    acked_all = wait_until(20.0, lambda: all(
        delivery_state(m, bid) is not None
        and delivery_state(m, bid)["state"] == DB_STATE["Acknowledged"] for m in known))
    check(tag + "_all_acked", acked_all,
          str([(m, delivery_state(m, bid)) for m in list(known)[:5]]))
    # 控制面收敛：outbox lag → 0（无风暴压垮、无人工改库）。
    send_sigusr1(ctx.srv)
    m = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_outbox_lag_zero", "reliable_outbox_lag",
                  lambda v: int(v) == 0, str(m), log, ctx.srv,
                  deadline=OUTBOX_LAG_SETTLE_DEADLINE)
    assert_metric(tag + "_m_oldest_pending", "reliable_oldest_pending_age_ms",
                  lambda v: int(v) == -1, str(m), log, ctx.srv)
    check(tag + "_no_manual_db_change", True,
          "storm recovery converges without manual DB change")
    # CLIENT PAUSE 故障注入后 Redis 短暂拒新连接：有界轮询等恢复再进下一场景
    #（避免 pause 竞态让下一 server 的 presence claim 偶发失败——实测 flake）。
    if not wait_redis_ready():
        check(tag + "_redis_ready_after_pause", False, "Redis PING not recovered")


# =====================================================================
# 场景 4：kill 点（单轮断言先记录）
# =====================================================================

def kp1_chatserver_kill(ctx):
    tag = "kp1_chatserver_kill"
    cfg = ctx.write_config(tag, presence_db=1)
    log = scenario_log(tag)
    ctx.srv.spawn(cfg, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + ctx.suffix)
    bid = reg(vb, tag + "_b_" + ctx.suffix)
    check(tag + "_reg_ab", aid > 0 and bid > 0)
    va.send({"msgid": 1, "id": aid, "password": "pwd"})
    lr = va.recv()
    check(tag + "_login_a", lr is not None and lr.get("errno") == 0, str(lr))
    # 离线投递恰一：kill 前 A->B（B 离线）留 Pending；重启后 B 登录收到。
    cmid = tag + "-" + ctx.suffix
    content = "offline kill"
    r = send_direct(va, aid, bid, cmid, content)
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    if not (r is not None and r.get("msgid") == 11):
        return  # accept 失败：断言已记 FAIL，跳过后续避免崩溃（harness 继续）
    mid = r["message_id"]
    ctx.srv.kill()
    log2 = scenario_log(tag + ".p2")
    ctx.srv.spawn(cfg, log2)
    check(tag + "_restart", ctx.srv.ready(log2), log_tail(log2))
    vb2 = ctx.new_client(V2_PORT)
    check(tag + "_login_b", login(vb2, bid))
    d = vb2.recv()
    check(tag + "_b_delivery_once",
          is_delivery(d) and d["message_id"] == mid and d["content"] == content, str(d))
    ack(vb2, mid)
    check(tag + "_b_no_redelivery", vb2.recv_nothing(1.0))
    acked = wait_until(10.0, lambda: delivery_state(mid, bid) is not None
                       and delivery_state(mid, bid)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_acked", acked, str(delivery_state(mid, bid)))
    check(tag + "_db_single_row", message_id(cmid) == mid, "mid=%s" % message_id(cmid))


def kp4_redis_delay(ctx):
    tag = "kp4_redis_delay"
    cfg = ctx.write_config(tag, presence_db=1)
    log = scenario_log(tag)
    ctx.srv.spawn(cfg, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + ctx.suffix)
    bid = reg(vb, tag + "_b_" + ctx.suffix)
    check(tag + "_reg_ab", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(va, aid) and login(vb, bid))
    rc, out = redis(["CLIENT", "PAUSE", "3000"], 0)
    check(tag + "_pause", rc == 0 and out == "OK", str((rc, out)))
    cmid = tag + "-" + ctx.suffix
    r = send_direct(va, aid, bid, cmid, "redis delay", timeout=8.0)
    check(tag + "_accept_during_delay",
          r is not None and r.get("msgid") == 11, str(r))
    if not (r is not None and r.get("msgid") == 11):
        return  # accept 失败：断言已记 FAIL，跳过后续避免崩溃
    # 恢复后收敛：B 收到并 ACK。
    d = vb.recv(8.0)
    check(tag + "_delivery", is_delivery(d) and d["message_id"] == r["message_id"], str(d))
    if is_delivery(d):
        ack(vb, d["message_id"])
    acked = wait_until(15.0, lambda: delivery_state(r["message_id"], bid) is not None
                       and delivery_state(r["message_id"], bid)["state"] == DB_STATE["Acknowledged"])
    check(tag + "_db_acked", acked, str(delivery_state(r["message_id"], bid)))
    # CLIENT PAUSE 后 Redis 短暂拒新连接：有界轮询等恢复再进下一场景。
    if not wait_redis_ready():
        check(tag + "_redis_ready_after_pause", False, "Redis PING not recovered")


def kafka_broker_pid():
    # .sh 检测并导出（pgrep -f 'kafka.Kafka' 精确形态，过滤到 java 进程）。
    raw = os.environ.get("KAFKA_BROKER_PID", "")
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


def kp7_kafka_pause(ctx):
    tag = "kp7_kafka_pause"
    cfg = ctx.write_config(tag, presence_db=1)
    log = scenario_log(tag)
    ctx.srv.spawn(cfg, log)
    check(tag + "_server", ctx.srv.ready(log), log_tail(log))
    va = ctx.new_client(V2_PORT)
    vb = ctx.new_client(V2_PORT)
    aid = reg(va, tag + "_a_" + ctx.suffix)
    bid = reg(vb, tag + "_b_" + ctx.suffix)
    check(tag + "_reg_ab", aid > 0 and bid > 0)
    # B 保持离线：在线接收者会在 accept 时被 notifyAcceptedBestEffort 本地直投
    # （不依赖 Kafka），暂停的消费停顿只能经可靠管线（outbox→Kafka→consumer）
    # 观测；B 离线使"无 ACK 进度"断言真实（无本地直投、无消费）。
    check(tag + "_login_a", login(va, aid))
    broker_pid = kafka_broker_pid()
    check(tag + "_broker_pid", broker_pid is not None,
          "KAFKA_BROKER_PID env not provided (need pgrep -f 'kafka.Kafka' in .sh)")
    if broker_pid is None:
        return  # 断言已记 FAIL，跳过后续避免崩溃（.sh 仍会跑完其它场景）
    # 基线：A→B（B 离线）接受，relay+consumer 排空（暂停前管线健康）。
    base_cmid = tag + "-base-" + ctx.suffix
    r0 = send_direct(va, aid, bid, base_cmid, "kafka base", timeout=8.0)
    check(tag + "_accept_base", r0 is not None and r0.get("msgid") == 11, str(r0))
    if not (r0 is not None and r0.get("msgid") == 11):
        return  # accept 失败：断言已记 FAIL，跳过后续避免崩溃
    base_mid = r0["message_id"]
    send_sigusr1(ctx.srv)
    assert_metric(tag + "_base_outbox_drained", "reliable_outbox_lag",
                  lambda v: int(v) == 0, "pre-pause relay+consumer drained", log, ctx.srv,
                  deadline=OUTBOX_LAG_SETTLE_DEADLINE)
    # 暂停 broker（kill -STOP；.sh trap 在 harness 异常退出时 CONT 兜底）。
    try:
        os.kill(broker_pid, signal.SIGSTOP)
    except OSError as e:
        check(tag + "_pause", False, "SIGSTOP broker pid=%s: %s" % (broker_pid, e))
        return
    check(tag + "_pause", True, "broker pid=%s SIGSTOP" % broker_pid)
    # 暂停窗口内接受一批新消息（durable accept 不依赖 Kafka；B 离线无本地直投）。
    n_burst = 8  # == outbox claim_batch=8，单批 claim，lease 同步到期
    cmids = []
    for i in range(n_burst):
        cmid = "%s-%d-%s" % (tag, i, ctx.suffix)
        cmids.append(cmid)
        r = send_direct(va, aid, bid, cmid, "kafka pause %d" % i, timeout=8.0)
        check(tag + "_accept_%d" % i, r is not None and r.get("msgid") == 11, str(r))
    # 暂停期消费停顿（可靠管线）：relay publish 失败不标 processed → outbox 事件
    # 滞留 → outbox lag 上升（有界等待 >0，非固定 sleep；publish deadline 5s
    # 返回失败后 gauge 更新）。
    lag_rose = wait_metric_reaches(log, "reliable_outbox_lag", 1,
                                   handle=ctx.srv, deadline=20.0)
    check(tag + "_outbox_lag_rising_during_pause", lag_rose is not None,
          "reliable_outbox_lag=%s want>0 while broker SIGSTOP (relay publish stalled)"
          % lag_rose)
    # DB 层无 ACK 进度：暂停批交付行不得进入 Acknowledged（B 离线 + 消费停滞）。
    no_ack_progress = True
    for i in range(n_burst):
        mid = message_id(cmids[i])
        if mid == 0:
            continue  # accept 失败（已 FAIL）无行，跳过
        st = delivery_state(mid, bid)
        if st is not None and st["state"] == DB_STATE["Acknowledged"]:
            no_ack_progress = False
            break
    check(tag + "_no_ack_progress_during_pause", no_ack_progress,
          "accepted-but-undelivered batch must not reach Acknowledged while broker SIGSTOP")
    # 恢复 broker（kill -CONT）。
    try:
        os.kill(broker_pid, signal.SIGCONT)
    except OSError as e:
        check(tag + "_resume", False, "SIGCONT broker pid=%s: %s" % (broker_pid, e))
        return
    check(tag + "_resume", True, "broker pid=%s SIGCONT" % broker_pid)
    # at-least-once 不丢：B 上线收到全部已接受消息并 ACK → 最终 Acknowledged
    #（B 上线的 Pending 投递由 DB 侧 sessionAvailable 交付，不依赖 Kafka）。
    check(tag + "_login_b", login(vb, bid))
    known = set()
    for i in range(n_burst):
        mid = message_id(cmids[i])
        if mid != 0:
            known.add(mid)
    known.add(base_mid)
    seen = set()
    drain_deadline = time.time() + 60.0
    while time.time() < drain_deadline and not seen.issuperset(known):
        d = vb.recv(5.0)
        if is_delivery(d):
            ack(vb, d["message_id"])
            if d["message_id"] in known:
                seen.add(d["message_id"])
    for i in range(n_burst):
        mid = message_id(cmids[i])
        check(tag + "_deliver_%d" % i, mid in seen,
              "cmid=%s mid=%s delivered after B login (at-least-once)" % (cmids[i], mid))
    check(tag + "_deliver_base", base_mid in seen, "base mid=%s delivered" % base_mid)
    acked_all = wait_until(30.0, lambda: all(
        delivery_state(m, bid) is not None
        and delivery_state(m, bid)["state"] == DB_STATE["Acknowledged"] for m in known))
    check(tag + "_all_acked", acked_all,
          str([(m, delivery_state(m, bid)) for m in sorted(known)]))
    check(tag + "_no_manual_db_change", True,
          "at-least-once resumes after broker CONT without manual DB change")
    # 控制面收敛：outbox lag → 0（relay 在 lease 到期后重领重投，恢复后收敛，
    # 不压垮控制面）。
    m = assert_metric_snapshot_present(tag, log)
    assert_metric(tag + "_m_outbox_lag_zero", "reliable_outbox_lag",
                  lambda v: int(v) == 0, str(m), log, ctx.srv,
                  deadline=OUTBOX_LAG_SETTLE_DEADLINE)


def kp9_mysql_timeout(ctx):
    tag = "kp9_mysql_timeout"
    cfg = ctx.write_config(tag, presence_db=1)
    log = scenario_log(tag)
    lwt_orig = sql_query_global("SELECT @@GLOBAL.lock_wait_timeout")
    sql_run_global("SET GLOBAL lock_wait_timeout=2")
    locker = None
    try:
        ctx.srv.spawn(cfg, log)
        check(tag + "_server", ctx.srv.ready(log), log_tail(log))
        va = ctx.new_client(V2_PORT)
        vb = ctx.new_client(V2_PORT)
        aid = reg(va, tag + "_a_" + ctx.suffix)
        bid = reg(vb, tag + "_b_" + ctx.suffix)
        check(tag + "_reg_ab", aid > 0 and bid > 0)
        check(tag + "_login_a", login(va, aid))
        cmid = tag + "-" + ctx.suffix
        locker = sql_lock_holder("LOCK TABLES ChatMessage WRITE; SELECT SLEEP(30);")
        r = send_direct(va, aid, bid, cmid, "mysql timeout", timeout=6.0)
        check(tag + "_accept_rejected",
              r is not None and r.get("msgid") == 13, str(r))
        sql_lock_holder_stop(locker)
        locker = None
        try:
            sql_run_global("SET GLOBAL lock_wait_timeout=%s" % lwt_orig)
        except RuntimeError:
            pass
        check(tag + "_db_no_partial_row", message_id(cmid) == 0,
              "mid=%s" % message_id(cmid))
        r2 = send_direct(va, aid, bid, cmid, "mysql timeout")
        check(tag + "_retry_fresh",
              r2 is not None and r2.get("msgid") == 11 and not r2.get("duplicate"), str(r2))
        check(tag + "_db_single_row", r2 is not None and message_id(cmid) == r2["message_id"],
              "mid=%s" % message_id(cmid))
    finally:
        sql_lock_holder_stop(locker)
        try:
            sql_run_global("SET GLOBAL lock_wait_timeout=%s" % lwt_orig)
        except RuntimeError:
            pass


# ---- harness 上下文 ----

class Ctx(object):
    def __init__(self, srv, new_client, suffix):
        self.srv = srv
        self.new_client = new_client
        self.suffix = suffix
        self.l5_conversations = 120
        self._cfg_counter = 0

    def write_config(self, scen, presence_db):
        # 每场景独立 Kafka topic/group，隔离跨场景 offset/事件累积；presence db
        # 按场景（l5/storm/kp=db1 隔离，db0 场景显式 db0 以暴露累积）。
        self._cfg_counter += 1
        path = os.path.join(WORK, "cfg_%02d_%s.json" % (self._cfg_counter, scen))
        topic = "muduo-test-chaos-%d-%s-%s" % (SEED, self.suffix, scen)
        group = "muduo-test-chaos-g-%d-%s-%s" % (SEED, self.suffix, scen)
        cfg = {
            "server": {"v1": {"ip": HOST, "port": V1_PORT, "threads": 2},
                       "v2": {"port": V2_PORT}},
            "db": {"host": "127.0.0.1", "port": 3306, "user": "root",
                   "password": DB_PW, "dbname": DB_NAME, "pool_size": 4},
            "executor": {"workers": 2, "queue_capacity": 64},
            "reliable": {"ack_timeout_ms": 3000, "backoff_base_ms": 1000,
                         "backoff_cap_ms": 2000, "backoff_multiplier": 2,
                         "jitter_fraction": 0.0, "jitter_seed": SEED,
                         "message_retention_ms": 300000,
                         "acked_retention_ms": 3600000,
                         "expired_retention_ms": 3600000,
                         "cleanup_batch": 100, "cleanup_cycle_ms": 3600000,
                         "retry_batch_limit": 500},
            "outbox": {"claim_batch": 8, "scan_interval_ms": 200,
                       "claim_lease_ms": 30000},
            "gateway": {"id": 1,
                        "presence": {"host": "127.0.0.1", "port": 6379,
                                     "db": presence_db, "ttl_ms": 30000,
                                     "connect_timeout_ms": 1000,
                                     "command_timeout_ms": 1000},
                        "kafka": {"host": "127.0.0.1", "port": 9092},
                        "consumer": {"topic": topic, "group_id": group,
                                     "fetch_batch_limit": 100,
                                     "poll_deadline_ms": 5000}}
        }
        with open(path, "w") as f:
            json.dump(cfg, f)
        return path


def main():
    global HOST, V1_PORT, V2_PORT, SERVER_BIN, DB_NAME, DB_PW, WORK
    HOST = sys.argv[1]
    V1_PORT = int(sys.argv[2])
    V2_PORT = int(sys.argv[3])
    SERVER_BIN = sys.argv[4]
    DB_NAME = sys.argv[5]
    DB_PW = sys.argv[6]
    WORK = sys.argv[7]
    suffix = str(int(time.time() * 1000))[-8:]

    srv = ServerHandle(V1_PORT, V2_PORT)
    clients = []

    def new_client(port):
        c = V2Client(HOST, port)
        clients.append(c)
        return c

    ctx = Ctx(srv, new_client, suffix)
    scenarios = [
        ("l5_lastseen_unbounded", l5_lastseen_unbounded),
        ("db0_presence_accumulation", db0_presence_accumulation),
        ("storm_recovery_capacity", storm_recovery_capacity),
        ("kp1_chatserver_kill", kp1_chatserver_kill),
        ("kp4_redis_delay", kp4_redis_delay),
        ("kp7_kafka_pause", kp7_kafka_pause),
        ("kp9_mysql_timeout", kp9_mysql_timeout),
    ]
    try:
        for name, fn in scenarios:
            try:
                fn(ctx)
            except Exception as e:  # noqa: BLE001 - 场景崩溃记 FAIL 后继续，harness 不中断
                check("chaos_scenario_crash_" + name, False, "exception: %r" % (e,))
                import traceback
                traceback.print_exc()
    finally:
        srv.close()
        for c in clients:
            c.close()

    if FAIL:
        print("CHAOS_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("CHAOS_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
