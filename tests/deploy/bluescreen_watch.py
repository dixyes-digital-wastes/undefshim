#!/usr/bin/env python3
"""Wait for the guest to paint a bluescreen, without waiting a fixed time.

A stopped machine and a slow machine look the same over the serial port: both
are silent. The screen is the only thing that tells them apart, and it says so
the moment Windows draws it.

Colour is necessary but not sufficient. The fraction of the frame that is the
bugcheck blue rising past a threshold means Windows got there; it does not say
which bugcheck, and it cannot tell a real bluescreen from a picture of one. So
the answer this prints is "look now", and whatever is actually wrong is read
out of memory afterwards -- see bugcheck_probe.py.

Nothing is polled from a log file: a screendump is taken and looked at, and the
loop ends on the first frame that is blue.

Usage: bluescreen_watch.py [--qmp-port N] [--interval SEC] [--timeout SEC]
                           [--shots DIR] [--keep]
Exits 0 when the screen goes blue, 1 on timeout, 2 when the machine goes away.
"""

import argparse
import json
import os
import re
import socket
import sys
import time

# The bugcheck colour, measured rather than guessed: 96% of the frame in a
# capture of one. The tolerance is for the dithering and the antialiasing of
# the text drawn on top of it.
BLUE = (0, 61, 146)
TOLERANCE = 32

# A frame that is mostly this colour is a bluescreen. Well below the 96% a
# real one shows and well above anything a normal screen reaches.
THRESHOLD = 0.5


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


def readPpm(path):
    """The pixels of a binary PPM, as a bytes object, with its size.

    QEMU writes P6 with a comment-free header, but the header is still parsed
    rather than assumed: a PPM whose width changed between two captures would
    otherwise be read as garbage rather than as the wrong size.
    """
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None, 0, 0, None

    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i : i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1

    width, height, _ = fields
    if len(data) - i < width * height * 3:
        return None, width, height, None
    return data, width, height, i


def blueFraction(path, stride=7):
    """How much of the frame is the bugcheck colour, and how much is black.

    Sampled rather than counted: the answer only has to be on the right side
    of a threshold, and this runs between two screendumps.
    """
    data, width, height, off = readPpm(path)
    if data is None:
        return None, None

    total = 0
    blue = 0
    black = 0
    for p in range(off, off + width * height * 3 - 2, 3 * stride):
        r, g, b = data[p], data[p + 1], data[p + 2]
        total += 1
        if (abs(r - BLUE[0]) <= TOLERANCE and abs(g - BLUE[1]) <= TOLERANCE
                and abs(b - BLUE[2]) <= TOLERANCE):
            blue += 1
        elif r < 16 and g < 16 and b < 16:
            black += 1
    if total == 0:
        return None, None
    return blue / total, black / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--report", type=int, default=5,
                    help="report every this many frames")
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--shots", help="directory to keep the frames in")
    ap.add_argument("--keep", action="store_true",
                    help="keep every frame, not only the one that matched")
    args = ap.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = sock.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    if args.shots:
        os.makedirs(args.shots, exist_ok=True)
    shot = os.path.join(args.shots or "/tmp", "bluescreen-watch.ppm")
    shot = os.path.abspath(shot)

    deadline = time.monotonic() + args.timeout
    started = time.monotonic()
    n = 0
    while time.monotonic() < deadline:
        n += 1
        elapsed = time.monotonic() - started
        try:
            cmd(f, {"execute": "screendump", "arguments": {"filename": shot}})
        except (SystemExit, OSError):
            print("the machine went away", file=sys.stderr)
            return 2
        time.sleep(0.2)

        frac, black = blueFraction(shot)
        if frac is None:
            print("frame %d: unreadable" % n, file=sys.stderr)
        elif frac >= THRESHOLD:
            print("frame %d (%.0fs): rgb%s covers %.1f%% -- bluescreen"
                  % (n, elapsed, BLUE, 100 * frac))
            if args.shots:
                keep = os.path.join(args.shots, "bluescreen.ppm")
                os.replace(shot, keep)
                print("frame kept at %s" % keep)
            return 0
        elif args.shots and args.keep:
            os.replace(shot, os.path.join(args.shots, "frame-%04d.ppm" % n))
        elif frac is not None and (n <= 1 or n % args.report == 0):
            # Often enough to tell a slow boot from a hung one. The first
            # frame is always reported: a watch that says nothing at all
            # looks like it never started.
            print("frame %d (%.0fs): blue %.1f%%, black %.1f%%"
                  % (n, elapsed, 100 * frac, 100 * black))
        time.sleep(args.interval)

    print("no bluescreen within %g seconds" % args.timeout, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
