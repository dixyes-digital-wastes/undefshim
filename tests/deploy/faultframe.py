#!/usr/bin/env python3
"""Read the frame the payload was working on when it faulted.

The entry leaves the frame at the top of the CPU's private stack and never
gets to remove it, so the frame of the exception that faulted is the one at
the top. The CPU that faulted is the one spinning in the kernel's halt loop
after the bugcheck; the others are parked in the stall check.

Usage: faultframe.py [--serial build/x/serial.log] [--probe-log build/x.log]
                     [--qmp-port 4448]
"""

import argparse
import json
import re
import socket
import sys

POOL_HEADER = 0x2000
STACK_SIZE = 0x4000
FRAME_STRIDE = 288

FRAME = ["x%d" % i for i in range(31)] + ["sp", "elr", "spsr", "esr", "far"]


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


def readWords(f, addr, count, physical=False):
    unit = "xp" if physical else "x"
    values = []
    done = 0
    while done < count:
        want = min(count - done, 128)
        out = cmd(f, {"execute": "human-monitor-command",
                      "arguments": {"command-line":
                                    "%s /%dgx 0x%x" % (unit, want, addr + done * 8)}})
        got = 0
        for line in out.get("return", "").splitlines():
            if ":" not in line:
                continue
            row = [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
            values += row
            got += len(row)
        if got == 0:
            break
        done += got
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True)
    ap.add_argument("--probe-log", required=True)
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--vamap-offset", type=int, default=11096)
    args = ap.parse_args()

    qmp = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = qmp.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    text = open(args.serial, "rb").read().decode("latin1")
    blob = int(re.search(r"payload: at 0x([0-9a-f]+)", text).group(1), 16)
    record = readWords(f, blob + args.vamap_offset, 34, physical=True)
    pool_after = record[3]
    payload_after = record[5]
    print("payloadAfter = 0x%x" % payload_after)
    print("poolAfter    = 0x%x" % pool_after)

    log = open(args.probe_log, "rb").read().decode("latin1")
    base = int(re.search(r"^kernel base = 0x([0-9a-f]+)", log, re.M).group(1), 16)
    halt = None
    for m in re.finditer(r"cpu(\d) pc = 0x([0-9a-f]+)", log):
        if int(m.group(2), 16) - base == 0x452f20:
            halt = int(m.group(1))
    print("halt cpu     = %s" % halt)
    if halt is None:
        return 1

    top = pool_after + POOL_HEADER + (halt + 1) * STACK_SIZE
    frame = top - 16 - FRAME_STRIDE
    print("stack top    = 0x%x" % top)
    print("frame at     = 0x%x" % frame)
    words = readWords(f, frame, 36)
    if len(words) != 36:
        print("the frame could not be read")
        return 1
    for name, value in zip(FRAME, words):
        print("  %-5s = 0x%x" % (name, value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
