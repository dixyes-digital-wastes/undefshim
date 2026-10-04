#!/usr/bin/env python3
"""Sample the machine while it runs and say whether it is making progress.

A machine that is stuck in a retry loop and one that is slowly working through
a boot look the same from a single look. Sampling the program counter and the
payload's count of carried-out loads together separates them: a growing count
means exceptions are being taken and returned from, so the kernel is running;
a static count with a moving program counter means the kernel is running
without ever trapping again, which is a different thing again.

Usage: progress.py [--seconds 60] [--every 5] [--work build/prog]
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pool_dump                                       # noqa: E402  (offsets)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def cmd(f, obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise OSError("the monitor closed the connection")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def hmp(f, c):
    return cmd(f, {"execute": "human-monitor-command",
                   "arguments": {"command-line": c}}).get("return", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--every", type=float, default=5)
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--work", default="build/prog")
    ap.add_argument("--find-timeout", type=float, default=90)
    ap.add_argument("--attach", action="store_true",
                    help="sample a machine that is already running instead of "
                         "starting one")
    args = ap.parse_args()

    os.chdir(ROOT)
    if not args.attach:
        subprocess.run(["python3", "-u", "tests/deploy/bugcheck_probe.py",
                        "--arm", "true", "--rewrite", "false",
                        "--find-timeout", str(args.find_timeout),
                        "--catch-timeout", "2", "--keep", "--work", args.work])

    f = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20).makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    text = open(os.path.join(args.work, "serial.log"), "rb").read().decode("latin1")
    m = re.search(r"pool: pa=0x([0-9a-f]+)", text)
    pool = int(m.group(1), 16) if m else None

    def word(addr):
        out = hmp(f, "xp /1gx 0x%x" % addr)
        m2 = re.search(r":\s*0x([0-9a-f]+)", out)
        return int(m2.group(1), 16) if m2 else None

    def entries():
        return word(pool + 8 + 8)                       # magic then entries

    pc = re.compile(r"CPU#(\d+)\s*\n\s*PC=([0-9a-f]+)")
    start = time.monotonic()
    last = None
    while time.monotonic() - start < args.seconds:
        out = hmp(f, "info registers -a")
        pcs = [int(mm.group(2), 16) for mm in pc.finditer(out)]
        n = entries()
        # The rewrite count is the other half of the picture: a machine whose
        # entries stop growing because sites are being replaced is working,
        # and one whose entries never grew at all is not.
        rewritten = None
        if pool is not None:
            offsets = pool_dump.entryOffsets()
            rewritten = word(pool + 8 + offsets["rewriteWritten"])
        moving = "start" if last is None else ("+" if n != last else "  ")
        print("%5.0fs  entries=%-10s %s  rewritten=%-8s pc0=0x%x  busy=%d"
              % (time.monotonic() - start, n, moving, rewritten,
                 pcs[0] if pcs else 0, sum(1 for p in pcs if p)), flush=True)
        last = n
        time.sleep(args.every)
    return 0


if __name__ == "__main__":
    sys.exit(main())
