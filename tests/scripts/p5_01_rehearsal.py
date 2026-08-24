#!/usr/bin/env python3
"""P5-01 real rehearsal: ChatServer (metrics.enabled=true port=9095) + tarball
prometheus (scrape localhost /metrics) + the kp7 Kafka pause scenario family to
trigger faults end-to-end, asserting the corresponding experimental alerts fire
within the fault window, auto resolve after recovery, and the steady-state control
round has no firing. x3 rounds.

Real rehearsal coverage (experimental SLO, docs/observability/alert-rules.md):
  - Kafka broker SIGSTOP (kp7 family) -> P5_DELIVERY_OUTBOX_LAG_HIGH
      (outbox publish stalls, reliable_outbox_lag > 60 -> for 30s -> firing)
      AND -> P5_DELIVERY_OLDEST_PENDING_STALLED
      (undelivered offline message pending age > 60000ms -> for 30s -> firing)
  - steady control round -> no P5_* firing

The remaining scenario families (kp1 kill -9 / kp4 Redis down / kp9 MySQL timeout)
are asserted statically by promtool `test rules` (tests/scripts/p5_01_test_rules.yml,
12 firing+resolve assertions); real scenario injection for them is deferred to later
cards (P5-02/后续). scenario_offline_pending is a passive placeholder: its oldest-
pending alert is driven within the kafka_pause scenario, not a separate fault
injection (docs/tasks/P5-01.md §验证).

Exit 0 iff all assertions pass across ROUNDS rounds; any failure exits 1 (never skip).
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
HOST = ""
V1_PORT = 0
V2_PORT = 0
METRICS_PORT = 9095
PROM_HTTP = "http://127.0.0.1:9090"
SERVER_BIN = ""
PROM_BIN = ""
DBMIGRATE_BIN = ""
MIGRATIONS_DIR = ""
DB_PREFIX = ""
DB_PW = ""
WORK = ""
ROUNDS = 3
SUFFIX = str(int(time.time() * 1000))[-8:]

OUTBOX_THRESHOLD = 60      # rule P5_DELIVERY_OUTBOX_LAG_HIGH expr threshold
OUTBOX_BURST = 75          # send enough offline directs so lag exceeds threshold
PENDING_THRESHOLD_MS = 60000
KAFKA_PAUSE_SECS = 40      # hold broker SIGSTOP so outbox_lag > 60 for >= 30s for-window


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name, flush=True)
    else:
        print("FAIL %s %s" % (name, detail), flush=True)
        FAIL.append(name)


def fresh_db(db_name):
    """drop+create+migrate 独立库：隔离各场景 pending/outbox，避免跨场景泄漏。"""
    import subprocess as sp
    sp.run(["mysql", "-uroot", "-p" + DB_PW, "-e",
            "DROP DATABASE IF EXISTS %s; CREATE DATABASE %s DEFAULT CHARSET utf8"
            % (db_name, db_name)], capture_output=True, text=True)
    p = sp.run([DBMIGRATE_BIN, "--db", db_name, "--password", DB_PW,
                "--migrations-dir", MIGRATIONS_DIR], capture_output=True, text=True)
    if p.returncode != 0:
        check("fresh_db_%s" % db_name, False, p.stderr.strip())
        return False
    return True


def sql_delivery_states(db_name):
    import subprocess as sp
    p = sp.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", db_name,
                "-e", "SELECT state, COUNT(*) FROM MessageDelivery GROUP BY state"],
               capture_output=True, text=True)
    return p.stdout.strip().replace("\n", "; ") if p.returncode == 0 else "<sql err>"


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


def send_direct(cli, from_id, to_id, cmid, content, timeout=8.0):
    cli.send({"msgid": 6, "id": from_id, "toid": to_id,
              "client_message_id": cmid, "content": content})
    return cli.recv(timeout)


def _port_ready(host, port):
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


def kafka_broker_pid():
    raw = os.environ.get("KAFKA_BROKER_PID", "")
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


class ServerHandle(object):
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
            self.kill()
        if not self._wait_ports_free():
            print("FAIL rehearsal_spawn_ports_busy", flush=True)
            FAIL.append("rehearsal_spawn_ports_busy")
        if self.logfile is not None:
            self.logfile.close()
        self.logfile = open(log_path, "w")
        cmd = ["setarch", "x86_64", "-R", SERVER_BIN, HOST, str(self.v1),
               "--config", config]
        self.proc = subprocess.Popen(cmd, stdout=self.logfile, stderr=subprocess.STDOUT,
                                     env=dict(os.environ))
        return self.proc

    def ready(self, log_path, timeout=30.0):
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
            if log_ready and _port_ready(HOST, self.v2) and _port_ready(HOST, METRICS_PORT):
                return True
            if self.proc is not None and self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def kill(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, signal.SIGKILL)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            pass
        if self.logfile is not None:
            self.logfile.close()
            self.logfile = None

    def close(self):
        self.kill()


class PromHandle(object):
    def __init__(self):
        self.proc = None
        self.logfile = None
        self.storage = os.path.join(WORK, "prom-tsdb")

    def spawn(self, log_path):
        if self.proc is not None and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass
        if self.logfile is not None:
            self.logfile.close()
        self.logfile = open(log_path, "w")
        cfg = os.path.join(WORK, "prometheus.yml")
        # 相对 rule_files 指向仓库 docker/prometheus/rules/*.yml
        rules_glob = os.path.join(
            os.path.dirname(SERVER_BIN).replace("/bin", ""), "..", "docker", "prometheus", "rules", "*.yml")
        rules_abs = os.path.normpath(os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "..",
            "docker", "prometheus", "rules", "*.yml"))
        cfg_body = (
            "global:\n"
            "  scrape_interval: 5s\n"
            "  evaluation_interval: 5s\n"
            "rule_files:\n"
            "  - '%s'\n"
            "scrape_configs:\n"
            "  - job_name: chat_metrics\n"
            "    honor_labels: true\n"
            "    metrics_path: /metrics\n"
            "    static_configs:\n"
            "      - targets: ['127.0.0.1:%d']\n" % (rules_abs, METRICS_PORT)
        )
        with open(cfg, "w") as f:
            f.write(cfg_body)
        cmd = [PROM_BIN, "--config.file=" + cfg,
               "--storage.tsdb.path=" + self.storage,
               "--web.listen-address=127.0.0.1:9090",
               "--storage.tsdb.retention.time=1h"]
        self.proc = subprocess.Popen(cmd, stdout=self.logfile, stderr=subprocess.STDOUT,
                                     env=dict(os.environ))
        return self.proc

    def ready(self, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", 9090), timeout=1.0):
                    return True
            except OSError:
                pass
            if self.proc is not None and self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
            os.kill(self.proc.pid, signal.SIGKILL)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass
        if self.logfile is not None:
            self.logfile.close()
            self.logfile = None


def prom_query(expr):
    import urllib.request
    url = PROM_HTTP + "/api/v1/query?query=" + urllib.parse.quote(expr)
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            data = json.loads(r.read().decode("utf-8"))
        if data.get("status") != "success":
            return None
        res = data["data"]["result"]
        if not res:
            return None
        return float(res[0]["value"][1])
    except Exception:  # noqa: BLE001
        return None


def prom_alert_states(alertname):
    import urllib.request
    url = PROM_HTTP + "/api/v1/alerts"
    states = []
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            data = json.loads(r.read().decode("utf-8"))
        for a in data.get("data", {}).get("alerts", []):
            if a.get("labels", {}).get("alertname") == alertname:
                states.append(a.get("state"))
    except Exception:  # noqa: BLE001
        pass
    return states


def any_p5_firing():
    import urllib.request
    url = PROM_HTTP + "/api/v1/alerts"
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            data = json.loads(r.read().decode("utf-8"))
        for a in data.get("data", {}).get("alerts", []):
            if a.get("labels", {}).get("alertname", "").startswith("P5_") \
                    and a.get("state") == "firing":
                return True
    except Exception:  # noqa: BLE001
        pass
    return False


def wait_until(timeout, fn, pause=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(pause)
    return False


def wait_alert_firing(alertname, timeout=120.0):
    return wait_until(timeout, lambda: "firing" in prom_alert_states(alertname))


def wait_alert_resolved(alertname, timeout=120.0):
    def resolved():
        st = prom_alert_states(alertname)
        return not st or "firing" not in st
    return wait_until(timeout, resolved)


def write_config(path, db_name, tag):
    topic = "muduo-p5-%s-%s" % (SUFFIX, tag)
    group = "muduo-p5-g-%s-%s" % (SUFFIX, tag)
    cfg = {
        "server": {"v1": {"ip": HOST, "port": V1_PORT, "threads": 2},
                   "v2": {"port": V2_PORT}},
        "db": {"host": "127.0.0.1", "port": 3306, "user": "root",
               "password": DB_PW, "dbname": db_name, "pool_size": 4},
        "executor": {"workers": 2, "queue_capacity": 64},
        "reliable": {"ack_timeout_ms": 3000, "backoff_base_ms": 1000,
                     "backoff_cap_ms": 2000, "backoff_multiplier": 2,
                     "jitter_fraction": 0.0, "jitter_seed": 20260824,
                     "message_retention_ms": 300000,
                     "acked_retention_ms": 3600000,
                     "expired_retention_ms": 3600000,
                     "cleanup_batch": 100, "cleanup_cycle_ms": 3600000,
                     "retry_batch_limit": 500},
        "outbox": {"claim_batch": 8, "scan_interval_ms": 200,
                   "claim_lease_ms": 30000},
        "gateway": {"id": 1,
                    "presence": {"host": "127.0.0.1", "port": 6379,
                                 "db": 1, "ttl_ms": 30000,
                                 "connect_timeout_ms": 1000,
                                 "command_timeout_ms": 1000},
                    "kafka": {"host": "127.0.0.1", "port": 9092},
                    "consumer": {"topic": topic, "group_id": group,
                                 "fetch_batch_limit": 100,
                                 "poll_deadline_ms": 5000}},
        "metrics": {"enabled": True, "port": METRICS_PORT},
    }
    with open(path, "w") as f:
        json.dump(cfg, f)
    return path


def scenario_control(srv, prom, rnd):
    """稳态对照轮：正常在线投递后，断言无 P5_* alert firing。"""
    tag = "control"
    db = "%s_%d_%s" % (DB_PREFIX, rnd, tag)
    if not fresh_db(db):
        return
    cfg = write_config(os.path.join(WORK, tag + ".json"), db, tag)
    log = os.path.join(WORK, tag + ".log")
    srv.spawn(cfg, log)
    check(tag + "_server", srv.ready(log), "server start")
    if not srv.ready(log):
        return
    # 稳态对照轮：断言 /metrics 已被 prometheus 抓取（chat_metrics 采样存在）。
    scraped = wait_until(30.0, lambda: prom_query("reliable_accepts") is not None)
    check("scrape_metrics_present", scraped, "chat_metrics target scraped")
    ca = V2Client(HOST, V2_PORT)
    cb = V2Client(HOST, V2_PORT)
    aid = reg(ca, tag + "_a_" + SUFFIX)
    bid = reg(cb, tag + "_b_" + SUFFIX)
    check(tag + "_reg_ab", aid > 0 and bid > 0)
    check(tag + "_login_ab", login(ca, aid) and login(cb, bid))
    r = send_direct(ca, aid, bid, tag + "-" + SUFFIX, "control msg")
    check(tag + "_accept", r is not None and r.get("msgid") == 11, str(r))
    d = cb.recv(5.0)
    if is_delivery(d):
        ack(cb, d["message_id"])
    # 等 outbox 排空、无 pending，且 prometheus 已抓取若干轮。
    wait_until(30.0, lambda: prom_query("reliable_outbox_lag") == 0.0)
    time.sleep(15.0)
    check(tag + "_no_firing", not any_p5_firing(), "steady state must not fire")
    ca.close()
    cb.close()
    srv.kill()


def scenario_kafka_pause(srv, prom, rnd):
    """kp7 family：broker SIGSTOP → outbox publish 停滞 + 离线消息滞留 Pending。
    断言（experimental，docs/observability/alert-rules.md）：
      - reliable_outbox_lag > 60 持续 for 30s → P5_DELIVERY_OUTBOX_LAG_HIGH firing
      - reliable_oldest_pending_age_ms > 60000 持续 for 30s → P5_DELIVERY_OLDEST_PENDING_STALLED firing
    CONT 后收敛（outbox lag → 0、pending 排空）→ 两 alert 自动 resolve。"""
    tag = "kafka_pause"
    db = "%s_%d_%s" % (DB_PREFIX, rnd, tag)
    if not fresh_db(db):
        return
    broker_pid = kafka_broker_pid()
    check(tag + "_broker_pid", broker_pid is not None,
          "KAFKA_BROKER_PID env not provided")
    if broker_pid is None:
        return
    cfg = write_config(os.path.join(WORK, tag + ".json"), db, tag)
    log = os.path.join(WORK, tag + ".log")
    srv.spawn(cfg, log)
    check(tag + "_server", srv.ready(log))
    if not srv.ready(log):
        return
    ca = V2Client(HOST, V2_PORT)
    aid = reg(ca, tag + "_a_" + SUFFIX)
    bid = reg(ca, tag + "_b_" + SUFFIX)  # offline recipient
    check(tag + "_reg_ab", aid > 0 and bid > 0)
    check(tag + "_login_a", login(ca, aid))
    try:
        os.kill(broker_pid, signal.SIGSTOP)
    except OSError as e:
        check(tag + "_pause", False, str(e))
        return
    check(tag + "_pause", True, "broker SIGSTOP")
    # pause 窗口内灌入离线直发：outbox 事件累积（publish 失败不标 processed →
    # outbox_lag 涨）+ 消息滞留 Pending（consumer 无法消费 → oldest pending 涨）。
    for i in range(OUTBOX_BURST):
        r = send_direct(ca, aid, bid, "%s-%d-%s" % (tag, i, SUFFIX),
                        "kafka pause %d" % i, timeout=8.0)
        check(tag + "_accept_%d" % i, r is not None and r.get("msgid") == 11, str(r))
    # outbox_lag > 60（应很快达到）。
    lag_ge = wait_until(30.0, lambda: (prom_query("reliable_outbox_lag") or 0) > OUTBOX_THRESHOLD)
    check(tag + "_outbox_lag_over_threshold", lag_ge,
          "reliable_outbox_lag must exceed %s during pause" % OUTBOX_THRESHOLD)
    # oldest pending age > 60000（Pause 期间消息滞留 Pending，age 单调涨；~60s 达阈值）。
    pending_ge = wait_until(90.0, lambda:
        (prom_query("reliable_oldest_pending_age_ms") or 0) > PENDING_THRESHOLD_MS)
    print("DBG %s age=%s lag=%s states=%s db=%s" % (tag,
          prom_query("reliable_oldest_pending_age_ms"), prom_query("reliable_outbox_lag"),
          prom_alert_states("P5_DELIVERY_OLDEST_PENDING_STALLED"),
          sql_delivery_states(db)), flush=True)
    check(tag + "_oldest_pending_over_threshold", pending_ge,
          "oldest pending must exceed 60000ms during pause")
    # 保持 pause 至两 alert 均 firing（覆盖 for: 30s）。
    fired_outbox = wait_alert_firing("P5_DELIVERY_OUTBOX_LAG_HIGH", timeout=40.0)
    fired_pending = wait_alert_firing("P5_DELIVERY_OLDEST_PENDING_STALLED", timeout=40.0)
    check(tag + "_alert_outbox_firing", fired_outbox,
          "P5_DELIVERY_OUTBOX_LAG_HIGH must fire during pause window")
    check(tag + "_alert_pending_firing", fired_pending,
          "P5_DELIVERY_OLDEST_PENDING_STALLED must fire during pause window")
    # 恢复 broker。
    try:
        os.kill(broker_pid, signal.SIGCONT)
    except OSError as e:
        check(tag + "_resume", False, str(e))
        return
    check(tag + "_resume", True, "broker CONT")
    # 1) outbox 收敛（事件被消费、processed）→ outbox_lag → 0。
    outbox_drained = wait_until(90.0, lambda:
        (prom_query("reliable_outbox_lag") or 0) == 0.0)
    check(tag + "_outbox_drained", outbox_drained, "outbox_lag must drain to 0 after CONT")
    # 2) 收件人 B 上线 claim 并 ACK 全部离线 Pending → oldest pending 排空 → 自动 resolve。
    cb = V2Client(HOST, V2_PORT)
    check(tag + "_login_b", login(cb, bid))
    seen = set()
    drain_deadline = time.time() + 120.0
    while time.time() < drain_deadline and len(seen) < OUTBOX_BURST:
        d = cb.recv(8.0)
        if is_delivery(d):
            ack(cb, d["message_id"])
            seen.add(d["message_id"])
    check(tag + "_all_delivered_acked", len(seen) >= OUTBOX_BURST,
          "B must receive and ACK all %d offline deliveries (got %d)"
          % (OUTBOX_BURST, len(seen)))
    pending_drained = wait_until(90.0, lambda:
        (prom_query("reliable_oldest_pending_age_ms") or -1) <= 0)
    check(tag + "_pending_drained", pending_drained,
          "oldest pending must drain (<=0) after B online delivery+ACK")
    resolved_outbox = wait_alert_resolved("P5_DELIVERY_OUTBOX_LAG_HIGH", timeout=60.0)
    resolved_pending = wait_alert_resolved("P5_DELIVERY_OLDEST_PENDING_STALLED", timeout=60.0)
    check(tag + "_alert_outbox_resolved", resolved_outbox,
          "outbox alert must auto-resolve after recovery")
    check(tag + "_alert_pending_resolved", resolved_pending,
          "pending alert must auto-resolve after B drains pending")
    ca.close()
    cb.close()
    srv.kill()


def scenario_offline_pending(srv, prom, rnd):
    """passive 占位（无独立故障注入）：离线消息滞留 Pending 的 oldest_pending 断言已并入
    kafka_pause 场景（broker SIGSTOP 使消息滞留 Pending、age 单调涨）。本函数仅登记映射，
    不做任何 fault injection。"""
    check("offline_pending_covered_by_kafka_pause", True,
          "oldest-pending alert driven within kafka_pause (P5_DELIVERY_OLDEST_PENDING_STALLED)")


def main():
    global HOST, V1_PORT, V2_PORT, SERVER_BIN, PROM_BIN, DBMIGRATE_BIN, MIGRATIONS_DIR
    global DB_PREFIX, DB_PW, WORK
    HOST = sys.argv[1]
    V1_PORT = int(sys.argv[2])
    V2_PORT = int(sys.argv[3])
    SERVER_BIN = sys.argv[4]
    PROM_BIN = sys.argv[5]
    DBMIGRATE_BIN = sys.argv[6]
    MIGRATIONS_DIR = sys.argv[7]
    DB_PREFIX = sys.argv[8]
    DB_PW = sys.argv[9]
    WORK = sys.argv[10]
    if len(sys.argv) > 11:
        global ROUNDS
        ROUNDS = int(sys.argv[11])

    srv = ServerHandle(V1_PORT, V2_PORT)
    prom = PromHandle()
    try:
        for r in range(1, ROUNDS + 1):
            print("===== ROUND %d/%d =====" % (r, ROUNDS), flush=True)
            prom.spawn(os.path.join(WORK, "prom_r%d.log" % r))
            check("round%d_prom_ready" % r, prom.ready(), "prometheus start")
            if not prom.ready():
                break
            scenario_control(srv, prom, r)
            scenario_kafka_pause(srv, prom, r)
            scenario_offline_pending(srv, prom, r)
            prom.close()
    finally:
        srv.close()
        prom.close()

    if FAIL:
        print("REHEARSAL_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)), flush=True)
        sys.exit(1)
    print("REHEARSAL_ALL_PASS x%d" % ROUNDS, flush=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
