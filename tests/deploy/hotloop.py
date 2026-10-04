#!/usr/bin/env python3
"""Collect the sites that are trapping, patch them, and measure again.

One round is one boot: the list from the previous round is applied before the
kernel runs, the trap rate is measured from the payload's own counters, and
the addresses still trapping are read out of its record of the last
exception -- those become the next round's list. Each round therefore answers
both questions at once, and the rate is the thing to watch.

The machine is stopped with a QMP quit rather than a signal, so nothing here
needs to know what else is running. The lists are ordinary usPatch files, so
a round can also be seeded with one written by hand.
"""

import argparse
import collections
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bugcheck_probe  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DRIVER = "build/rwClean/undefshim_driver.efi"
SAMPLES = 24
INTERVAL = 0.5


def connect(port, tries=80):
    for _ in range(tries):
        try:
            f = socket.create_connection(("127.0.0.1", port), timeout=30).makefile("rwb")
            f.readline()
            bugcheck_probe.cmd(f, {"execute": "qmp_capabilities"})
            return f
        except Exception:
            time.sleep(3)
    return None


def readList(path):
    """The sites in a list file, as (rva, instruction) pairs."""
    sites = {}
    if not os.path.exists(path):
        return sites
    for line in open(path, "r", encoding="ascii", errors="replace"):
        line = line.split("#", 1)[0].strip()
        if not line or not line.startswith("0x"):
            continue
        field = line.split()
        if len(field) >= 3:
            sites[int(field[0], 16)] = int(field[1], 16)
    return sites


def writeList(path, digest, sites):
    lines = ["# Written by tests/deploy/hotloop.py: the sites the machine was",
             "# measured trapping on, one per line. LDAPR to LDAR keeps the base",
             "# and target register and drops the acquire-RCpc ordering field.",
             "USPATCHV1",
             "peFile ntoskrnl",
             "textSHA256Hash " + digest,
             ""]
    for rva in sorted(sites):
        insn = sites[rva]
        ld = (insn & 0xC0000000) | 0x08DFFC00 | (insn & 0x1FE0) | (insn & 0x1F)
        lines.append("0x%x %08x %08x" % (rva, insn, ld))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="ascii") as handle:
        handle.write("\n".join(lines) + "\n")


def round_(args, roundIndex, sites):
    work = os.path.join(args.work, "round%d" % roundIndex)
    lists = os.path.join(args.work, "lists")
    listPath = os.path.join(lists, "hot.txt")
    os.makedirs(work, exist_ok=True)
    if roundIndex > 0:
        writeList(listPath, args.digest, sites)

    env = dict(os.environ)
    env["DRIVER"] = os.path.join(ROOT, DRIVER)
    env["PATCHLIST_DIR"] = lists if roundIndex > 0 else ""
    env["QEMU_VNC"] = "0.0.0.0:%d" % args.vnc
    probe = subprocess.run(
        [sys.executable, "-u", os.path.join(ROOT, "tests/deploy/bugcheck_probe.py"),
         "--arm", "true", "--rewrite", "false", "--vamap", "true", "--keep",
         "--find-timeout", "240", "--catch-timeout", "2",
         "--qmp-port", str(args.qmp_port), "--gdb-port", str(args.gdb_port),
         "--work", os.path.join(work, "run")],
        cwd=ROOT, env=env, capture_output=True)
    serial = os.path.join(work, "run", "serial.log")
    text = open(serial, "rb").read().decode("latin1") if os.path.exists(serial) else ""
    applied = re.search(r"patch: (\S+) applied (\d+) refused (\d+) out of range (\d+)", text)
    digest = re.search(r"patch: text sha256 ([0-9a-f]{64})", text)
    if digest:
        args.digest = digest.group(1)
    pool = re.search(r"pool: pa=0x([0-9a-f]+)", text)
    report = {"round": roundIndex, "applied": 0, "refused": 0, "rate": 0.0, "hot": {}}
    if applied:
        report["applied"] = int(applied.group(2))
        report["refused"] = int(applied.group(3))
    if pool is None or args.digest is None:
        report["error"] = "no pool or digest"
        subprocess.run(["pkill", "-f", "qmp-port %d" % args.qmp_port], check=False)
        return report, sites

    f = connect(args.qmp_port)
    if f is None:
        report["error"] = "no monitor"
        return report, sites
    kbase = bugcheck_probe.findKernelBase(f, bugcheck_probe.programCounter(f))
    seen = collections.Counter()
    start = None
    for _ in range(SAMPLES):
        words = bugcheck_probe.readWords(f, int(pool.group(1), 16) + 8, 26, physical=True)
        if start is None:
            start = words[1]
        elr = words[21]
        if elr > kbase:
            seen[elr - kbase] += 1
        time.sleep(INTERVAL)
    end = bugcheck_probe.readWords(f, int(pool.group(1), 16) + 8, 26, physical=True)[1]
    report["rate"] = (end - start) / (SAMPLES * INTERVAL)
    for rva, count in seen.items():
        insn = bugcheck_probe.readWords(f, kbase + rva, 1)[0] & 0xFFFFFFFF
        report["hot"][rva] = (insn, count)
        # The same encoding test the parser and the enumerator use: mask the
        # fields away and see what is left of the opcode.
        if (insn & 0x3FFFFC00) == 0x38BFC000:
            sites[rva] = insn
    try:
        bugcheck_probe.cmd(f, {"execute": "quit"})
    except Exception:
        pass
    return report, sites


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--work", default="build/hotloop")
    parser.add_argument("--qmp-port", type=int, default=4480)
    parser.add_argument("--gdb-port", type=int, default=1265)
    parser.add_argument("--vnc", type=int, default=9)
    args = parser.parse_args()
    args.digest = None

    sites = {}
    for index in range(args.rounds):
        report, sites = round_(args, index, sites)
        if "error" in report:
            print("round %d: %s" % (index, report["error"]))
            continue
        print("round %d: list %d applied %d refused %d | entries %.0f/s | hot now: %s"
              % (index, len(sites), report["applied"], report["refused"], report["rate"],
                 ", ".join("%#x x%d" % (rva, count)
                           for rva, (insn, count) in sorted(report["hot"].items(),
                                                            key=lambda kv: -kv[1][1])[:4])))
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
