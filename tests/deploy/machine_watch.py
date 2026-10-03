#!/usr/bin/env python3
"""Poll a running machine and say which of three things it is doing.

A stopped machine and a slow one are both silent on the serial port, and the
screen alone does not tell them apart either: a machine that has stopped goes
on showing whatever it drew last, which looks the same as a machine that has
not drawn anything new yet. Neither signal is enough by itself.

Two together are:

  the program counter   advances while the machine is doing anything at all,
                        and stops when it is not. That is what separates a
                        machine that is working from one that has halted.
  the screen colour     goes to the bugcheck blue when Windows gives up, which
                        is a crash -- a *different* thing from a halt, and one
                        that has a record attached.

So the answer is one of:

  crashed   the bugcheck colour is on screen. There is a bugcheck record to
            read, and the PC does not say anything useful.
  halted    every processor's PC has been the same for several samples and no
            bugcheck was drawn. The machine is not going anywhere, and where
            the PC is stopped is the whole of the evidence.
  working   the PCs are moving. Whatever is wrong has not happened yet.

The halted case is why this exists: an earlier version watched for the colour
alone, and a run that produced no colour for two and a half minutes could not
be told from one that was simply still booting.

Exit codes: 0 crashed, 3 halted, 1 timeout, 2 the machine went away.
"""

import argparse
import json
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bluescreen_watch import blueFraction, readPpm, THRESHOLD, BLUE  # noqa: E402


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


def hmp(sock_file, command):
    return cmd(sock_file, {"execute": "human-monitor-command",
                           "arguments": {"command-line": command}}).get("return", "")


def processors(sock_file):
    """Every processor's program counter.

    All of them, not the first: a machine whose first processor is waiting on
    something another one is computing is not halted, and looking at one
    processor would call it that.
    """
    out = hmp(sock_file, "info registers -a")
    return tuple(int(m.group(2), 16)
                 for m in re.finditer(r"CPU#(\d+)\s*\n\s*PC=([0-9a-f]+)", out))


def connect(port, timeout):
    """The monitor, or None when there is nothing to talk to.

    A machine that has reset is a machine whose monitor socket is gone, and
    that is an answer rather than a failure: it is what a triple fault looks
    like from outside. Raising here instead reported it as the tool breaking.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--frozen", type=int, default=8,
                    help="consecutive unchanged samples that mean halted")
    ap.add_argument("--shots", help="directory to keep a frame in")
    ap.add_argument("--report", type=int, default=5,
                    help="report every this many samples")
    args = ap.parse_args()

    sock, f = connect(args.qmp_port, 20)
    if sock is None:
        print("the machine is not there to watch", flush=True)
        return 2

    if args.shots:
        os.makedirs(args.shots, exist_ok=True)
    shot = os.path.abspath(os.path.join(args.shots or "/tmp", "machine-watch.ppm"))

    deadline = time.monotonic() + args.timeout
    started = time.monotonic()
    n = 0
    last = None
    unchanged = 0

    while time.monotonic() < deadline:
        n += 1
        elapsed = time.monotonic() - started

        try:
            pc = processors(f)
            cmd(f, {"execute": "screendump", "arguments": {"filename": shot}})
        except (SystemExit, OSError):
            print("the machine went away after %.0fs" % elapsed, flush=True)
            return 2
        time.sleep(0.2)

        frac, black = blueFraction(shot)
        if frac is None:
            frac, black = 0.0, 0.0

        if frac >= THRESHOLD:
            print("%4.0fs: frame %d, rgb%s covers %.1f%% -- crashed"
                  % (elapsed, n, BLUE, 100 * frac), flush=True)
            if args.shots:
                os.replace(shot, os.path.join(args.shots, "bluescreen.ppm"))
            # The PCs are printed as well: a bugcheck record says what the
            # kernel was told, and where it was stopped says where from.
            print("  pcs: %s" % " ".join("0x%x" % p for p in pc), flush=True)
            return 0

        if last is not None and pc == last:
            unchanged += 1
        else:
            unchanged = 0
        last = pc

        if unchanged >= args.frozen:
            print("%4.0fs: frame %d, no bugcheck, and every processor has been "
                  "at the same place for %d samples -- halted"
                  % (elapsed, n, unchanged), flush=True)
            for i, p in enumerate(pc):
                print("  cpu%d pc = 0x%x" % (i, p), flush=True)
            if args.shots:
                os.replace(shot, os.path.join(args.shots, "halted.ppm"))
            return 3

        if n <= 1 or n % args.report == 0:
            print("%4.0fs: frame %d, blue %.1f%%, pc %s%s"
                  % (elapsed, n, 100 * frac,
                     " ".join("0x%x" % p for p in pc[:2]),
                     (" (+%d more)" % (len(pc) - 2)) if len(pc) > 2 else ""),
                  flush=True)
        time.sleep(args.interval)

    print("no verdict within %g seconds" % args.timeout, flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
