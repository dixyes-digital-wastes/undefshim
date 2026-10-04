#!/usr/bin/env python3
"""Read words from a stopped machine, by physical address.

Usage: readmem.py --addr 0x1234 [--count 8] [--qmp-port 4448]
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
    ap.add_argument("--addr", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--physical", action="store_true",
                    help="read by physical address instead of the virtual view")
    args = ap.parse_args()

    s = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = s.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})
    unit = "xp" if args.physical else "x"
    out = cmd(f, {"execute": "human-monitor-command",
                  "arguments": {"command-line":
                                "%s /%dgx 0x%x" % (unit, args.count, args.addr)}}).get("return", "")
    if out.strip() == "" or "Cannot access memory" in out:
        print("(nothing readable at 0x%x in the %s view)"
              % (args.addr, "physical" if args.physical else "virtual"))
        return 1
    for line in out.splitlines():
        if ":" not in line:
            continue
        addr, rest = line.split(":", 1)
        words = re.findall(r"0x([0-9a-f]+)", rest)
        print("%s : %s" % (addr.strip(), " ".join("0x%016x" % int(w, 16) for w in words)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
