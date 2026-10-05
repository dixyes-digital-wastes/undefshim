#!/usr/bin/env python3
"""Read the payload's record out of a stopped machine and lay it out.

The payload cannot write to the console once the kernel has its own page
tables, so the pool is the only channel it has. The monitor can read the pool
afterwards, and this prints everything in it in the shapes the C structures
have: the summary, the rolling ring of whole frames, and the rolling ring of
carried-out loads.

The frames are the part that answers "what did the interrupted code have in
this register", which is what a stop needs and what nothing else can supply.
The loads are the part that answers "did the value we put there come from
somewhere sensible".

Usage: pool_dump.py [--qmp-port N] [--pool ADDR] [--serial FILE]
The pool address is taken from the serial log when it is not given.
"""

import argparse
import json
import re
import socket
import sys
import logtext

MAGIC = 0x005952544E455355  # "USENTRY", the pool entry record

# The C structures, counted out as they are laid out. They are written here
# rather than derived, because the point of printing them is to check what the
# C side actually produced.
PUBLIC = ["magic", "entries", "handled", "handedBack", "handbackEsr", "handbackElr",
          "stuck", "stuckKind", "stuckEsr", "stuckElr", "stuckSpsr", "stuckVbar",
          "nestedFaults", "nestedEsr", "nestedFar", "nestedElr",
          "nestedSelfVa", "nestedPoolBase", "nestedStackTop", "nestedCpu",
          "lastEsr", "lastElr", "lastFar",
          "lastCpu", "lastSp", "lastInsn", "emuInsn", "emuAddr", "emuValue",
          "emuElr", "emuX0", "emuX9"]
PUBLIC_WORDS = len(PUBLIC)
TRACE_SLOTS = 8
TRACE_WORDS = 38          # cpu, mpidr, then the 36 words of the frame
EMU_SLOTS = 8
EMU_WORDS = 5             # elr, insn, rt, address, value
REWRITE_SLOTS = 8
REWRITE_WORDS = 5         # site, insn, descriptorVa, descriptor, result

# What a rewrite attempt came to, from UsRewriteResult in payload/rewrite.h.
REWRITE_RESULTS = {
    1: "written",
    8: "not an RCpc load",
    9: "misaligned for the substitute",
    10: "in progress: reading the base",
    11: "in progress: walking the tables",
    12: "in progress: probing the descriptor",
    13: "in progress: clearing the permission",
    14: "in progress: storing",
    15: "in progress: publishing the instruction",
    16: "in progress: putting the permission back",
    2: "already replaced",
    3: "refused",
    4: "no descriptor base",
    5: "unmapped",
    6: "descriptor page read-only",
    7: "permission stuck",
}

FRAME = ["x%d" % i for i in range(31)] + ["sp", "elr", "spsr", "esr", "far"]


def entryOffsets():
    """Where the fields after the summary live, in bytes from the entry.

    Counted the same way the dump counts them, so the two cannot drift: a
    caller that wants one field does not have to read the whole ring.
    """
    at = PUBLIC_WORDS * 8
    at += TRACE_SLOTS * TRACE_WORDS * 8      # trace
    at += 8                                  # emuCount
    at += EMU_SLOTS * EMU_WORDS * 8          # emu
    at += TRACE_SLOTS * TRACE_WORDS * 8      # handback
    return {"rewriteCount": at, "rewriteWritten": at + 8, "rewrite": at + 16}


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


def readPhysical(f, addr, count):
    out = cmd(f, {"execute": "human-monitor-command",
                  "arguments": {"command-line":
                                "xp /%dgx 0x%x" % (min(count, 128), addr)}}).get("return", "")
    values = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        values += [int(v, 16) for v in line.split(":", 1)[1].split()]
    return values


def readAll(f, addr, count):
    """The monitor caps one command, so ask in blocks."""
    out = []
    done = 0
    while done < count:
        want = min(count - done, 128)
        block = readPhysical(f, addr + done * 8, want)
        if not block:
            break
        out += block
        done += len(block)
    return out


def poolFromSerial(path):
    text = logtext.read(path)
    m = re.search(r"pool: pa=0x([0-9a-f]+)", text)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--pool", type=lambda s: int(s, 16), default=None)
    ap.add_argument("--serial", default="/tmp/alive.serial")
    args = ap.parse_args()

    pool = args.pool
    if pool is None:
        pool = poolFromSerial(args.serial)
    if pool is None:
        print("no pool address: not in the serial log, and none given")
        return 1

    sock = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = sock.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    total = (PUBLIC_WORDS + TRACE_SLOTS * TRACE_WORDS + 1 + EMU_SLOTS * EMU_WORDS
             + TRACE_SLOTS * TRACE_WORDS + 2 + REWRITE_SLOTS * REWRITE_WORDS)
    words = readAll(f, pool + 8, total)
    if len(words) < total:
        print("only %d of %d words could be read at 0x%x"
              % (len(words), total, pool + 8))
        return 1

    at = 0
    print("record at 0x%x" % (pool + 8))
    head = words[at:at + PUBLIC_WORDS]
    at += PUBLIC_WORDS
    print("  magic %s" % ("ok" if head[0] == MAGIC else "ABSENT (0x%x)" % head[0]))
    for name, v in zip(PUBLIC[1:], head[1:]):
        print("    %-9s = 0x%x" % (name, v))

    entries = head[1]
    trace = words[at:at + TRACE_SLOTS * TRACE_WORDS]
    at += TRACE_SLOTS * TRACE_WORDS
    emuCount = words[at]
    at += 1
    emu = words[at:at + EMU_SLOTS * EMU_WORDS]
    at += EMU_SLOTS * EMU_WORDS
    handback = words[at:at + TRACE_SLOTS * TRACE_WORDS]
    at += TRACE_SLOTS * TRACE_WORDS
    rewriteCount, rewriteWritten = words[at], words[at + 1]
    at += 2
    rewrites = words[at:at + REWRITE_SLOTS * REWRITE_WORDS]
    at += REWRITE_SLOTS * REWRITE_WORDS

    # The rewrites are the difference between a boot that finishes and one that
    # only looks busy, so they are printed even when there are none.
    print("replacing instructions where they stand: %d attempted, %d written"
          % (rewriteCount, rewriteWritten))
    # Every slot that has something in it, oldest first: an attempt that never
    # finished wrote its slot as it went, and which step it reached is the only
    # thing a machine that stopped has to say.
    live = [i for i in range(REWRITE_SLOTS)
            if any(rewrites[i * REWRITE_WORDS:(i + 1) * REWRITE_WORDS])]
    for base in [i * REWRITE_WORDS for i in live]:
        site, insn, descriptorVa, descriptor, result = rewrites[base:base + REWRITE_WORDS]
        print("  site 0x%-16x insn 0x%-10x desc 0x%x%s  %s"
              % (site, insn, descriptorVa,
                 (" = 0x%x" % descriptor) if descriptor else "",
                 REWRITE_RESULTS.get(result, "result %d" % result)))

    print("")
    print("the last up to %d exceptions, oldest first" % TRACE_SLOTS)
    if entries == 0:
        print("  none: the handler was never entered")
    else:
        shown = min(entries, TRACE_SLOTS)
        for back in range(shown - 1, -1, -1):
            index = entries - 1 - back
            slot = index % TRACE_SLOTS
            t = trace[slot * TRACE_WORDS:(slot + 1) * TRACE_WORDS]
            h = handback[slot * TRACE_WORDS:(slot + 1) * TRACE_WORDS]
            print("  entry %d (slot %d): cpu=%d mpidr=0x%x"
                  % (index, slot, t[0], t[1]))
            frame = t[2:2 + 36]
            after = h[2:2 + 36]
            for name, v, w in zip(FRAME, frame, after):
                if v != w:
                    print("      %-5s = 0x%-16x -> 0x%x   (changed)" % (name, v, w))
                elif v:
                    print("      %-5s = 0x%x" % (name, v))
            changed = [(n, a, b) for n, a, b in zip(FRAME, frame, after) if a != b]
            if changed:
                print("      *** %d registers changed between receiving and "
                      "handing back: %s"
                      % (len(changed), ", ".join(n for n, _, _ in changed)))
            else:
                print("      nothing changed between receiving and handing back")

    print("")
    print("%d loads carried out, the last up to %d:"
          % (emuCount, EMU_SLOTS))
    if emuCount == 0:
        print("  none")
    else:
        shown = min(emuCount, EMU_SLOTS)
        for back in range(shown - 1, -1, -1):
            index = emuCount - 1 - back
            e = emu[(index % EMU_SLOTS) * EMU_WORDS:(index % EMU_SLOTS + 1) * EMU_WORDS]
            print("  #%d elr=0x%x insn=0x%08x rt=%d addr=0x%x value=0x%x"
                  % (index, e[0], e[1], e[2], e[3], e[4]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
