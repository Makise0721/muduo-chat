#!/usr/bin/env python3
"""P5-02 statistics: mean + 95% CI (t distribution) + CV per scenario.

Reads a bench-result-v1 JSON (single scenario object or array of them) and prints
a table. Also usable as a library from run.py.

  stats.py <result.json> [--json-out]
"""
import json
import math
import sys

# t_{0.975, df} for df = n-1 (n = repetitions). 95% two-sided.
_T95 = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
    8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145,
    15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086,
    21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060, 26: 2.056,
    27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042,
}


def t95(df):
    if df <= 0:
        return 0.0
    return _T95.get(df, 1.96)


def summarize(values):
    """Return {mean, ci95_low, ci95_high, cv} for a non-empty list of numbers."""
    n = len(values)
    if n == 0:
        return {"mean": 0.0, "ci95_low": 0.0, "ci95_high": 0.0, "cv": 0.0}
    mean = sum(values) / n
    if n == 1:
        s = 0.0
    else:
        s = math.sqrt(sum((v - mean) ** 2 for v in values) / (n - 1))
    se = s / math.sqrt(n)
    half = t95(n - 1) * se
    cv = (s / mean) if mean != 0 else 0.0
    return {"mean": mean,
            "ci95_low": mean - half,
            "ci95_high": mean + half,
            "cv": cv}


def compute_stats(result):
    """Attach 'stats' to a single scenario result object (in place), return it."""
    reps = result.get("repetitions", [])
    keys = ["msg_per_sec", "p50_ms", "p95_ms", "p99_ms", "rss_kb"]
    if reps:
        for k in keys:
            result["stats"][k] = summarize([r.get(k, 0.0) for r in reps])
    return result


def print_table(scenarios):
    hdr = ["scenario", "n", "msg/s", "95%CI low", "95%CI high", "CV",
           "p50", "p95", "p99", "RSS(KB)"]
    print("%-16s %3s %9s %10s %10s %7s %8s %8s %8s %10s"
          % (hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
             hdr[6], hdr[7], hdr[8], hdr[9]))
    for sc in scenarios:
        st = sc.get("stats", {})
        m = st.get("msg_per_sec", {})
        n = len(sc.get("repetitions", []))
        print("%-16s %3d %9.2f %10.2f %10.2f %7.3f %8.2f %8.2f %8.2f %10d"
              % (sc["scenario"], n,
                 m.get("mean", 0.0), m.get("ci95_low", 0.0),
                 m.get("ci95_high", 0.0), m.get("cv", 0.0),
                 st.get("p50_ms", {}).get("mean", 0.0),
                 st.get("p95_ms", {}).get("mean", 0.0),
                 st.get("p99_ms", {}).get("mean", 0.0),
                 int(st.get("rss_kb", {}).get("mean", 0.0))))


def main():
    if len(sys.argv) < 2:
        print("usage: stats.py <result.json> [--json-out]")
        return 2
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        data = json.load(f)
    scenarios = data if isinstance(data, list) else [data]
    scenarios = [compute_stats(s) for s in scenarios]
    if "--json-out" in sys.argv:
        print(json.dumps(scenarios if isinstance(data, list) else scenarios[0],
                         indent=2))
    else:
        print_table(scenarios)
    return 0


if __name__ == "__main__":
    sys.exit(main())