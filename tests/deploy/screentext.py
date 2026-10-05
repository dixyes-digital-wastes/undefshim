#!/usr/bin/env python3
"""Read the text off the machine's screen, with tesseract.

The driver draws black on white in a small bitmap face, and an image of that
reads back once it is scaled up, which is what the ffmpeg call is for. This is
for the cases where the serial port cannot be used at all -- a configuration
that is missing or that will not parse leaves the driver with no port, and the
screen is then the only thing that can say what happened.

tesseract is an external tool and may not be installed. This script does not
try to work without it: it says so and exits with a status of 2, and the
callers skip the checks that need it rather than failing them.

    tests/deploy/screentext.py --ppm build/x/screen.ppm
    tests/deploy/screentext.py --qmp-port 4444 | grep 'broken:'
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

SKIP = 2


def dump(qmpPort, path):
    """Ask the monitor for a picture of the screen."""
    # Called through the interpreter rather than run as a program: the
    # script beside this one is not marked executable, and whether it is
    # should not decide whether this works
    subprocess.run(
        [sys.executable, os.path.join(os.path.dirname(__file__), "screendump.py"),
         "--qmp-port", str(qmpPort), "--out", path],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def upscale(ppm, png, scale):
    """A bigger copy, with the pixels kept square.

    The face is eight by sixteen, which is small for a recogniser; nearest
    neighbour keeps the edges the glyph is made of sharp, where a smoothing
    filter would blur the very pixels that make it readable.
    """
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", ppm,
         "-vf", "scale=iw*%d:ih*%d:flags=neighbor" % (scale, scale), png],
        check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ppm")
    ap.add_argument("--qmp-port", type=int)
    ap.add_argument("--scale", type=int, default=4)
    # Sparse text, and four times the size. The face is eight by sixteen,
    # which is small for a recogniser, and the page mode matters more than
    # the scale: as a block of text the lines run together and the headings
    # come out wrong, and read as loose lines they come out right
    ap.add_argument("--psm", default="11")
    args = ap.parse_args()

    if shutil.which("tesseract") is None:
        print("screentext: no tesseract, skipping", file=sys.stderr)
        return SKIP
    if shutil.which("ffmpeg") is None:
        print("screentext: no ffmpeg, skipping", file=sys.stderr)
        return SKIP
    if args.ppm is None and args.qmp_port is None:
        ap.error("either --ppm or --qmp-port is needed")

    with tempfile.TemporaryDirectory() as tmp:
        ppm = args.ppm or os.path.join(tmp, "screen.ppm")
        png = os.path.join(tmp, "screen.png")
        if args.ppm is None:
            dump(args.qmp_port, ppm)
        upscale(ppm, png, args.scale)
        out = subprocess.run(
            ["tesseract", png, "stdout", "--psm", args.psm],
            check=True, capture_output=True, text=True)
        sys.stdout.write(out.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
