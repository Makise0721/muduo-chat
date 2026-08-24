#!/usr/bin/env python3
"""P5-02 stats wrapper: mean + 95% CI (t distribution) + CV per scenario.

Reads a bench-result-v1 JSON and prints a table. Implementation lives in
tools/bench/stats.py; this wrapper keeps the tests/scripts entry point.
"""
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools", "bench"))
from stats import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())