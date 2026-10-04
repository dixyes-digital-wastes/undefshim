#!/usr/bin/env python3
"""Find which site in a list is the one that stops the machine booting.

The failure this looks for is a bad site: one whose bytes looked like a load
but are something else, so writing the replacement breaks what reads them.
That shows up long before a desktop - the loader never hands over - so the
test for a candidate set is cheap: start the machine, wait, and ask whether
the kernel ever ran. It did if the payload's entry counter moved past the one
carried over from before the list was applied; it did not if the processors
are still sitting in the loader.

Each step boots once and stops the machine with a QMP quit. Halving finds one
bad site in about log2(n) boots; with several bad ones it finds one of them,
and running it again finds the next.
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bugcheck_probe  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DRIVER = "build/rwClean/undefshim_driver.efi"


def parse(path):
    head, sites = [], []
    for line in open(path, "r", encoding="ascii", errors="replace"):
        (sites if line.lstrip().startswith("0x") else head).append(line.rstrip("\n"))
    return head, sites


def writeList(path, head, sites, digest):
    out = [line for line in head if not line.startswith("textSHA256Hash")]
    stamped = False
    result = []
    for line in out:
        result.append(line)
        if line.startswith("USPATCHV1") and digest:
            result.append("textSHA256Hash " + digest)
            stamped = True
    if not stamped and digest:
        result.insert(1, "textSHA256Hash " + digest)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="ascii").write("\n".join(result + sites) + "\n")
    return path


def boots(work, lists, port, vnc, gdb, wait):
    """True when the kernel ran with this list, false when it did not."""
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    env = dict(os.environ)
    env["DRIVER"] = os.path.join(ROOT, DRIVER)
    env["PATCHLIST_DIR"] = lists
    env["QEMU_VNC"] = "0.0.0.0:%d" % vnc
    subprocess.run([sys.executable, "-u", os.path.join(ROOT, "tests/deploy/bugcheck_probe.py"),
                    "--arm", "true", "--rewrite", "false", "--vamap", "true", "--keep",
                    "--find-timeout", "240", "--catch-timeout", "2",
                    "--qmp-port", str(port), "--gdb-port", str(gdb),
                    "--work", os.path.join(work, "run")],
                   cwd=ROOT, env=env, capture_output=True)
    serial = os.path.join(work, "run", "serial.log")
    text = open(serial, "rb").read().decode("latin1") if os.path.exists(serial) else ""
    applied = re.search(r"applied (\d+) refused (\d+)", text)
    pool = re.search(r"pool: pa=0x([0-9a-f]+)", text)
    result = {"applied": int(applied.group(1)) if applied else 0,
              "refused": int(applied.group(2)) if applied else 0, "ran": False, "entries": 0}
    if pool is None:
        result["error"] = "no pool"
        return result
    f = None
    for _ in range(40):
        try:
            f = socket.create_connection(("127.0.0.1", port), timeout=20).makefile("rwb")
            f.readline()
            bugcheck_probe.cmd(f, {"execute": "qmp_capabilities"})
            break
        except Exception:
            f = None
            time.sleep(3)
    if f is None:
        result["error"] = "no monitor"
        return result
    deadline = time.time() + wait
    while time.time() < deadline:
        result["entries"] = bugcheck_probe.readWords(f, int(pool.group(1), 16) + 8 + 8, 1,
                                                     physical=True)[0]
        if result["entries"] > 2:
            result["ran"] = True
            break
        time.sleep(2)
    try:
        bugcheck_probe.cmd(f, {"execute": "quit"})
    except Exception:
        pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", default="config/usPatch/all-ldapr.txt")
    parser.add_argument("--work", default="build/bisect")
    parser.add_argument("--digest-from", default="",
                        help="a run log whose driver-printed digest to stamp the candidate with")
    parser.add_argument("--wait", type=float, default=90.0,
                        help="seconds to give the kernel to run before calling it a hang")
    parser.add_argument("--qmp-port", type=int, default=4493)
    parser.add_argument("--gdb-port", type=int, default=1273)
    parser.add_argument("--vnc", type=int, default=0)
    args = parser.parse_args()

    head, sites = parse(args.list)
    digest = ""
    if args.digest_from:
        found = re.search(r"patch: text sha256 ([0-9a-f]{64})",
                          open(args.digest_from, "rb").read().decode("latin1", "replace"))
        digest = found.group(1) if found else ""
    print("list has %d sites, digest %s" % (len(sites), digest or "as written"))

    lists = os.path.join(args.work, "lists")
    path = os.path.join(lists, "candidate.txt")
    step = 0
    while len(sites) > 1:
        half = len(sites) // 2
        for label, candidate in (("first", sites[:half]), ("second", sites[half:])):
            step += 1
            writeList(path, head, candidate, digest)
            result = boots(os.path.join(args.work, "step%d" % step), lists,
                           args.qmp_port, args.vnc, args.gdb_port, args.wait)
            print("step %d: %s half, %d sites, applied %d refused %d, entries %d -> %s"
                  % (step, label, len(candidate), result["applied"], result["refused"],
                     result["entries"], "ran" if result["ran"] else "hung"))
            sys.stdout.flush()
            if result["ran"]:
                sites = candidate
                break
        else:
            print("neither half hangs: the problem needs a different split")
            return 1
    print("the site that stops it: %s" % sites[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
