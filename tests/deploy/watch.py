#!/usr/bin/env python3
"""Watch a running machine and say whether it is moving, and how.

A machine that has stopped and one that is slowly working look the same from
outside: the serial port is silent for both, and the screen shows whatever was
drawn last for both. Two readings together tell them apart.

  the program counters   advance while the machine is doing anything at all,
                         and stop when it is not. This is the reading that
                         separates working from halted, and it is taken for
                         every processor: one that is waiting on another is
                         not a machine that has stopped.
  the pool's counters    say what the payload has been asked to do -- how many
                         exceptions it carried out, how many instructions it
                         replaced where they stood, and whether it ever gave
                         up with no destination. A count that grows is a
                         machine making progress through the work this project
                         exists for; a count that never grew at all is a
                         different thing from a machine that is merely slow.

This is what the earlier separate probes each answered a part of, and it is
one tool because the parts are only meaningful together: "the PC is moving" is
not progress if the payload never ran, and "the payload ran" is not progress if
the PC has not moved since.

The machine is not started or stopped here. Hand it the monitor port of a run
that is already up -- run.sh with KEEP=1 leaves one -- and it watches that.

Exit codes: 3 halted, 1 still going when the time ran out, 2 the machine went
away.
"""

import argparse
import json
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pool_dump                                       # noqa: E402  (offsets)


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


def hmp(f, command):
    return cmd(f, {"execute": "human-monitor-command",
                   "arguments": {"command-line": command}}).get("return", "")


def processors(f):
    """Every processor's program counter."""
    out = hmp(f, "info registers -a")
    return tuple(int(m.group(2), 16)
                 for m in re.finditer(r"CPU#(\d+)\s*\n\s*PC=([0-9a-f]+)", out))


def connect(port, timeout):
    """The monitor, or None when there is nothing to talk to.

    A machine that has reset is a machine whose monitor socket is gone, and
    that is an answer rather than a failure: it is what a triple fault looks
    like from outside.
    """
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        sock.settimeout(timeout)
        f = sock.makefile("rwb")
        f.readline()
        cmd(f, {"execute": "qmp_capabilities"})
        return sock, f
    except (OSError, SystemExit, TimeoutError):
        return None, None


def poolCounters(f, pool):
    """The few numbers in the pool that say whether work is being done.

    Read one field at a time rather than the whole record: this runs between
    samples, and the rings are kilobytes that nothing here looks at.
    """
    if not pool:
        return None
    offsets = pool_dump.entryOffsets()
    at = pool + 8                                          # past the magic
    want = {"entries": 8, "handled": 16, "handedBack": 24, "stuck": 48,
            "stuckKind": 56, "rewriteWritten": offsets["rewriteWritten"]}
    told = hmp(f, "xp /%dgx 0x%x" % (max(want.values()) // 8 + 1, at))
    words = {}
    for line in told.splitlines():
        if ":" not in line:
            continue
        addr, rest = line.split(":", 1)
        base = int(addr, 16) - at
        for i, v in enumerate(rest.split()):
            words[base + i * 8] = int(v, 16)
    return {name: words.get(off) for name, off in want.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--frozen", type=int, default=8,
                    help="consecutive unchanged samples that mean halted")
    ap.add_argument("--report", type=int, default=5,
                    help="report every this many samples")
    ap.add_argument("--serial", help="a run's serial log, to find the pool in")
    ap.add_argument("--pool", help="the pool's physical address, if known")
    args = ap.parse_args()

    sock, f = connect(args.qmp_port, 20)
    if sock is None:
        print("the machine is not there to watch", flush=True)
        return 2

    pool = int(args.pool, 0) if args.pool else None
    if pool is None and args.serial:
        pool = pool_dump.poolFromSerial(args.serial)
    if pool is None:
        print("no pool address: watching the processors alone", flush=True)

    deadline = time.monotonic() + args.timeout
    started = time.monotonic()
    n = 0
    last = None
    lastCounters = None
    unchanged = 0

    while time.monotonic() < deadline:
        n += 1
        elapsed = time.monotonic() - started

        try:
            pc = processors(f)
            counters = poolCounters(f, pool)
        except (SystemExit, OSError):
            print("the machine went away after %.0fs" % elapsed, flush=True)
            return 2

        if last is not None and pc == last:
            unchanged += 1
        else:
            unchanged = 0
        last = pc

        grew = ""
        if counters and lastCounters:
            d = counters["entries"] - lastCounters["entries"]
            grew = " entries%+d" % d
        lastCounters = counters

        if unchanged >= args.frozen:
            print("%4.0fs: frame %d, every processor has been at the same place "
                  "for %d samples -- halted" % (elapsed, n, unchanged), flush=True)
            for i, p in enumerate(pc):
                print("  cpu%d pc = 0x%x" % (i, p), flush=True)
            if counters:
                print("  pool: entries=%s handled=%s handedBack=%s stuck=%s/%s"
                      % (counters["entries"], counters["handled"],
                         counters["handedBack"], counters["stuck"],
                         counters["stuckKind"]), flush=True)
            return 3

        if n <= 1 or n % args.report == 0:
            extra = ""
            if counters:
                extra = ("  entries=%s rewritten=%s stuck=%s%s"
                         % (counters["entries"], counters["rewriteWritten"],
                            counters["stuck"], grew))
            print("%4.0fs: frame %d, pc %s%s%s"
                  % (elapsed, n,
                     " ".join("0x%x" % p for p in pc[:2]),
                     (" (+%d more)" % (len(pc) - 2)) if len(pc) > 2 else "",
                     extra), flush=True)
        time.sleep(args.interval)

    print("still going when the time ran out (%g seconds)" % args.timeout, flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
