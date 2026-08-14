#!/usr/bin/env python3
"""P3-10 升级 + 应用回滚演练 process test（docs/tasks/P3-10.md RED 计划 8 / 验证节）。
schema 由 legacy_cutover_rehearsal_test.sh 建立（chat_p310_rehearsal_$$：create +
dbmigrate 0001+0002 + schema checksum 文件-vs-库核对）。

场景（任何失败 exit 1，不 skip）：
  seed：旧 OfflineMessage 快照——direct（u1→u2 两条，其中一条为重复行）、
    group（u1→群 1，离线成员 u3）、坏 JSON、截断 JSON、超界（>500 字节合法 JSON），
    共 6 行（含 3 行不可解析）；
  backfill CLI --dry-run：exit 0，stats 源=6/迁移=3/quarantine=3/skipped=0，
    sourceHash 16 位 hex、dry-run 与实跑一致，不写任何 ledger/checkpoint；
  backfill CLI --run：同上计数守恒（源 = migrated + quarantine），
    legacy:<offline_id> 落库、checkpoint=6；
  重复 --run：sourceRows=0、不增 Message（legacy 幂等）；
  quarantine 可查询（offline_id=4/5/6，reason 非空）；
  新版 ChatServer 启动：u2 登录收到 2 条迁移后 direct 投递、u3 收到 1 条 group 投递，
    unique message_id 守恒（= ChatMessage.legacy:* 的 id 集合）；
  应用回滚演练：旧 P2/P3-07 Release ChatServer 二进制对同一库登录 → 旧路径
    takeOffline 读删 OfflineMessage（u2 行清零）、不破坏新 ledger（ChatMessage 与
    legacy:* 行数不变）、旧服务器存活；旧二进制不可得 → 该子项 WARN 跳过并记录。
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time

HOST = ""
V1 = 0
V2 = 0
SERVER_BIN = ""
BACKFILL_BIN = ""
OLD_SERVER_BIN = ""
DB_NAME = ""
DB_PW = ""
CONFIG = ""
WORK = ""

FAIL = []


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


def collect_deliveries(cli, idle_window=2.0):
    out = []
    deadline = time.time() + 10.0
    while time.time() < deadline:
        d = cli.recv(timeout=idle_window)
        if d is None:
            break
        out.append(d)
    return out


# ---- 真实 MySQL 断言（mysql CLI；失败即抛错 → 驱动 fail-fast）----

def sql_rows(query):
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", DB_NAME,
                        "-e", query], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (query, p.stderr.strip()))
    return [ln.split("\t") for ln in p.stdout.splitlines()]


def sql_exec(query):
    p = subprocess.run(["mysql", "-uroot", "-p" + DB_PW, "-N", "-B", "-D", DB_NAME,
                        "-e", query], capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("sql error: %s\n%s" % (query, p.stderr.strip()))
    return p.stdout


def sql_count(table, where=""):
    q = "SELECT COUNT(*) FROM %s" % table
    if where:
        q += " WHERE " + where
    return int(sql_rows(q)[0][0])


# ---- backfill CLI ----

def run_backfill(mode):
    cmd = [BACKFILL_BIN, mode, "--db", DB_NAME, "--password", DB_PW, "--batch", "2"]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    stats = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            stats[k] = v
    return p, stats


# ---- 服务器进程 harness（spawn + 就绪探测 + 精确 pid kill -9）----

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
    def __init__(self, v1, v2, bin_path=None):
        self.v1 = v1
        self.v2 = v2
        self.bin = bin_path or SERVER_BIN
        self.proc = None
        self.logfile = None

    def spawn(self, config, log_path):
        if self.logfile is not None:
            self.logfile.close()
        self.logfile = open(log_path, "w")
        cmd = ["setarch", "x86_64", "-R", self.bin, HOST, str(self.v1), "--config", config]
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
            print("FAIL rb_server_exit_timeout", flush=True)
            FAIL.append("rb_server_exit_timeout")
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


def seed_data():
    sql_exec("INSERT INTO User(id,name,password) VALUES(1,'u1','pwd'),(2,'u2','pwd'),"
             "(3,'u3','pwd'),(4,'u4','pwd'),(5,'u5','pwd')")
    sql_exec("INSERT INTO AllGroup(id,groupname) VALUES(1,'g1')")
    direct = '{"msgid":6,"id":1,"toid":2,"content":"legacy direct 1"}'
    group = '{"msgid":10,"id":1,"groupid":1,"content":"legacy group 1"}'
    # 超界行：合法 JSON 但 >500 字节（240 × 'é' = 480 字节 + 外壳 ≈ 521 字节）。
    # 以原始字节构造（b"\xc3\xa9" 即 'é' 的 UTF-8 字节），hex 字面量写入免转义。
    oversized = (b'{"msgid":6,"id":1,"toid":2,"content":"'
                 + (b"\xc3\xa9" * 240) + b'"}')
    rows = [
        (1, 2, direct),          # direct u1->u2
        (2, 2, direct),          # 重复行（旧表不去重，两行各自迁移）
        (3, 3, group),           # group 群 1，离线成员 u3
        (4, 4, "this is not json"),
        (5, 4, '{"msgid":6,"id":1,"toid":2,"content":"oops'),
        (6, 5, oversized),       # >500 字节合法 JSON
    ]
    for oid, uid, payload in rows:
        if isinstance(payload, bytes):
            sql_exec("INSERT INTO OfflineMessage(id,userid,message) VALUES(%d,%d,0x%s)"
                     % (oid, uid, payload.hex()))
        else:
            sql_exec("INSERT INTO OfflineMessage(id,userid,message) VALUES(%d,%d,'%s')"
                     % (oid, uid, payload.replace("'", "''")))


def main():
    global HOST, V1, V2, SERVER_BIN, BACKFILL_BIN, OLD_SERVER_BIN
    global DB_NAME, DB_PW, CONFIG, WORK
    HOST = sys.argv[1]
    V1 = int(sys.argv[2])
    V2 = int(sys.argv[3])
    SERVER_BIN = sys.argv[4]
    BACKFILL_BIN = sys.argv[5]
    DB_NAME = sys.argv[6]
    DB_PW = sys.argv[7]
    OLD_SERVER_BIN = sys.argv[8]
    CONFIG = sys.argv[9]
    WORK = sys.argv[10]

    LOG_NEW = os.path.join(WORK, "server_new.log")
    LOG_OLD = os.path.join(WORK, "server_old.log")
    new_server = ServerHandle(V1, V2)
    old_server = ServerHandle(V1, V2, bin_path=OLD_SERVER_BIN)
    clients = []

    def new_client(port):
        c = V1Client(HOST, port)
        clients.append(c)
        return c

    try:
        # ================= Phase 0：种旧 OfflineMessage 快照 =================
        seed_data()
        check("seed_users_group", sql_count("User") == 5 and sql_count("AllGroup") == 1)
        check("seed_offline_6", sql_count("OfflineMessage") == 6, sql_count("OfflineMessage"))

        # ================= Phase 1：backfill CLI --dry-run =================
        p, dry = run_backfill("--dry-run")
        check("bf_dry_exit0", p.returncode == 0, p.stderr.strip())
        check("bf_dry_stats",
              dry.get("sourceRows") == "6" and dry.get("migrated") == "3"
              and dry.get("quarantined") == "3"
              and dry.get("skippedIdempotent") == "0", str(dry))
        check("bf_dry_hash", len(dry.get("sourceHash", "")) == 16, dry.get("sourceHash"))
        check("bf_dry_checkpoint0", dry.get("checkpoint") == "0", str(dry))
        # dry-run 不写任何 ledger/checkpoint。
        check("bf_dry_no_ledger",
              sql_count("ChatMessage") == 0 and sql_count("MessageDelivery") == 0
              and sql_count("Conversation") == 0, "")

        # ================= Phase 2：backfill CLI --run =================
        p, run = run_backfill("--run")
        check("bf_run_exit0", p.returncode == 0, p.stderr.strip())
        check("bf_run_counts_match_dry",
              run.get("sourceRows") == dry.get("sourceRows")
              and run.get("migrated") == dry.get("migrated")
              and run.get("quarantined") == dry.get("quarantined")
              and run.get("skippedIdempotent") == dry.get("skippedIdempotent"), str(run))
        check("bf_run_hash_stable", run.get("sourceHash") == dry.get("sourceHash"),
              "%s vs %s" % (run.get("sourceHash"), dry.get("sourceHash")))
        # 守恒：源行 = migrated + quarantine。
        check("bf_run_conservation",
              int(run["sourceRows"]) == int(run["migrated"]) + int(run["quarantined"]), str(run))
        check("bf_run_checkpoint", run.get("checkpoint") == "6", str(run))
        # ledger：3 条 ChatMessage，幂等键 legacy:<offline_id>。
        check("bf_ledger_count", sql_count("ChatMessage") == 3, sql_count("ChatMessage"))
        cmids = sorted(r[0] for r in sql_rows(
            "SELECT client_message_id FROM ChatMessage ORDER BY client_message_id"))
        check("bf_legacy_ids", cmids == ["legacy:1", "legacy:2", "legacy:3"], str(cmids))
        # quarantine 可查询：offline_id=4/5/6，reason 非空。
        check("bf_quarantine_count", sql_count("OfflineBackfillQuarantine") == 3,
              sql_count("OfflineBackfillQuarantine"))
        qrows = sql_rows("SELECT offline_id, reason FROM OfflineBackfillQuarantine "
                         "ORDER BY offline_id")
        check("bf_quarantine_ids", [r[0] for r in qrows] == ["4", "5", "6"], str(qrows))
        check("bf_quarantine_reasons", all(len(r[1]) > 0 for r in qrows), str(qrows))

        # ================= Phase 3：重复运行不增 Message（幂等）=================
        p, r2 = run_backfill("--run")
        check("bf_rerun_exit0", p.returncode == 0, p.stderr.strip())
        check("bf_rerun_no_new",
              r2.get("sourceRows") == "0" and r2.get("migrated") == "0"
              and r2.get("quarantined") == "0", str(r2))
        check("bf_rerun_message_stable", sql_count("ChatMessage") == 3,
              sql_count("ChatMessage"))

        # ================= Phase 4：新版 ChatServer，登录收迁移离线消息 =================
        new_server.spawn(CONFIG, LOG_NEW)
        check("up_server_ready", new_server.ready(LOG_NEW), log_tail(LOG_NEW))
        mids = {cmid: int(mid) for mid, cmid in
                sql_rows("SELECT id, client_message_id FROM ChatMessage")}
        expected = {"legacy:1": mids["legacy:1"], "legacy:2": mids["legacy:2"],
                    "legacy:3": mids["legacy:3"]}

        cu2 = new_client(V1)
        check("up_login_u2", login(cu2, 2), "")
        du2 = collect_deliveries(cu2)
        check("up_u2_delivery_count", len(du2) == 2, str([d.get("content") for d in du2]))
        u2_ids = [d["message_id"] for d in du2]
        check("up_u2_ids_unique",
              len(set(u2_ids)) == 2 and set(u2_ids) == {expected["legacy:1"],
                                                        expected["legacy:2"]}, str(u2_ids))
        check("up_u2_contents",
              sorted(d.get("content") for d in du2) == ["legacy direct 1", "legacy direct 1"],
              str([d.get("content") for d in du2]))

        cu3 = new_client(V1)
        check("up_login_u3", login(cu3, 3), "")
        du3 = collect_deliveries(cu3)
        check("up_u3_delivery_count", len(du3) == 1, str([d.get("content") for d in du3]))
        check("up_u3_delivery",
              is_delivery(du3[0]) and du3[0]["message_id"] == expected["legacy:3"]
              and du3[0]["content"] == "legacy group 1", str(du3[0] if du3 else None))
        check("up_new_server_alive", new_server.alive(), log_tail(LOG_NEW))

        # ================= Phase 5：应用回滚演练（旧二进制对同一库）=================
        chat_before = sql_count("ChatMessage")
        legacy_before = sql_count("ChatMessage", "client_message_id LIKE 'legacy:%'")
        offline_u2_before = sql_count("OfflineMessage", "userid=2")
        new_server.kill()
        if not os.path.exists(OLD_SERVER_BIN):
            print("WARN legacy-cutover: 旧二进制不可得 %s，应用回滚演练子项跳过（卡记录）"
                  % OLD_SERVER_BIN, flush=True)
        else:
            old_server.spawn(CONFIG, LOG_OLD)
            check("rb_old_server_ready", old_server.ready(LOG_OLD), log_tail(LOG_OLD))
            check("rb_pre_state", chat_before == 3 and legacy_before == 3
                  and offline_u2_before == 2,
                  "chat=%s legacy=%s offline_u2=%s" % (chat_before, legacy_before,
                                                       offline_u2_before))
            cou2 = new_client(V1)
            check("rb_old_login_u2", login(cou2, 2), "")
            # 旧路径 takeOffline 读删旧表：u2 的旧 OfflineMessage 行被清空。
            check("rb_old_takeoffline_deleted", sql_count("OfflineMessage", "userid=2") == 0,
                  sql_count("OfflineMessage", "userid=2"))
            # 新 ledger 不被旧路径破坏：ChatMessage / legacy:* 行数不变。
            check("rb_ledger_message_unchanged", sql_count("ChatMessage") == chat_before,
                  "%s vs %s" % (sql_count("ChatMessage"), chat_before))
            check("rb_ledger_legacy_unchanged",
                  sql_count("ChatMessage", "client_message_id LIKE 'legacy:%'") == legacy_before,
                  sql_count("ChatMessage", "client_message_id LIKE 'legacy:%'"))
            check("rb_old_server_alive", old_server.alive(), log_tail(LOG_OLD))
            old_server.kill()
    finally:
        new_server.close()
        old_server.close()
        for c in clients:
            c.close()

    if FAIL:
        print("LEGACY_CUTOVER_REHEARSAL_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)))
        sys.exit(1)
    print("LEGACY_CUTOVER_REHEARSAL_ALL_PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
