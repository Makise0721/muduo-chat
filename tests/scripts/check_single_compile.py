#!/usr/bin/env python3
"""Assert each mymuduo production .cc is compiled exactly once.

Reads a compile_commands.json and fails if any mymuduo/*.cc has more than one
compile entry (i.e. a test target recompiles production code instead of linking
the mymuduo library target).
"""

import json
import os
import sys


def main(argv):
    if len(argv) != 2:
        print("usage: check_single_compile.py <compile_commands.json>")
        return 2
    with open(argv[1], encoding="utf-8") as f:
        entries = json.load(f)
    per_source = {}
    for entry in entries:
        src = entry.get("file", "")
        if os.path.sep + "mymuduo" + os.path.sep in src and src.endswith(".cc"):
            per_source.setdefault(src, []).append(entry.get("output", "?"))
    dup = {src: outs for src, outs in per_source.items() if len(outs) > 1}
    if dup:
        for src, outs in sorted(dup.items()):
            print("DUPLICATE_COMPILE %s -> %d entries: %s" % (src, len(outs), outs))
        print("FAIL: %d production source(s) compiled more than once" % len(dup))
        return 1
    print("OK: %d production source(s), all compiled exactly once" % len(per_source))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
