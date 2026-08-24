#!/usr/bin/env python3
"""P5-02 benchmark runner (D3).

Orchestrates: dep liveness (MySQL/Redis/Kafka) -> topic hygiene (fresh topic) ->
fresh DB + dbmigrate + data-scale seed (1000 users / 900 direct / 100 group /
hot target) -> start echo testserver + ChatServer (metrics.enabled=true) ->
scenario matrix (9 scenarios x warmup + >=5 reps) -> per-rep metrics + RSS ->
bench-result-v1 JSON.

Fault scenarios:
  db-backpressure: LOCK TABLES Conversation WRITE during hot load (accept
                   transactions block on the injected table lock -> lock wait).
  redis-down     : redis-cli CLIENT PAUSE ALL covering the load window -> new
                   login claimPresence fails (freeze downgrade: login paused,
                   durable accept continues).
  kafka-pause    : SIGSTOP broker during hot load -> outbox publish stalls,
                   messages stay Pending, outbox_lag/oldest_pending rise.

Runs on WSL (Ubuntu). Usage:
  python3 tools/bench/run.py --build-dir <tree> [--reps N] [--scenarios a,b,c]
                             [--duration-ms T] [--conns N] [--out <json>]

Environment decisions (P5-02 card): perf unavailable -> gprof fallback + fixed
timing metrics (/metrics / SIGUSR1) + RSS. All numbers are WSL2/loopback/
single-machine/experimental; not extrapolatable.
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stats import compute_stats  # noqa: E402

HOST = "127.0.0.1"
ECHO_PORT = 18000       # mymuduo echo testserver (chat-bench 3 suite)
V1_PORT = 16201         # ChatServer v1 (newline JSON)
V2_PORT = 16211         # ChatServer v2 (binary)
METRICS_PORT = 17201    # /metrics Prometheus endpoint
DB_PASSWORD = os.environ.get("DB_PASSWORD", "123456")
DB_NAME = "chat_p502_bench"
KAFKA_TOPIC = "muduo-p502"   # fresh suffix appended per run
MIGRATIONS_DIR = "sql/migrations"


def sh(args, timeout=120, check=True):
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError("cmd failed (%s): %s\n%s\n%s"
                           % (" ".join(args), r.returncode, r.stdout, r.stderr))
    return r


def port_open(port, host=HOST, timeout=1.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port(port, timeout=30, host=HOST):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_open(port, host):
            return True
        time.sleep(0.2)
    return False


def rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid, "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (OSError, ValueError):
        pass
    return 0


def fetch_metrics(port=METRICS_PORT):
    """Parse /metrics Prometheus text into a dict {name: value}."""
    try:
        with socket.create_connection((HOST, port), timeout=3) as s:
            s.sendall(b"GET / HTTP/1.0\r\nHost: localhost\r\n\r\n")
            data = b""
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
                if len(data) > 1 << 20:
                    break
    except OSError:
        return {}
    text = data.decode("utf-8", "replace")
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 2 and parts[1].replace(".", "", 1).replace("-", "", 1).isdigit():
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return out


class ServerProc:
    def __init__(self, args, log_path, cwd=None):
        self.args = args
        self.log = open(log_path, "a")
        self.cwd = cwd
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT,
                                     cwd=cwd)
        self.pid = self.proc.pid

    def stop(self, timeout=15):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.log.close()


def check_deps():
    print("[preflight] dependency liveness checks", flush=True)
    try:
        sh(["mysql", "-uroot", "-p" + DB_PASSWORD, "-N", "-B", "-e", "SELECT 1"],
           check=True)
        print("[preflight] MySQL 3306 OK", flush=True)
    except (RuntimeError, FileNotFoundError) as e:
        print("[preflight] MySQL FAIL: %s" % e, flush=True)
        return False
    try:
        r = sh(["redis-cli", "ping"], check=True)
        if "PONG" not in r.stdout:
            print("[preflight] Redis ping FAIL: %s" % r.stdout, flush=True)
            return False
        print("[preflight] Redis 6379 OK", flush=True)
    except (RuntimeError, FileNotFoundError) as e:
        print("[preflight] Redis FAIL: %s" % e, flush=True)
        return False
    if not port_open(9092, timeout=2):
        print("[preflight] Kafka 9092 unreachable", flush=True)
        return False
    print("[preflight] Kafka 9092 OK", flush=True)
    return True


def check_load_isolation(ports):
    for p in ports:
        if port_open(p, timeout=0.5):
            print("[preflight] WARNING: port %d already listening (load isolation)" % p,
                  flush=True)
            return False
    return True


def create_topic(topic):
    bin_dir = "/home/makise/tools/kafka_2.13-4.3.1/bin"
    script = bin_dir + "/kafka-topics.sh"
    if not os.path.exists(script):
        print("[topic] kafka-topics.sh not found; relying on auto-create", flush=True)
        return
    r = sh([script, "--bootstrap-server", "127.0.0.1:9092", "--list"],
           check=False)
    if r.returncode != 0:
        print("[topic] list failed: %s" % r.stderr, flush=True)
        return
    if topic in r.stdout.split():
        print("[topic] %s already exists (reusing)" % topic, flush=True)
        return
    r = sh([script, "--bootstrap-server", "127.0.0.1:9092",
            "--create", "--topic", topic, "--partitions", "1",
            "--replication-factor", "1"], check=False)
    print("[topic] create %s rc=%d %s" % (topic, r.returncode, r.stdout.strip()),
          flush=True)
    if r.returncode != 0:
        print("[topic] create stderr: %s" % r.stderr.strip(), flush=True)


def setup_db(root, work):
    """Drop/create fresh DB + dbmigrate."""
    print("[db] drop/create %s" % DB_NAME, flush=True)
    sh(["mysql", "-uroot", "-p" + DB_PASSWORD, "-e",
        "DROP DATABASE IF EXISTS %s; CREATE DATABASE %s CHARACTER SET utf8mb4;"
        % (DB_NAME, DB_NAME)])
    dbmigrate = os.path.join(work, "bin", "dbmigrate")
    if not os.path.exists(dbmigrate):
        dbmigrate = shutil.which("dbmigrate")
    print("[db] dbmigrate %s" % DB_NAME, flush=True)
    r = sh([dbmigrate, "--db", DB_NAME, "--password", DB_PASSWORD,
            "--migrations-dir", os.path.join(root, MIGRATIONS_DIR)],
           check=False)
    if r.returncode != 0:
        raise RuntimeError("dbmigrate failed: %s" % r.stderr)
    print(r.stdout, flush=True)


def seed_data(root, work, hot_target):
    """Seed 1000 users + 900 direct conv + 100 group conv + hot target."""
    print("[seed] building dataset (1000 users / 900 direct / 100 group)",
          flush=True)
    users = []
    for i in range(1, 1001):
        users.append("(%d,'p502_u%d','123456','offline')" % (i, i))
    sql = []
    sql.append("INSERT INTO User(id,name,password,state) VALUES %s;"
               % ",".join(users))
    # 900 direct conversations: distinct (low,high) pairs among users 1..901.
    pairs = []
    seen = set()
    for i in range(1, 1000):
        lo = i
        hi = i + 1
        key = (lo, hi)
        if key in seen:
            continue
        seen.add(key)
        pairs.append(key)
        if len(pairs) == 900:
            break
    conv_ids = list(range(1, 901))
    sql.append("INSERT INTO Conversation(id,kind,next_sequence) VALUES %s;"
               % ",".join("(%d,'DIRECT',0)" % c for c in conv_ids))
    sql.append("INSERT INTO DirectConversation(conversation_id,user_low_id,user_high_id) "
               "VALUES %s;"
               % ",".join("(%d,%d,%d)" % (cid, lo, hi)
                          for cid, (lo, hi) in zip(conv_ids, pairs[:900])))
    # 100 groups: AllGroup + GroupConversation + GroupUser membership.
    # load.py group scenario: sender = group creator g, online members =
    # [200+g, 300+g, 400+g] (all ACK). Seed the same membership.
    gstart = 1001
    group_rows = []
    gconv_rows = []
    guser_rows = []
    member_set = {}
    for g in range(1, 101):
        gid = gstart + g - 1
        group_rows.append("(%d,'p502_g%d','bench group %d')" % (g, g, g))
        gconv_rows.append("(%d,%d)" % (gid, g))
        members = [g, 200 + g, 300 + g, 400 + g]
        member_set[g] = members
        for m in members:
            if m <= 1000:
                guser_rows.append("(%d,%d,'normal')" % (g, m))
    sql.append("INSERT INTO AllGroup(id,groupname,groupdesc) VALUES %s;"
               % ",".join(group_rows))
    sql.append("INSERT INTO Conversation(id,kind,next_sequence) VALUES %s;"
               % ",".join("(%d,'GROUP',0)" % c for c in range(gstart, gstart + 100)))
    sql.append("INSERT INTO GroupConversation(conversation_id,group_id) VALUES %s;"
               % ",".join(gconv_rows))
    sql.append("INSERT INTO GroupUser(groupid,userid,grouprole) VALUES %s;"
               % ",".join(guser_rows))
    # hot target user (single offline target) — ensure exists (it does, id 1000)
    assert 1 <= hot_target <= 1000
    # 预建 hot 直连会话（sender 1..16 × hot_target，固定 conversation id 2001..2016），
    # 供 DB backpressure 用 FOR UPDATE 行锁精确阻塞 hot accept（P3-11 行锁串行验证）。
    hot_start = 2001
    hot_rows = []
    hot_conv_rows = []
    for h in range(16):
        cid = hot_start + h
        lo, hi = sorted((h + 1, hot_target))
        hot_rows.append("(%d,'DIRECT',0)" % cid)
        hot_conv_rows.append("(%d,%d,%d)" % (cid, lo, hi))
    sql.append("INSERT INTO Conversation(id,kind,next_sequence) VALUES %s;"
               % ",".join(hot_rows))
    sql.append("INSERT INTO DirectConversation(conversation_id,user_low_id,user_high_id) "
               "VALUES %s;" % ",".join(hot_conv_rows))
    sql_text = "\n".join(sql)
    r = sh(["mysql", "-uroot", "-p" + DB_PASSWORD, DB_NAME, "-e", sql_text],
           check=False)
    if r.returncode != 0:
        raise RuntimeError("seed failed: %s\n%s" % (r.stderr, sql_text[:2000]))
    counts = sh(["mysql", "-uroot", "-p" + DB_PASSWORD, DB_NAME, "-N", "-B", "-e",
                 "SELECT (SELECT COUNT(*) FROM User),"
                 "(SELECT COUNT(*) FROM DirectConversation),"
                 "(SELECT COUNT(*) FROM AllGroup),"
                 "(SELECT COUNT(*) FROM GroupUser);"], check=True).stdout
    print("[seed] counts(User,Direct,Group,GroupUser)=%s" % counts.strip(),
          flush=True)


def write_config(root, work, topic):
    cfg = {
        "server": {
            "v1": {"ip": HOST, "port": V1_PORT, "threads": 1},
            "v2": {"port": V2_PORT},
        },
        "db": {"host": HOST, "port": 3306, "user": "root",
               "password": DB_PASSWORD, "dbname": DB_NAME, "pool_size": 5},
        "executor": {"workers": 1, "queue_capacity": 64},
        "gateway": {
            "id": 1,
            "presence": {"host": HOST, "port": 6379, "db": 0, "ttl_ms": 30000,
                         "connect_timeout_ms": 1000, "command_timeout_ms": 1000},
            "kafka": {"host": HOST, "port": 9092},
            "consumer": {"topic": topic, "fetch_batch_limit": 100,
                         "poll_deadline_ms": 5000},
        },
        "metrics": {"enabled": True, "port": METRICS_PORT},
    }
    path = os.path.join(work, "p5-02-config.json")
    with open(path, "w") as f:
        json.dump(cfg, f, indent=2)
    return path


def start_echo(work):
    """Start mymuduo echo testserver (chat-bench 3 suite target) on ECHO_PORT."""
    ts = os.path.join(work, "bin", "testserver")
    src = "/mnt/d/agent_learning/muduo-chat/mymuduo/example/testserver.cc"
    root = "/mnt/d/agent_learning/muduo-chat/mymuduo"
    # 变体放 build tree；`../EventLoop.h` 相对 include 按源位置解析，改写为
    # mymuduo 根的绝对 include（不改源码树、不落仓库）。
    variant = os.path.join(work, "testserver_echo.cc")
    if not os.path.exists(variant):
        with open(src) as f:
            text = f.read()
        text = text.replace('InetAddress listenAddr(8000);',
                            'InetAddress listenAddr(%d);' % ECHO_PORT)
        text = text.replace('#include "../', '#include "%s/' % root)
        with open(variant, "w") as f:
            f.write(text)
        print("[echo] compiling testserver (port %d)" % ECHO_PORT, flush=True)
        cc_srcs = [os.path.join(root, f) for f in sorted(os.listdir(root))
                   if f.endswith(".cc")]
        sh(["g++", "-std=c++11", "-I", root,
            "-I", "/mnt/d/agent_learning/muduo-chat/thirdparty", variant] +
           cc_srcs + ["-pthread", "-o", ts])
    if not os.path.exists(ts):
        raise RuntimeError("testserver binary missing: %s" % ts)
    log = os.path.join(work, "testserver.log")
    sp = ServerProc([ts], log, cwd=work)
    if not wait_port(ECHO_PORT, timeout=15):
        sp.stop()
        raise RuntimeError("echo testserver did not start on %d" % ECHO_PORT)
    print("[echo] testserver on %d pid=%d" % (ECHO_PORT, sp.pid), flush=True)
    return sp


def start_chatserver(work, config_path):
    bin_dir = os.path.join(work, "bin", "ChatServer")
    log = os.path.join(work, "chatserver.log")
    sp = ServerProc([bin_dir, HOST, str(V1_PORT), "--config", config_path],
                    log, cwd=work)
    ok = wait_port(METRICS_PORT, timeout=30) and wait_port(V1_PORT, timeout=30)
    if not ok:
        sp.stop()
        with open(log) as f:
            tail = f.read()[-4000:]
        raise RuntimeError("ChatServer did not become ready\n%s" % tail)
    print("[server] ChatServer v1=%d v2=%d metrics=%d pid=%d"
          % (V1_PORT, V2_PORT, METRICS_PORT, sp.pid), flush=True)
    return sp


def run_scenario_reliable(args, work, scenario, conns, duration_ms, hot_target):
    """One rep of a reliable load scenario. Returns parsed result dict."""
    load = os.path.join(os.path.dirname(os.path.abspath(__file__)), "load.py")
    cmd = [sys.executable, load, HOST, str(V1_PORT), scenario,
           str(conns), str(duration_ms)]
    if scenario == "hot":
        cmd += ["--target", str(hot_target)]
    r = sh(cmd, timeout=duration_ms / 1000.0 + 60, check=False)
    parsed = {}
    if r.returncode == 0:
        m = re.search(r"scenario=(\S+) conns=(\d+) duration_ms=(\d+) ok=(\d+) "
                      r"msg_per_sec=([\d.]+) p50_ms=([\d.]+) p95_ms=([\d.]+) "
                      r"p99_ms=([\d.]+) errors=(\d+)", r.stdout)
        if m:
            parsed = {"msg_per_sec": float(m.group(5)), "p50_ms": float(m.group(6)),
                      "p95_ms": float(m.group(7)), "p99_ms": float(m.group(8)),
                      "errors": int(m.group(9))}
    return parsed


# chat-bench 三场景固定参数（开卡冻结；workload 元数据与命令行共用同一来源，避免
# args.conns/duration_ms 默认值 16/2000 误填三场景）。
CHATBENCH_PARAMS = {
    "connect": {"connections": 200},
    "echo": {"connections": 4, "messages": 200, "payload_size": 64},
    "slow-consumer": {"connections": 2, "messages": 200, "payload_size": 4096,
                      "duration_ms": 5000},
}


def run_chatbench(work, scenario):
    """One rep of chat-bench connect/echo/slow-consumer against echo server."""
    bench = os.path.join(work, "bin", "chat-bench")
    p = CHATBENCH_PARAMS[scenario]
    if scenario == "connect":
        cmd = [bench, HOST, str(ECHO_PORT), "connect",
               "--connections", str(p["connections"])]
    elif scenario == "echo":
        cmd = [bench, HOST, str(ECHO_PORT), "echo",
               "--connections", str(p["connections"]),
               "--messages", str(p["messages"]),
               "--payload-size", str(p["payload_size"])]
    else:  # slow-consumer
        cmd = [bench, HOST, str(ECHO_PORT), "slow-consumer",
               "--connections", str(p["connections"]),
               "--messages", str(p["messages"]),
               "--payload-size", str(p["payload_size"]),
               "--duration-ms", str(p["duration_ms"])]
    t0 = time.time()
    r = sh(cmd, timeout=120, check=False)
    elapsed = time.time() - t0
    if r.returncode != 0:
        return {}
    try:
        j = json.loads(r.stdout)
    except ValueError:
        return {}
    res = j.get("result", {})
    lat = res.get("latency_us", {})
    out = {}
    if scenario == "connect":
        # connect: msg/s = connections_ok / 实测耗时（runner 计时，chat-bench 无内置）。
        out["msg_per_sec"] = float(res.get("connections_ok", 0)) / max(0.001, elapsed)
    elif scenario == "echo":
        out["msg_per_sec"] = float(res.get("msg_per_sec", 0.0))
        out["throughput_mbps"] = float(res.get("throughput_mbps", 0.0))
    else:  # slow-consumer：数据一致性场景，吞吐非主指标
        out["msg_per_sec"] = 0.0
        out["bytes_sent"] = int(res.get("bytes_sent", 0))
        out["bytes_received"] = int(res.get("bytes_received", 0))
        out["early_closes"] = int(res.get("early_closes", 0))
    out["p50_ms"] = lat.get("p50", 0.0) / 1000.0
    out["p95_ms"] = lat.get("p95", 0.0) / 1000.0
    out["p99_ms"] = lat.get("p99", 0.0) / 1000.0
    return out


def collect_server_metrics(port=METRICS_PORT):
    m = fetch_metrics(port)
    out = {
        "executor_queue_depth": int(m.get("executor_queue", 0)),
        "executor_dropped": int(m.get("executor_dropped", 0)),
        "oldest_pending_age_ms": int(m.get("reliable_oldest_pending_age_ms", -1)),
        "outbox_lag": int(m.get("reliable_outbox_lag", 0)),
        "pool_active": int(m.get("pool_active", 0)),
        "loop_lag_ms": int(m.get("loop_lag_ms", -1)),
        "reliable_accepts": int(m.get("reliable_accepts", 0)),
        "reliable_pending": int(m.get("reliable_pending", 0)),
        "reliable_inflight": int(m.get("reliable_inflight", 0)),
        "reliable_acked": int(m.get("reliable_acked", 0)),
        "reliable_duplicates": int(m.get("reliable_duplicates", 0)),
        "reliable_attempts": int(m.get("reliable_attempts", 0)),
        "ack_latency_p99_ms": int(m.get("reliable_ack_latency_p99_ms", 0)),
    }
    return out


def mid_window_metrics(server_pid, delay_s=1.0):
    """Fetch /metrics during the fault window (after a short delay)."""
    time.sleep(delay_s)
    return collect_server_metrics()


def sample_lock_wait():
    """Return max lock-wait seconds across trx/processlist.

    FOR UPDATE row lock → innodb_trx trx_state='LOCK WAIT' (row lock wait).
    Also check processlist for 'Waiting for table metadata lock' (table lock).
    """
    try:
        r = sh(["mysql", "-uroot", "-p" + DB_PASSWORD, DB_NAME, "-N", "-B", "-e",
                "SELECT COALESCE(MAX(TIMESTAMPDIFF(SECOND, "
                "trx_wait_started, NOW())),0) FROM information_schema.innodb_trx "
                "WHERE trx_state='LOCK WAIT';"], check=True)
        trx_wait = float(r.stdout.strip() or 0)
    except (RuntimeError, FileNotFoundError):
        trx_wait = 0.0
    try:
        r = sh(["mysql", "-uroot", "-p" + DB_PASSWORD, "-N", "-B", "-e",
                "SELECT COALESCE(MAX(time),0) FROM information_schema.processlist "
                "WHERE state LIKE '%lock%';"], check=True)
        pl_wait = float(r.stdout.strip() or 0)
    except (RuntimeError, FileNotFoundError):
        pl_wait = 0.0
    return max(trx_wait, pl_wait)


# db-backpressure 后台锁等待采样（负载窗口内重复采样取最大值，毫秒）。
_SAMPLED_LOCK_WAIT = [0.0]


def _sample_lock_wait_loop(duration_s):
    deadline = time.time() + duration_s
    best = 0.0
    while time.time() < deadline:
        try:
            v = sample_lock_wait()
            if v > best:
                best = v
        except Exception:
            pass
        time.sleep(0.5)
    _SAMPLED_LOCK_WAIT[0] = best * 1000.0


def probe_durable_accept_during_redis_down(port, target_id):
    """Measure durable accept continuing + fresh login rejection under Redis down.

    Returns dict: {accepts_during_down, login_rejected_during_down,
                   login_ok_after_recovery}.
    """
    def connect_and_login(uid):
        try:
            s = socket.create_connection((HOST, port), timeout=10)
        except OSError:
            return None
        s.sendall(json.dumps({"msgid": 1, "id": uid, "password": "123456"}).encode()
                  + b"\n")
        s.settimeout(10)
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                s.close()
                return None
            buf += chunk
        try:
            ack = json.loads(buf.split(b"\n", 1)[0])
        except ValueError:
            s.close()
            return None
        if ack.get("errno") != 0:
            s.close()
            return None
        return s

    # 1) login a sender while Redis is up
    s = connect_and_login(1)
    if s is None:
        return {"accepts_during_down": -1, "login_rejected_during_down": -1,
                "login_ok_after_recovery": -1}
    # 2) pause Redis
    subprocess.run(["redis-cli", "-n", "0", "CLIENT", "PAUSE", "4000", "ALL"],
                   capture_output=True, text=True)
    time.sleep(0.3)
    # 3) durable accepts during the pause (sender already logged in)
    accepts = 0
    for i in range(5):
        s.sendall(json.dumps({"msgid": 6, "id": 1, "toid": target_id,
                              "msg": "redis-down-probe-%d" % i}).encode() + b"\n")
        s.settimeout(4)
        buf = b""
        try:
            while b"\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        except OSError:
            break
        if buf:
            try:
                ack = json.loads(buf.split(b"\n", 1)[0])
                if ack.get("errno") == 0:
                    accepts += 1
            except ValueError:
                pass
    # 4) fresh login during the pause -> expected rejected (login paused)
    s2 = connect_and_login(2)
    rejected = (s2 is None)
    if s2:
        s2.close()
    # 5) wait for pause expiry, then fresh login should succeed
    time.sleep(2.0)
    s3 = connect_and_login(2)
    ok_after = (s3 is not None)
    if s3:
        s3.close()
    s.close()
    return {"accepts_during_down": accepts, "login_rejected_during_down": 1 if rejected else 0,
            "login_ok_after_recovery": 1 if ok_after else 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True, help="Debug build tree")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--scenarios", default=None, help="comma list; default all 9")
    ap.add_argument("--duration-ms", type=int, default=2000)
    ap.add_argument("--conns", type=int, default=16)
    ap.add_argument("--hot-target", type=int, default=1000)
    ap.add_argument("--out", default=None, help="output JSON path")
    ap.add_argument("--keep", action="store_true", help="keep server running")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    root = "/mnt/d/agent_learning/muduo-chat"
    if not os.path.exists(os.path.join(build_dir, "bin", "ChatServer")):
        raise SystemExit("build tree missing bin/ChatServer: %s" % build_dir)

    all_scenarios = ["connect", "echo", "slow-consumer",
                     "reliable-direct", "reliable-group", "hot",
                     "db-backpressure", "redis-down", "kafka-pause"]
    scenarios = all_scenarios if not args.scenarios else \
        [s.strip() for s in args.scenarios.split(",")]

    if not check_deps():
        raise SystemExit("dependency preflight failed")
    if not check_load_isolation([ECHO_PORT, V1_PORT, V2_PORT, METRICS_PORT]):
        print("[preflight] aborting: ports in use (load isolation)", flush=True)
        raise SystemExit(1)

    run_id = str(int(time.time()))
    topic = KAFKA_TOPIC + "-" + run_id
    create_topic(topic)

    setup_db(root, build_dir)
    seed_data(root, build_dir, args.hot_target)
    config_path = write_config(root, build_dir, topic)

    chat_server = None
    echo_server = None
    try:
        if any(s not in ("connect", "echo", "slow-consumer") for s in scenarios):
            chat_server = start_chatserver(build_dir, config_path)
        if any(s in ("connect", "echo", "slow-consumer") for s in scenarios):
            echo_server = start_echo(build_dir)
        time.sleep(1)

        results = []
        for scenario in scenarios:
            print("\n[scenario] %s (warmup 1x5s)" % scenario, flush=True)
            # warmup (discard)
            if scenario in ("connect", "echo", "slow-consumer"):
                run_chatbench(build_dir, scenario)
                time.sleep(5)
            else:
                base = {"reliable-direct": "direct", "reliable-group": "group",
                        "hot": "hot", "db-backpressure": "hot",
                        "redis-down": "direct", "kafka-pause": "hot"}[scenario]
                run_scenario_reliable(args, build_dir, base, args.conns,
                                      args.duration_ms, args.hot_target)
                time.sleep(5)

            reps = []
            # RSS 归属：chat-bench 三件套测 echo testserver（负载落在该进程）；
            # reliable/fault 场景测 ChatServer。
            server_pid = (echo_server.pid if echo_server and scenario in
                          ("connect", "echo", "slow-consumer")
                          else (chat_server.pid if chat_server else 0))
            for i in range(args.reps):
                mid = {}
                if scenario in ("connect", "echo", "slow-consumer"):
                    data = run_chatbench(build_dir, scenario)
                elif scenario == "db-backpressure":
                    # hot accept 事务对 Conversation 行做 FOR UPDATE 取 next_sequence
                    # → 外部持有同一行锁阻塞 accept（P3-11 行锁串行验证）。锁住预建
                    # hot 会话（id 2001..2016，sender 1..16 × hot_target）。
                    lock_err = os.path.join(build_dir, "db-lock-err.log")
                    lock_secs = args.duration_ms / 1000.0 * 2 + 8
                    ids = ",".join(str(2001 + i) for i in range(16))
                    lock_proc = subprocess.Popen(
                        ["mysql", "-uroot", "-p" + DB_PASSWORD, DB_NAME,
                         "-e",
                         "START TRANSACTION;"
                         "SELECT id FROM Conversation WHERE id IN (%s) FOR UPDATE;"
                         "SELECT SLEEP(%d); COMMIT;" % (ids, lock_secs)],
                        stdout=subprocess.DEVNULL, stderr=open(lock_err, "w"))
                    time.sleep(1.0)  # let the lock take effect
                    # 负载运行期间后台采样锁等待（load 客户端阻塞在 10s recv 超时内，
                    # 结束后采样可能错过锁窗）。
                    sampler = threading.Thread(
                        target=_sample_lock_wait_loop,
                        args=(args.duration_ms / 1000.0 + 2,))
                    sampler.start()
                    data = run_scenario_reliable(args, build_dir, "hot",
                                                 args.conns, args.duration_ms,
                                                 args.hot_target)
                    mid = mid_window_metrics(server_pid, 0)
                    sampler.join(timeout=30)
                    lock_wait_ms = _SAMPLED_LOCK_WAIT[0]
                    lock_proc.wait(timeout=60)
                    data["db_lock_wait_ms"] = lock_wait_ms
                elif scenario == "redis-down":
                    subprocess.run(["redis-cli", "-n", "0", "CLIENT", "PAUSE",
                                    str(args.duration_ms + 1500), "ALL"],
                                   capture_output=True, text=True)
                    time.sleep(0.5)
                    data = run_scenario_reliable(args, build_dir, "direct",
                                                 args.conns, args.duration_ms,
                                                 args.hot_target)
                    mid = mid_window_metrics(server_pid, 0)
                    # probe: durable accept continues, fresh login rejected
                    probe = probe_durable_accept_during_redis_down(
                        V1_PORT, args.hot_target)
                    data.update(probe)
                    time.sleep(1.5)  # let CLIENT PAUSE expire before next rep
                elif scenario == "kafka-pause":
                    pid = find_kafka_broker_pid()
                    if pid:
                        os.kill(pid, signal.SIGSTOP)
                    try:
                        data = run_scenario_reliable(args, build_dir, "hot",
                                                     args.conns, args.duration_ms,
                                                     args.hot_target)
                        mid = mid_window_metrics(server_pid, 0)
                    finally:
                        if pid:
                            os.kill(pid, signal.SIGCONT)
                            time.sleep(1.0)  # let consumer catch up
                else:
                    base = scenario.replace("reliable-", "")
                    data = run_scenario_reliable(args, build_dir, base,
                                                 args.conns, args.duration_ms,
                                                 args.hot_target)
                    time.sleep(0.5)  # let metrics settle
                m = collect_server_metrics()
                rep = {"rep": i + 1, "rss_kb": rss_kb(server_pid)}
                rep.update(data)
                rep.update(m)
                if mid:
                    rep["fault_window_metrics"] = mid
                reps.append(rep)
                print("  rep %d: %s" % (i + 1, {k: v for k, v in rep.items()
                                                if k != "rep"}), flush=True)

            host = {
                "kernel": subprocess.run(["uname", "-r"], capture_output=True,
                                         text=True).stdout.strip(),
                "cpu_model": "Intel(R) Core(TM) i9-14900HX",
                "cpu_count": 32,
                "ulimit_nofile": 10240,
                "mem_mb": 15796,
            }
            build = {
                "commit": subprocess.run(["git", "rev-parse", "HEAD"],
                                         capture_output=True, text=True,
                                         cwd=root).stdout.strip(),
                "cxx_flags": "-g -std=c++11",
                "build_type": "Debug",
            }
            if scenario in ("connect", "echo", "slow-consumer"):
                workload = {"tool": "chat-bench", "host": HOST, "port": ECHO_PORT}
                workload.update(CHATBENCH_PARAMS[scenario])
            else:
                workload = {"tool": "tools/bench/load.py", "host": HOST,
                            "port": V1_PORT, "connections": args.conns,
                            "duration_ms": args.duration_ms}
            result = {
                "schema_version": "bench-result-v1",
                "commit": build["commit"],
                "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "host": host,
                "build": build,
                "scenario": scenario,
                "workload": workload,
                "warmup": {"rounds": 1, "duration_ms": 5000},
                "repetitions": reps,
                "stats": {},
            }
            compute_stats(result)
            results.append(result)
            st = result["stats"]["msg_per_sec"]
            print("  stats: msg/s=%.2f (CI %.2f..%.2f) CV=%.3f n=%d"
                  % (st["mean"], st["ci95_low"], st["ci95_high"], st["cv"],
                     len(reps)), flush=True)

        out_path = args.out or os.path.join(build_dir, "p5-02-baseline-r1.json")
        with open(out_path, "w") as f:
            json.dump(results, f, indent=2)
        print("\n[out] wrote %s" % out_path, flush=True)
        return 0
    finally:
        if not args.keep:
            for s in (chat_server, echo_server):
                if s:
                    try:
                        s.stop()
                    except Exception:
                        pass


def find_kafka_broker_pid():
    try:
        r = subprocess.run(["pgrep", "-f", "kafka.Kafka"], capture_output=True,
                           text=True)
    except FileNotFoundError:
        return None
    for p in r.stdout.split():
        try:
            with open("/proc/%s/cmdline" % p, "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode()
            # 首个空白 token 的 basename 必须为 java（沿 cluster_chaos_test.sh
            # kp7 精确形态：`java kafka.Kafka ...`，过滤掉 kafka-topics.sh 等包装）。
            first = cmd.split(" ")[0].strip()
            if first.split("/")[-1] == "java":
                return int(p)
        except OSError:
            continue
    return None


if __name__ == "__main__":
    sys.exit(main())