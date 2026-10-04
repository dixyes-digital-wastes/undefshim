#!/usr/bin/env python3
"""Print every processor's registers from a stopped machine, by exception level.

Usage: allregs.py [--qmp-port 4448] [--grep ELR|ESR|FAR]
"""

import argparse
import json
import re
import socket
import sys


def cmd(f, obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise SystemExit("the monitor closed the connection")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--grep", default="")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    s = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = s.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})
    out = cmd(f, {"execute": "human-monitor-command",
                  "arguments": {"command-line": "info registers"}}).get("return", "")

    want = re.compile(args.grep, re.I) if args.grep else None
    if args.all:
        print(out)
        return 0
    for line in out.splitlines():
        if re.match(r"\s*(CPU#|ELR|ESR|FAR|SPSR|VBAR|TTBR|SCTLR|PC=)", line):
            if want and not want.search(line) and "CPU#" not in line:
                continue
            print(line.rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
