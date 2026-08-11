#!/usr/bin/env python3
"""P2-10 chat load benchmark: concurrent reg -> login -> offline one-chat loop.

Each worker: register unique user, login (wait ACK), then loop oneChat to a
fixed offline target (wait ACK per message). Reports throughput and latency
percentiles. One line per run:

  conns=N duration_ms=T ok=F msg_per_sec=P p50_ms=.. p95_ms=.. p99_ms=.. errors=E
"""
import json
import socket
import sys
import threading
import time

USAGE = "usage: chat_load.py <host> <port> <conns> <duration_ms>"


def send_line(sock, obj):
    sock.sendall(json.dumps(obj).encode() + b"\n")


def recv_line(sock, timeout):
    sock.settimeout(timeout)
    buf = b""
    while b"\n" not in buf:
        try:
            chunk = sock.recv(4096)
        except (socket.timeout, ConnectionResetError, OSError):
            return None
        if not chunk:
            return None
        buf += chunk
    line, _ = buf.split(b"\n", 1)
    try:
        return json.loads(line)
    except ValueError:
        return None


def worker(host, port, worker_id, target_id, duration_ms, out, lock):
    try:
        sock = socket.create_connection((host, port), timeout=10)
    except OSError:
        with lock:
            out["errors"] += 1
        return
    name = "load_%d_%d" % (time.process_time_ns() % 1000000, worker_id)
    try:
        send_line(sock, {"msgid": 4, "name": name, "password": "123456"})
    except (socket.timeout, OSError):
        with lock:
            out["errors"] += 1
        sock.close()
        return
    ack = recv_line(sock, 10)
    if ack is None or ack.get("msgid") != 5 or ack.get("errno") != 0:
        with lock:
            out["errors"] += 1
        sock.close()
        return
    my_id = ack.get("id", -1)
    try:
        send_line(sock, {"msgid": 1, "id": my_id, "password": "123456"})
    except (socket.timeout, OSError):
        with lock:
            out["errors"] += 1
        sock.close()
        return
    ack = recv_line(sock, 10)
    if ack is None or ack.get("msgid") != 2 or ack.get("errno") != 0:
        with lock:
            out["errors"] += 1
        sock.close()
        return

    deadline = time.time() + duration_ms / 1000.0
    latencies = []
    ok = 0
    while time.time() < deadline:
        start = time.time()
        try:
            send_line(sock, {"msgid": 6, "id": my_id, "toid": target_id,
                             "msg": "bench payload"})
        except (socket.timeout, OSError):
            with lock:
                out["errors"] += 1
            sock.close()
            return
        ack = recv_line(sock, 10)
        if ack is None or ack.get("errno") != 0:
            with lock:
                out["errors"] += 1
            break
        latencies.append((time.time() - start) * 1000.0)
        ok += 1
    sock.close()
    with lock:
        out["ok"] += ok
        out["latencies"].extend(latencies)


def percentile(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    idx = int((len(values) - 1) * p)
    return values[idx]


def main():
    if len(sys.argv) != 5:
        print(USAGE)
        return 2
    host, port = sys.argv[1], int(sys.argv[2])
    conns, duration_ms = int(sys.argv[3]), int(sys.argv[4])

    target_name = "load_target_%d" % (time.time_ns() % 1000000)
    probe = socket.create_connection((host, port), timeout=10)
    send_line(probe, {"msgid": 4, "name": target_name, "password": "123456"})
    ack = recv_line(probe, 10)
    if ack is None or ack.get("msgid") != 5 or ack.get("errno") != 0:
        print("FAIL: cannot register target user: %r" % (ack,))
        probe.close()
        return 1
    target_id = ack.get("id", -1)
    probe.close()

    out = {"ok": 0, "errors": 0, "latencies": []}
    lock = threading.Lock()
    threads = [
        threading.Thread(target=worker,
                         args=(host, port, i, target_id, duration_ms, out, lock))
        for i in range(conns)
    ]
    start = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed_ms = max(1.0, (time.time() - start) * 1000.0)

    lat = out["latencies"]
    print("conns=%d duration_ms=%d ok=%d msg_per_sec=%.1f "
          "p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f errors=%d"
          % (conns, int(elapsed_ms), out["ok"],
             out["ok"] * 1000.0 / elapsed_ms,
             percentile(lat, 0.50), percentile(lat, 0.95), percentile(lat, 0.99),
             out["errors"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
