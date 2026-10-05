#!/usr/bin/env python3
"""Run the machine, then follow one register through the recorded frames.

The stop is a wild pointer used somewhere, and the recorded frames say what
the registers held at each of the last exceptions. A pointer that is already
wild in an early frame went wrong before that exception; one that is sensible
in every frame went wrong between the last of them and the stop, which is code
this project never sees and corruption from somewhere else.

That difference decides where to look next, and it is not visible from the
summary, which only says where the machine ended up.

Usage: reg_history.py [--reg x26] [--qemu ...]
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time
import logtext

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PUBLIC_WORDS = 15
TRACE_SLOTS = 8
TRACE_WORDS = 38
EMU_SLOTS = 8
EMU_WORDS = 5

FRAME = ["x%d" % i for i in range(31)] + ["sp", "elr", "spsr", "esr", "far"]
WIDTH = {"x%d" % i: i for i in range(31)}


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


def readAt(f, addr, count, physical):
    """`xp` for the pool, `x` for anything the kernel reaches by its own
    address. The pool is physical on this target and the kernel's data is a
    KASLR'd virtual address, so the wrong one answers "cannot access" and
    reads as an empty block rather than as an error."""
    out = cmd(f, {"execute": "human-monitor-command",
                  "arguments": {"command-line":
                                "%s /%dgx 0x%x" % ("xp" if physical else "x",
                                                     min(count, 128), addr)}}).get("return", "")
    if "Cannot" in out:
        return []
    values = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        values += [int(v, 16) for v in line.split(":", 1)[1].split()]
    return values


def readAll(f, addr, count, physical):
    out = []
    done = 0
    while done < count:
        block = readAt(f, addr + done * 8, min(count - done, 128), physical)
        if not block:
            break
        out += block
        done += len(block)
    return out


def kernelBase(f, pc):
    """The vector table's shape, as bugcheck_probe finds it."""
    def word(a):
        out = cmd(f, {"execute": "human-monitor-command",
                      "arguments": {"command-line": "x /1gx 0x%x" % a}}).get("return", "")
        m = re.search(r":\s*0x([0-9a-f]+)", out)
        return (int(m.group(1), 16) & 0xFFFFFFFF) if m else None

    PROLOGUE = 0xD5384112
    top = pc - (pc % 0x200000)
    for i in range(16):
        base = top - i * 0x200000
        table = base + 0x604800
        heads = [word(table + s * 0x80) for s in range(4)]
        if any(h is None for h in heads):
            continue
        if heads[1] == PROLOGUE and heads[2] == PROLOGUE and heads[3] == PROLOGUE:
            return base
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reg", default="x26")
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--work", default="build/hist")
    ap.add_argument("--find-timeout", type=float, default=90)
    ap.add_argument("--catch-timeout", type=float, default=120)
    args = ap.parse_args()

    os.chdir(ROOT)
    probe = ["python3", "-u", "tests/deploy/bugcheck_probe.py",
             "--arm", "true", "--rewrite", "false",
             "--find-timeout", str(args.find_timeout),
             "--catch-timeout", str(args.catch_timeout),
             "--keep", "--work", args.work]
    subprocess.run(probe)

    f = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20).makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    out = cmd(f, {"execute": "human-monitor-command",
                  "arguments": {"command-line": "info registers -a"}}).get("return", "")
    m = re.search(r"CPU#0\s*\n\s*PC=([0-9a-f]+)", out)
    pc = int(m.group(1), 16)
    base = kernelBase(f, pc)
    print("pc=0x%x base=0x%x" % (pc, base or 0))

    if base:
        rec = readAll(f, base + 0xdba5e0, 6, physical=False)
        if len(rec) >= 5:
            print("bugcheck 0x%x  p1=0x%x p2=0x%x p3=0x%x p4=0x%x"
                  % (rec[0], rec[1], rec[2], rec[3], rec[4]))
            for n, v in zip(("p1", "p2", "p3", "p4"), rec[1:5]):
                if base <= v < base + 0x1249000:
                    print("  %s -> kbase+0x%x" % (n, v - base))
        else:
            print("the bugcheck record could not be read")

    text = logtext.read(os.path.join(args.work, "serial.log"))
    m = re.search(r"pool: pa=0x([0-9a-f]+)", text)
    if not m:
        print("no pool address in the log")
        return 1
    pool = int(m.group(1), 16)

    total = PUBLIC_WORDS + TRACE_SLOTS * TRACE_WORDS + 1 + EMU_SLOTS * EMU_WORDS
    words = readAll(f, pool + 8, total, physical=True)
    entries = words[1]
    print("entries=0x%x handled=0x%x" % (words[1], words[2]))
    at = PUBLIC_WORDS
    trace = words[at:at + TRACE_SLOTS * TRACE_WORDS]

    print("")
    print("the last %d exceptions, oldest first" % TRACE_SLOTS)
    shown = min(entries, TRACE_SLOTS)
    for back in range(shown - 1, -1, -1):
        index = entries - 1 - back
        slot = index % TRACE_SLOTS
        t = trace[slot * TRACE_WORDS:(slot + 1) * TRACE_WORDS]
        frame = t[2:2 + 36]
        spsr = frame[33]
        where = "EL1t" if (spsr & 0xf) == 4 else ("EL1h" if (spsr & 0xf) == 5 else "?")
        reg = frame[WIDTH[args.reg]]
        elr = frame[32]
        rva = ("kbase+0x%x" % (elr - base)) if base and base <= elr < base + 0x1249000 else "0x%x" % elr
        print("  entry %-6d %-4s elr=%-16s %s=0x%-17x %s"
              % (index, where, rva, args.reg, reg,
                 "(pool)" if pool <= reg < pool + 0x21000 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
