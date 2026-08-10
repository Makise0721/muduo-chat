#!/usr/bin/env python3
"""fd-exhaustion client for the process-level CTest (P1R-06).

Connects as many sockets as the server's fd limit allows, then verifies the
server survives the EMFILE path and recovers once half the connections are
released (idle-fd accept restore).
"""

import socket
import sys
import time

PORT = 6000


def wait_port(timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def main():
    if not wait_port():
        print("FAIL: server did not bind")
        return 1

    conns = []
    for _ in range(200):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            conns.append(s)
        except OSError:
            break
    time.sleep(0.5)

    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
        s.close()
    except OSError:
        pass

    time.sleep(0.5)
    half = len(conns) // 2
    for s in conns[:half]:
        s.close()
    time.sleep(1.5)

    ok = 0
    for _ in range(5):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            s.close()
            ok += 1
        except OSError:
            break

    for s in conns:
        try:
            s.close()
        except OSError:
            pass

    print("CONNECTED=%d RECOVERED=%d" % (len(conns), ok))
    if len(conns) < 40:
        print("FAIL: expected fd exhaustion near the limit, only %d connections" % len(conns))
        return 1
    if ok < 3:
        print("FAIL: server did not recover accept after fd release")
        return 1
    print("FD_EXHAUSTION_PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
