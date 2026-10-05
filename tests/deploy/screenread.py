#!/usr/bin/env python3
"""Read the machine's screen.

Two things are worth reading off it, and they are read differently:

  text       the driver draws one glyph per cell in a small bitmap face, which
             a recognition engine reads back once the picture is scaled up.
             This is the only channel a machine that never got a serial port
             has, and the reason the case that checks a refused configuration
             can be checked at all.
  bugcheck   Windows paints the whole frame a known blue when it gives up,
             which is a colour rather than a word and is measured directly.

The text readers are pluggable. tesseract is the one that is implemented, and
it is an external program that may not be installed: a caller asks whether the
engine is there and skips its check if it is not, rather than failing it.
Adding another -- paddleocr, say -- is a class with three members, and the
name it registers under.

Usage:
    screenread.py --ppm build/x/screen.ppm
    screenread.py --qmp-port 4448
    screenread.py --qmp-port 4448 --poll 'broken'
    screenread.py --qmp-port 4448 --want bugcheck

Exits 0 when what was asked for was seen, 1 when it was not, 2 when the engine
is not installed.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

SKIP = 2

# The bugcheck colour, measured rather than guessed: 96% of the frame in a
# capture of one. The tolerance is for the dithering and the antialiasing of
# the text drawn on top of it, and the threshold is well below what a real one
# shows and well above anything a normal screen reaches.
BLUE = (0, 61, 146)
TOLERANCE = 32
THRESHOLD = 0.5


# ---------------------------------------------------------------- the picture

def grab(qmpPort, path):
    """Ask the monitor for a picture of the screen."""
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


def readPpm(path):
    """The pixels of a binary PPM, and the offset they start at."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None, 0, 0, 0
    fields = []
    i = 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i + 1].isspace():
            i += 1
        fields.append(int(data[start:i]))
    i += 1
    width, height, _ = fields
    if len(data) - i < width * height * 3:
        return None, width, height, 0
    return data, width, height, i


def blueFraction(path, stride=7):
    """How much of the frame is the bugcheck colour.

    Sampled rather than counted: the answer only has to be on the right side
    of a threshold, and this runs between two screendumps.
    """
    data, width, height, off = readPpm(path)
    if data is None:
        return None
    total = 0
    blue = 0
    for p in range(off, off + width * height * 3 - 2, 3 * stride):
        r, g, b = data[p], data[p + 1], data[p + 2]
        total += 1
        if (abs(r - BLUE[0]) <= TOLERANCE and abs(g - BLUE[1]) <= TOLERANCE
                and abs(b - BLUE[2]) <= TOLERANCE):
            blue += 1
    return blue / total if total else None


# ---------------------------------------------------------------- the engines

class Engine:
    """A text reader. A new one is this plus a line in ENGINES."""

    name = ""
    needs = ()            # programs that have to be on PATH

    @classmethod
    def available(cls):
        return all(shutil.which(p) for p in cls.needs)

    @classmethod
    def read(cls, png, args):
        raise NotImplementedError


class Tesseract(Engine):
    """Page mode 11, and four times the size.

    The page mode matters more than the scale: as a block of text the lines
    run together and the headings come out wrong, and read as loose lines
    they come out right. The face has to be scaled up because eight by
    sixteen is small for a recogniser.
    """

    name = "tesseract"
    needs = ("tesseract", "ffmpeg")

    @classmethod
    def read(cls, png, args):
        out = subprocess.run(["tesseract", png, "stdout", "--psm", str(args.psm)],
                             check=True, capture_output=True, text=True)
        return out.stdout


ENGINES = {e.name: e for e in (Tesseract,)}


# ---------------------------------------------------------------- the reading

def readText(args, tmp):
    """The text on the screen, once."""
    engine = ENGINES.get(args.engine)
    if engine is None:
        print("screenread: no such engine: %s" % args.engine, file=sys.stderr)
        return None
    if not engine.available():
        print("screenread: %s is not installed, skipping" % args.engine,
              file=sys.stderr)
        return None
    ppm = args.ppm or os.path.join(tmp, "screen.ppm")
    png = os.path.join(tmp, "screen.png")
    if args.ppm is None:
        grab(args.qmp_port, ppm)
    upscale(ppm, png, args.scale)
    return engine.read(png, args)


def readBlue(args, tmp):
    """How much of the screen is the bugcheck colour, once."""
    ppm = args.ppm or os.path.join(tmp, "screen.ppm")
    if args.ppm is None:
        grab(args.qmp_port, ppm)
    return blueFraction(ppm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ppm", help="read this picture instead of taking one")
    ap.add_argument("--qmp-port", type=int)
    ap.add_argument("--want", choices=("text", "bugcheck"), default="text")
    ap.add_argument("--engine", default="tesseract")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--psm", default="11")
    ap.add_argument("--poll", help="keep reading until this text appears")
    ap.add_argument("--wait", action="store_true",
                    help="keep reading until the bugcheck colour appears")
    ap.add_argument("--timeout", type=float, default=180)
    ap.add_argument("--interval", type=float, default=3.0)
    ap.add_argument("--keep", help="write the last picture here")
    args = ap.parse_args()

    if args.ppm is None and args.qmp_port is None:
        ap.error("either --ppm or --qmp-port is needed")

    want = args.want
    looping = bool(args.poll) or args.wait

    with tempfile.TemporaryDirectory() as tmp:
        deadline = time.monotonic() + args.timeout
        while True:
            if want == "text":
                text = readText(args, tmp)
                if text is None:
                    return SKIP
                sys.stdout.write(text)
                sys.stdout.flush()
                if not args.poll or args.poll in text:
                    return 0
            else:
                frac = readBlue(args, tmp)
                if frac is None:
                    print("screenread: the picture is unreadable", file=sys.stderr)
                    return 1
                if not args.wait:
                    print("blue %.1f%%" % (100 * frac))
                    return 0 if frac >= THRESHOLD else 1
                if frac >= THRESHOLD:
                    print("blue %.1f%% -- bugcheck" % (100 * frac))
                    if args.keep:
                        os.makedirs(os.path.dirname(os.path.abspath(args.keep)),
                                    exist_ok=True)
                        shutil.copyfile(os.path.join(tmp, "screen.ppm"), args.keep)
                    return 0
                print("blue %.1f%%" % (100 * frac))

            if not looping or time.monotonic() >= deadline:
                break
            time.sleep(args.interval)

        if want == "text":
            print("screenread: %r never appeared" % args.poll, file=sys.stderr)
        else:
            print("screenread: no bugcheck within %g seconds" % args.timeout,
                  file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
