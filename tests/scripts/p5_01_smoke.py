#!/usr/bin/env python3
"""P5-01 tarball-equivalent smoke: start prometheus (tarball, --config.file=repo
prometheus.yml) + ChatServer (metrics.enabled=true port=9095) + grafana (tarball,
provisioning -> repo dir), then assert:
  - prometheus /api/v1/targets: chat_metrics target UP
  - /api/v1/query: reliable_accepts present (metrics scraped)
  - grafana /api/health ok + dashboard chat-overview loaded (provisioning)
Exit 0 iff all pass; else 1 (never skip). Logs tee'd by the .sh wrapper.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time

FAIL = []
HOST = "127.0.0.1"
V1_PORT = 0
V2_PORT = 0
METRICS_PORT = 9095
SERVER_BIN = ""
PROM_BIN = ""
GRAFANA_BIN = ""
DB_NAME = ""
DB_PW = ""
WORK = ""
ROOT = ""
SUFFIX = str(int(time.time() * 1000))[-8:]


def check(name, cond, detail=""):
    if cond:
        print("PASS %s" % name, flush=True)
    else:
        print("FAIL %s %s" % (name, detail), flush=True)
        FAIL.append(name)


def _port_ready(host, port):
    try:
        with socket.create_connection((host, port), timeout=1.0):
            return True
    except OSError:
        return False


class ServerHandle(object):
    def __init__(self, v1, v2):
        self.v1 = v1
        self.v2 = v2
        self.proc = None
        self.logfile = None

    def spawn(self, config, log_path):
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


class PromHandle(object):
    def __init__(self):
        self.proc = None
        self.logfile = None

    def spawn(self, log_path):
        if self.logfile is not None:
            self.logfile.close()
        self.logfile = open(log_path, "w")
        cfg = os.path.join(ROOT, "docker", "prometheus", "prometheus.yml")
        storage = os.path.join(WORK, "prom-tsdb")
        cmd = [PROM_BIN, "--config.file=" + cfg,
               "--storage.tsdb.path=" + storage,
               "--web.listen-address=127.0.0.1:9090",
               "--storage.tsdb.retention.time=1h"]
        self.proc = subprocess.Popen(cmd, stdout=self.logfile, stderr=subprocess.STDOUT,
                                     env=dict(os.environ))
        return self.proc

    def ready(self, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if _port_ready(HOST, 9090):
                return True
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


def http_json(url, auth=None):
    import base64
    import urllib.request
    try:
        req = urllib.request.Request(url)
        if auth:
            token = base64.b64encode(auth.encode("utf-8")).decode("ascii")
            req.add_header("Authorization", "Basic " + token)
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:  # noqa: BLE001
        return None


def wait_until(timeout, fn, pause=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(pause)
    return False


def main():
    global V1_PORT, V2_PORT, SERVER_BIN, PROM_BIN, GRAFANA_BIN, DB_NAME, DB_PW, WORK, ROOT
    SERVER_BIN = sys.argv[1]
    PROM_BIN = sys.argv[2]
    GRAFANA_BIN = sys.argv[3]
    V1_PORT = int(sys.argv[4])
    V2_PORT = int(sys.argv[5])
    DB_NAME = sys.argv[6]
    DB_PW = sys.argv[7]
    WORK = sys.argv[8]
    ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

    cfg = {
        "server": {"v1": {"ip": HOST, "port": V1_PORT, "threads": 2},
                   "v2": {"port": V2_PORT}},
        "db": {"host": "127.0.0.1", "port": 3306, "user": "root",
               "password": DB_PW, "dbname": DB_NAME, "pool_size": 4},
        "executor": {"workers": 2, "queue_capacity": 64},
        "gateway": {"id": 1,
                    "presence": {"host": "127.0.0.1", "port": 6379, "db": 0,
                                 "ttl_ms": 30000, "connect_timeout_ms": 1000,
                                 "command_timeout_ms": 1000},
                    "kafka": {"host": "127.0.0.1", "port": 9092},
                    "consumer": {"topic": "muduo-p5-smoke-%s" % SUFFIX,
                                 "fetch_batch_limit": 100, "poll_deadline_ms": 5000}},
        "metrics": {"enabled": True, "port": METRICS_PORT},
    }
    cfg_path = os.path.join(WORK, "smoke.json")
    with open(cfg_path, "w") as f:
        json.dump(cfg, f)

    srv = ServerHandle(V1_PORT, V2_PORT)
    prom = PromHandle()
    grafana_proc = None
    grafana_log = None
    try:
        prom.spawn(os.path.join(WORK, "prom.log"))
        check("smoke_prom_ready", prom.ready())
        if not prom.ready():
            return
        srv.spawn(cfg_path, os.path.join(WORK, "server.log"))
        check("smoke_server_ready", srv.ready(os.path.join(WORK, "server.log")))
        if not srv.ready(os.path.join(WORK, "server.log")):
            return

        # prometheus target UP + metric present (scrape).
        def target_up():
            d = http_json("http://127.0.0.1:9090/api/v1/targets")
            if not d:
                return False
            for t in d.get("data", {}).get("activeTargets", []):
                if t.get("labels", {}).get("job") == "chat_metrics" \
                        and t.get("health") == "up":
                    return True
            return False
        check("smoke_target_up", wait_until(40.0, target_up), "chat_metrics target UP")

        def metric_present():
            d = http_json("http://127.0.0.1:9090/api/v1/query?query=reliable_accepts")
            return bool(d and d.get("data", {}).get("result"))
        check("smoke_metric_scraped", wait_until(40.0, metric_present),
              "reliable_accepts present in TSDB")

        # grafana：启动（provisioning 经 config override 指向 WORK 副本，副本内
        # dashboard provider path 重写为仓库 dashboards 目录），/api/health +
        # dashboard 加载。Grafana v13 无 --provisioning 旗标，走 paths.* override。
        if not _port_ready(HOST, 3000):
            import shutil
            prov_src = os.path.join(ROOT, "docker", "grafana", "provisioning")
            prov_dir = os.path.join(WORK, "grafana-provisioning")
            shutil.copytree(prov_src, prov_dir)
            dash_src = os.path.join(ROOT, "docker", "grafana", "dashboards")
            dash_dir = os.path.join(WORK, "grafana-dashboards")
            shutil.copytree(dash_src, dash_dir)
            provider = os.path.join(prov_dir, "dashboards", "dashboard-provider.yml")
            with open(provider, "r") as f:
                text = f.read()
            text = text.replace("/etc/grafana/dashboards", dash_dir)
            with open(provider, "w") as f:
                f.write(text)
            home = os.path.normpath(os.path.join(os.path.dirname(GRAFANA_BIN), ".."))
            data_dir = os.path.join(WORK, "grafana-data")
            os.makedirs(data_dir, exist_ok=True)
            ini = os.path.join(WORK, "grafana.ini")
            with open(ini, "w") as f:
                f.write(
                    "[server]\n"
                    "http_addr = 127.0.0.1\n"
                    "http_port = 3000\n"
                    "[paths]\n"
                    "provisioning = %s\n"
                    "data = %s\n"
                    "[security]\n"
                    "admin_user = admin\n"
                    "admin_password = admin\n" % (prov_dir, data_dir))
            grafana_log = open(os.path.join(WORK, "grafana.log"), "w")
            cmd = [GRAFANA_BIN, "server", "--homepath=" + home, "--config=" + ini]
            grafana_proc = subprocess.Popen(cmd, stdout=grafana_log,
                                            stderr=subprocess.STDOUT,
                                            env=dict(os.environ))

        def grafana_health():
            d = http_json("http://127.0.0.1:3000/api/health")
            return bool(d and d.get("database") == "ok")
        check("smoke_grafana_health", wait_until(60.0, grafana_health), "grafana /api/health")

        def dashboard_loaded():
            d = http_json("http://127.0.0.1:3000/api/search?query=Chat%20Overview",
                          auth="admin:admin")
            if not d:
                return False
            for item in d:
                if item.get("uid") == "chat-overview":
                    return True
            return False
        check("smoke_dashboard_loaded", wait_until(30.0, dashboard_loaded),
              "chat-overview dashboard via provisioning")
    finally:
        srv.close()
        prom.close()
        if grafana_proc is not None and grafana_proc.poll() is None:
            os.kill(grafana_proc.pid, signal.SIGKILL)
            try:
                grafana_proc.wait(10)
            except subprocess.TimeoutExpired:
                pass
        if grafana_log is not None:
            grafana_log.close()

    if FAIL:
        print("SMOKE_FAIL %d: %s" % (len(FAIL), ",".join(FAIL)), flush=True)
        sys.exit(1)
    print("SMOKE_ALL_PASS", flush=True)
    sys.exit(0)


if __name__ == "__main__":
    main()
