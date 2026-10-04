#!/usr/bin/env python3
"""Read the address-change record the payload leaves behind.

The notification fires during SetVirtualAddressMap, where nothing can be
printed: whether the serial port is reachable at that moment is not something
to find out by writing to it. So the payload writes what it found into its own
blob, and this reads it back through the monitor, by physical address.

The record's physical address is the boot address the driver printed, because
at that point the blob is identity mapped.

Usage: vamap_report.py [--serial build/x/serial.log] [--qmp-port 4448]
                       [--wait SECONDS]
"""

import argparse
import json
import re
import socket
import sys
import time

# UsVaMapRecord, in payload/vamap.h, in order.
FIELDS = [
    "magic", "fired", "poolBefore", "poolAfter", "payloadBefore",
    "payloadAfter", "poolStatus", "payloadStatus", "hookFired", "hookMapSize",
    "hookDescs", "hookStatus", "svmOriginal", "rt", "convertPointer",
]

MAGIC = 0x43455250414D4155



def cmd(sock_file, obj):
    sock_file.write((json.dumps(obj) + "\n").encode())
    sock_file.flush()
    while True:
        line = sock_file.readline()
        if not line:
            raise SystemExit("the monitor closed the connection")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def readWords(qmp_file, addr, count):
    values = []
    done = 0
    while done < count:
        want = min(count - done, 256)
        out = cmd(qmp_file, {"execute": "human-monitor-command",
                             "arguments": {"command-line":
                                           "xp /%dgx 0x%x" % (want, addr + done * 8)}})
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


def recordAddress(serialLog):
    text = open(serialLog, "rb").read().decode("latin1")
    m = re.search(r"vamap: record=0x([0-9a-f]+)", text)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default="build/probe1-run/serial.log")
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--wait", type=float, default=0.0,
                    help="poll until the notification has fired, up to this many seconds")
    args = ap.parse_args()

    qmp = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    qmpFile = qmp.makefile("rwb")
    qmpFile.readline()
    cmd(qmpFile, {"execute": "qmp_capabilities"})

    at = recordAddress(args.serial)
    deadline = time.monotonic() + args.wait
    while args.wait > 0 and at is not None:
        words = readWords(qmpFile, at, 2)
        if len(words) == 2 and words[1] != 0:
            break
        if time.monotonic() > deadline:
            break
        at = recordAddress(args.serial) or at
        time.sleep(1.0)

    if at is None:
        print("no 'vamap: record=' line in %s" % args.serial)
        return 1
    words = readWords(qmpFile, at, len(FIELDS))
    if len(words) != len(FIELDS):
        print("record at 0x%x could not be read" % at)
        return 1
    print("record at 0x%x  magic %s" % (at, "ok" if words[0] == MAGIC else "absent"))
    for name, v in zip(FIELDS, words):
        if name == "magic":
            continue
        print("  %-16s = 0x%-18x%s" % (name, v, extra))
    return 0


if __name__ == "__main__":
    sys.exit(main())
