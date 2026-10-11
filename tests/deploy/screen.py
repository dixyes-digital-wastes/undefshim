#!/usr/bin/env python3
"""Read the machine's screen: take the picture, say what is on it, wait for it.

Four things are asked of it, and they cost very different amounts:

  picture   the frame itself, saved as a PPM. Convert it before looking at it:
            ffmpeg -i x.ppm x.png
  colour    what the frame is made of, from a sample of its pixels
  layout    where those colours are: a console has a title bar across the top
            and a border down the left, a boot screen has neither
  text      what is written, which is the expensive one

    screen.py --qmp-port 4448 --out build/x/screen.ppm
    screen.py --ppm build/x/screen.ppm --text
    screen.py --qmp-port 4448 --text --poll 'broken'
    screen.py --qmp-port 4448 --want bugcheck
    screen.py --qmp-port 4448 --stable 3
    screen.py --qmp-port 4448 --stuck 60
    screen.py --qmp-port 4448 --want console --text --all

Exits 0 when what was asked for was reached, 1 when it was not, 2 when the
engine that reads text is not installed, 3 when the machine was called stuck
(a still screen that stayed still after being poked), 4 when it blue-screened.
Run the text cases under the interpreter that has it:

    . tests/deploy/venv/bin/activate
    screen.py --qmp-port 4448 --text
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SKIP = 2

# ---------------------------------------------------------------- the monitor

# Where the monitor is. Defaults to this machine; a machine being watched on
# another one names its address, and then everything here works over the
# network exactly as it does locally -- the monitor protocol does not care.
QMP_HOST = "127.0.0.1"

# Where a picture is asked for on a machine that is not this one. It has to be
# a path that machine has; it is overwritten every reading.
REMOTE_PPM = "/tmp/undefshim-screen.ppm"


def grab(port, path):
    """Ask the machine's monitor for a picture of the screen."""
    sock = socket.create_connection((QMP_HOST, port), timeout=20)
    f = sock.makefile("rwb")
    f.readline()
    for request in ({"execute": "qmp_capabilities"},
                    {"execute": "screendump", "arguments": {"filename": path}}):
        f.write((json.dumps(request) + "\n").encode())
        f.flush()
        while True:
            line = f.readline()
            if not line:
                raise OSError("the monitor closed the connection")
            reply = json.loads(line)
            if "error" in reply:
                raise OSError(reply["error"].get("desc", "the monitor refused"))
            if "return" in reply:
                break


def fetch(remote, path, local):
    """Bring a picture back from the machine the monitor wrote it on.

    screendump writes on the machine the monitor is running on, so watching a
    machine somewhere else means the file has to come back before it can be
    read. Named the way ssh names a host, because that is what carries it.
    """
    out = subprocess.run(
        ["scp", "-F", os.path.expanduser("~/.ssh/config"),
         "%s:%s" % (remote, path), local],
        capture_output=True, text=True)
    if out.returncode != 0:
        raise OSError("could not fetch %s: %s" % (path, out.stderr.strip()))


def poke(port, keys="ret", move=(80, 60)):
    """Knock on the guest's input, and say whether it could be sent.

    Two ways in, and the pointer is the better one: moving it is a change the
    machine makes to the frame and nothing else, where a keystroke is an event
    the program on the other end may act on -- which is the difference between
    asking whether the machine is there and changing what it is doing.

    Whether the guest acts on either is a separate question, and one this
    cannot answer: a pointer that arrives at a machine with no input driver
    does not move, exactly like one that arrives at a machine that has
    stopped.
    """
    sock = socket.create_connection((QMP_HOST, port), timeout=20)
    f = sock.makefile("rwb")
    f.readline()
    f.write((json.dumps({"execute": "qmp_capabilities"}) + "\n").encode())
    f.flush()
    f.readline()
    for command in ("mouse_move %d %d" % move, "sendkey %s" % keys):
        f.write((json.dumps({"execute": "human-monitor-command",
                             "arguments": {"command-line": command}})
                 + "\n").encode())
        f.flush()
        while True:
            line = f.readline()
            if not line:
                return False
            reply = json.loads(line)
            if "error" in reply:
                return False
            if "return" in reply:
                break
    return True


# ---------------------------------------------------------------- the picture

# The console window's title bar, and the colour the frame is otherwise made
# of. The tolerance is for the antialiasing of the text drawn on top
CONSOLE_BLUE = (18, 115, 170)
DARK = (12, 12, 12)
LIGHT = 200
COLOUR_TOLERANCE = 24

# The bugcheck colour, and how much of the frame has to be it
BLUE = (0, 61, 146)
BLUE_TOLERANCE = 32
BLUE_THRESHOLD = 0.5

# A run of rows this tall at the top of the frame is a title bar. The console's
# is 85; a boot screen has nothing like it
TITLE_BAR_MIN_ROWS = 20
# How much of a light frame has to be the window colour for it to be a page
# Windows drew rather than the firmware's own console. Both are a light field
# with dark text on it, and the colours alone cannot tell one from the other:
# what separates them is that Windows draws with its own blue. The firmware's
# console has none of it, and a Windows page has several percent of it: the
# driver's console 0.0% of the frame, the OOBE region page 4.2%. The
# watermark the firmware draws in the middle is not that colour, so it does
# not move the reading
UI_COLOUR_MIN = 0.03
# How much of a row has to be off the background for it to be part of a bar
ROW_FULL = 0.8
# How much of a row has to carry something for it to be a line of text
ROW_TEXT = 0.03

# The states, in the order they are tested
BLANK = "blank"
BUGCHECK = "bugcheck"
FIRMWARE = "firmware"
WINDOWS = "windows"
BOOT = "boot"
CONSOLE = "console"
UNKNOWN = "unknown"

STATES = (BLANK, BUGCHECK, FIRMWARE, WINDOWS, BOOT, CONSOLE, UNKNOWN)

# What the text can add to a state the colours could only place. A failure is
# ahead of a success and both are ahead of the prompt, which is what a console
# that failed also sits at once its output has scrolled past.
#
# Matched against the text with its whitespace gone, because the detector
# returns one line as several boxes and a pattern can otherwise be split by
# where a box happened to end. A pattern is written with no spaces in it.
TEXT_STATES = (
    # the volume's own script got to the end of its list
    ("demo-ok", re.compile(r"entrypoint:done", re.I)),
    ("demo-fail", re.compile(
        r"isnotrecognized"          # a command that is not on the drive
        r"|cannotfind"              # a file that is not there
        r"|syntaxofthecommand"      # a line the shell could not read
        r"|accessisdenied"
        r"|notenough"               # memory, arguments, whatever it asked for
        r"|fail"                    # what the program itself said
        r"|error", re.I)),
    ("demo-pass", re.compile(r"pass", re.I)),
    # the script started, which is the first thing it says
    ("demo-ran", re.compile(r"entrypoint:", re.I)),
    # a console sitting at its prompt: the shell's own directory, which the
    # title bar and the prompt both name
    ("console-idle", re.compile(r"system32>", re.I)),
)


def normalise(text):
    """The text with its whitespace gone, for matching."""
    return re.sub(r"\s+", "", text or "")


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


def near(r, g, b, colour):
    return (abs(r - colour[0]) <= COLOUR_TOLERANCE
            and abs(g - colour[1]) <= COLOUR_TOLERANCE
            and abs(b - colour[2]) <= COLOUR_TOLERANCE)


class Frame:
    """One picture of the screen, measured."""

    def __init__(self, path):
        self.path = path
        data, width, height, off = readPpm(path)
        if data is None:
            raise ValueError("the picture is not a readable PPM")
        self.data = data
        self.width = width
        self.height = height
        self.off = off
        self._colours = None
        self._rows = None
        self._signature = None

    def colours(self, stride=83):
        """How much of the frame each of the interesting colours is.

        Sampled on a stride that is not a multiple of anything the frame is
        made of, so the sample is spread rather than landing on one glyph
        column.
        """
        if self._colours is not None:
            return self._colours
        dark = console = bug = light = 0
        total = 0
        data, off = self.data, self.off
        for p in range(off, off + self.width * self.height * 3 - 2, 3 * stride):
            r, g, b = data[p], data[p + 1], data[p + 2]
            total += 1
            if near(r, g, b, DARK):
                dark += 1
            elif near(r, g, b, CONSOLE_BLUE):
                console += 1
            elif (abs(r - BLUE[0]) <= BLUE_TOLERANCE
                  and abs(g - BLUE[1]) <= BLUE_TOLERANCE
                  and abs(b - BLUE[2]) <= BLUE_TOLERANCE):
                bug += 1
            elif min(r, g, b) >= LIGHT:
                light += 1
        self._colours = {
            "dark": dark / total,
            "console": console / total,
            "bugcheck": bug / total,
            "light": light / total,
        }
        return self._colours

    def rows(self):
        """How much of each row is off the dark background.

        This is the layout: a title bar is a run of rows that are almost all
        off the background across the whole width, and the text area under it
        is rows that are mostly background with glyphs in them.
        """
        if self._rows is not None:
            return self._rows
        data, off, w = self.data, self.off, self.width
        out = []
        for y in range(0, self.height, 2):
            base = off + y * w * 3
            n = 0
            for x in range(0, w, 4):
                p = base + x * 3
                if not near(data[p], data[p + 1], data[p + 2], DARK):
                    n += 1
            out.append(n / (w // 4))
        self._rows = out
        return out

    def titleBarRows(self):
        """How tall the run of full rows at the top is, in frame rows."""
        n = 0
        for density in self.rows():
            if density < ROW_FULL:
                break
            n += 1
        return n * 2

    def signature(self, stride=997):
        """A short summary of the frame, for telling two apart.

        Taken from the pixels and not from the measurements, so that a change
        the classifier does not look at still counts as a change.
        """
        if self._signature is not None:
            return self._signature
        off = self.off
        end = off + self.width * self.height * 3 - 2
        self._signature = tuple(self.data[p] for p in range(off, end, stride))
        return self._signature

    def distance(self, other):
        """How much of the sample differs from another frame's."""
        a, b = self.signature(), other.signature()
        n = min(len(a), len(b))
        if n == 0:
            return 0.0
        return 1.0 - sum(1 for i in range(n) if a[i] == b[i]) / n

    def differs(self, other, pixels=8):
        """Whether any block of the frame changed at all, including a pointer.

        Every pixel rather than a sample, because what is being looked for is
        a pointer: it is a dozen pixels in a corner, which the stride used for
        telling frames apart steps straight over. The count is a threshold
        rather than one, so that the dithering of a cursor or a blinking one
        does not read as a redrawn screen.
        """
        a, b = self.data, other.data
        n = min(len(a), len(b))
        if n == 0 or self.width != other.width or self.height != other.height:
            return True
        different = 0
        lo, hi = self.off, n
        for p in range(lo, hi - 2, 3):
            if (a[p] != b[p] or a[p + 1] != b[p + 1] or a[p + 2] != b[p + 2]):
                different += 1
                if different >= pixels:
                    return True
        return False

    def classify(self):
        """The state, why, and the measurements it was decided from."""
        colours = self.colours()

        if colours["dark"] >= 0.995 or colours["light"] >= 0.995:
            return BLANK, colours, "one colour over the whole frame"
        if colours["bugcheck"] >= BLUE_THRESHOLD:
            return BUGCHECK, colours, "%.0f%% of it is the bugcheck blue" % (
                100 * colours["bugcheck"])
        if colours["light"] >= 0.3:
            # A light frame is the firmware's console, unless Windows drew it:
            # the pages Windows shows in a light theme carry its own blue, and
            # calling one of those the firmware says "Windows never started"
            # about a machine that got all the way to its own interface
            if colours["console"] >= UI_COLOUR_MIN:
                return WINDOWS, colours, "%.0f%% of it is the window colour" % (
                    100 * colours["console"])
            return FIRMWARE, colours, "%.0f%% of it is light" % (
                100 * colours["light"])
        if self.titleBarRows() >= TITLE_BAR_MIN_ROWS:
            return CONSOLE, colours, "a %d row bar across the top" % (
                self.titleBarRows())
        if colours["console"] >= 0.03 or colours["dark"] >= 0.3:
            return BOOT, colours, "blue and dark with no bar"
        return UNKNOWN, colours, "nothing recognised"


def refine(state, text):
    """What the text says about a state the colours could only place."""
    if not text:
        return None
    flat = normalise(text)
    for name, pattern in TEXT_STATES:
        if pattern.search(flat):
            return name
    return None


# ---------------------------------------------------------------- the text

class Engine:
    """A text reader. A new one is this plus a line in ENGINES."""

    name = ""

    @classmethod
    def available(cls):
        raise NotImplementedError

    @classmethod
    def read(cls, image, args):
        raise NotImplementedError


class RapidOCR(Engine):
    """The detector and recogniser the ONNX runtime runs, on the frame as it is.

    It reads an eight by sixteen cell without being asked to, and returns where
    each piece of text is, which is what lets a line be put back together when
    the detector returned it in parts.
    """

    name = "rapidocr"
    _engine = None

    @classmethod
    def available(cls):
        if importlib.util.find_spec("rapidocr_onnxruntime") is None:
            return False
        # Installed is not the same as usable. The engine and the runtime under
        # it are compiled, and a machine carrying a package built for another
        # interpreter has the files and cannot use them. A check that stopped
        # at the file would say the text can be read and then report every
        # screen as saying nothing, which is the one answer that must never be
        # guessed at
        try:
            from rapidocr_onnxruntime import RapidOCR  # noqa: F401
        except Exception:
            return False
        return True

    @classmethod
    def read(cls, image, args):
        # Before the import: the runtime decides how much it says when it is
        # loaded, and what it has to say about this machine's PCI topology is
        # not a thing a caller asked for
        os.environ.setdefault("ORT_LOGGING_LEVEL", "3")
        from rapidocr_onnxruntime import RapidOCR as Detector

        if cls._engine is None:
            cls._engine = Detector()
        result, _ = cls._engine(image)
        return boxesToText(result)


ENGINES = {e.name: e for e in (RapidOCR,)}

# How far apart two boxes' tops may be and still be the same line. A cell is
# eight by sixteen, so half a line. Compared by the tops and not by the boxes'
# extents: a detector's box is taller than the text in it, so one line's bottom
# reaches past the next line's top and the extents always overlap
LINE_TOLERANCE = 8.0


def boxesToText(boxes):
    """The recognised boxes, back in reading order.

    A line comes back as several boxes, split where the detector found a gap,
    so they are put together by where they are rather than by the order they
    arrived in -- the order is not promised to be anything.
    """
    if not boxes:
        return ""
    items = []
    for box, text, _score in boxes:
        ys = [p[1] for p in box]
        xs = [p[0] for p in box]
        items.append({"top": min(ys), "left": min(xs), "text": text})
    items.sort(key=lambda i: (i["top"], i["left"]))

    lines = []
    for item in items:
        if lines and item["top"] - lines[-1]["top"] < LINE_TOLERANCE:
            lines[-1]["parts"].append(item)
        else:
            lines.append({"parts": [item], "top": item["top"]})

    out = []
    for line in lines:
        parts = sorted(line["parts"], key=lambda i: i["left"])
        out.append(" ".join(p["text"] for p in parts))
    return "\n".join(out)


def readText(args, ppm):
    """What is written on the screen, once. None when it cannot be read."""
    engine = ENGINES.get(args.engine)
    if engine is None:
        print("screen: no such engine: %s" % args.engine, file=sys.stderr)
        return None
    if not engine.available():
        print("screen: %s is not installed or cannot be used here, skipping"
              % args.engine, file=sys.stderr)
        return None
    try:
        return engine.read(ppm, args)
    except Exception as why:
        # A reader that cannot run is not a screen that said nothing. The
        # caller of this one stops a run when the text it wanted is missing,
        # and only the reader's absence is a reason to stop watching instead
        print("screen: %s could not read: %s" % (args.engine, why),
              file=sys.stderr)
        return None


# The line a bugcheck puts its reason on, and the reason. Worth reading even
# when the caller did not ask for text: a blue screen is the end of a run, and
# this is the only thing on it that says why.
STOP_CODE = re.compile(r"stop\s*code\s*:?\s*(.+)", re.I)


def stopCode(text):
    """The stop code off a bugcheck screen, or None when it is not there."""
    for line in (text or "").splitlines():
        match = STOP_CODE.search(line)
        if match:
            return match.group(1).strip()
    return None


def tryText(args, ppm):
    """The screen's text, for a caller that did not ask for it.

    Empty when it cannot be read, and quiet about it: this is a bonus, and a
    run that cannot read text should still report the bugcheck it saw
    """
    engine = ENGINES.get(args.engine)
    if engine is None or not engine.available():
        return ""
    try:
        return engine.read(ppm, args) or ""
    except Exception:
        return ""


# ---------------------------------------------------------------- the waiting

def main():
    ap = argparse.ArgumentParser(
        description="Read the machine's screen, once or until it reaches "
                    "something.",
        epilog="The states to wait for are " + ", ".join(STATES)
               + ", and what the text can add: "
               + ", ".join(name for name, _ in TEXT_STATES) + ".")
    ap.add_argument("--qmp-port", type=int, help="where the machine's monitor is")
    ap.add_argument("--qmp-host", default=None,
                    help="which machine's monitor that is (default this one)")
    ap.add_argument("--fetch", default=None,
                    help="an ssh host to bring the picture back from, when the "
                         "monitor is on another machine")
    ap.add_argument("--ppm", help="read this picture every time instead of "
                                  "taking one")
    ap.add_argument("--out", help="write the picture here")
    ap.add_argument("--text", action="store_true",
                    help="read what is written, which is the slow part")
    ap.add_argument("--engine", default="rapidocr")
    ap.add_argument("--check-engine", action="store_true",
                    help="say whether the text can be read at all, and exit")
    ap.add_argument("--want", help="wait until the state is one of these, "
                                   "comma separated")
    ap.add_argument("--poll", help="wait until the screen says this text, "
                                   "which reads the text")
    ap.add_argument("--stable", type=int, default=0,
                    help="wait until the frame has not changed for this many "
                         "readings in a row")
    ap.add_argument("--stuck", type=float, default=0,
                    help="call the machine stuck when the frame has not "
                         "changed for this many seconds; poke it first, and "
                         "only call it stuck if that changes nothing")
    ap.add_argument("--poke", default="ret",
                    help="the key sent as well as moving the pointer, when "
                         "a still screen is being judged")
    ap.add_argument("--poke-grace", type=float, default=10.0,
                    help="seconds to keep reading after a poke")
    ap.add_argument("--interval", type=float, default=5.0,
                    help="seconds between readings")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--all", action="store_true",
                    help="print every reading, not only the changes")
    ap.add_argument("--keep", help="write the last picture here when the "
                                   "state was reached")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    global QMP_HOST
    if args.qmp_host:
        QMP_HOST = args.qmp_host

    if args.ppm is None and args.qmp_port is None and not args.check_engine:
        ap.error("either --ppm or --qmp-port is needed")
    if args.poll:
        args.text = True

    if args.check_engine:
        engine = ENGINES.get(args.engine)
        if engine is None:
            print("screen: no such engine: %s" % args.engine, file=sys.stderr)
            return SKIP
        if not engine.available():
            print("screen: %s is not installed" % args.engine, file=sys.stderr)
            return SKIP
        print("screen: %s is available" % args.engine)
        return 0

    want = set(s.strip() for s in args.want.split(",")) if args.want else set()
    # A state the colours cannot tell apart is one only the text can, so asking
    # for one of those reads the text too. Without this the wait is for a state
    # that is never produced - the reader only ever answers with the colours -
    # and it runs out its whole timeout on a screen that has been saying what
    # was wanted all along
    if want & {name for name, _ in TEXT_STATES}:
        args.text = True
    looping = bool(want) or bool(args.poll) or args.stable > 0 or args.stuck > 0

    with tempfile.TemporaryDirectory() as tmp:
        deadline = time.monotonic() + args.timeout
        started = time.monotonic()
        previous = None
        unchanged = 0
        lastState = None
        # When the frame last changed, and whether the still one has been
        # poked. A machine that answers to a pointer or a key is a machine that
        # is still there, so the poke comes before the verdict -- and what it
        # answered with is the frame before the poke, kept for the comparison
        movedAt = started
        pokedAt = None
        poked = False
        pokedFrame = None

        while True:
            ppm = args.ppm or os.path.join(tmp, "screen.ppm")
            if args.ppm is None:
                try:
                    # A machine somewhere else is asked for a path on itself,
                    # and the file then comes back: the name has to exist
                    # there, so it cannot be this side's temporary directory
                    where = REMOTE_PPM if args.fetch else ppm
                    grab(args.qmp_port, where)
                    if args.fetch:
                        fetch(args.fetch, where, ppm)
                except (OSError, ValueError) as exc:
                    print("screen: %s" % exc, file=sys.stderr)
                    return 1
            try:
                frame = Frame(ppm)
            except ValueError as exc:
                print("screen: %s" % exc, file=sys.stderr)
                return 1
            if args.out:
                os.makedirs(os.path.dirname(os.path.abspath(args.out)),
                            exist_ok=True)
                shutil.copyfile(ppm, args.out)

            # A change is measured against the previous frame, so a machine
            # that is animating a spinner is not a machine that has stopped
            moved = previous.distance(frame) if previous is not None else 1.0
            unchanged = 0 if moved > 0.001 else unchanged + 1
            now = time.monotonic()
            if moved > 0.001:
                movedAt = now
                poked = False
                pokedAt = None
                pokedFrame = None

            base, colours, why = frame.classify()
            state = base
            text = ""
            if args.text:
                got = readText(args, ppm)
                if got is None:
                    return SKIP
                text = got
                state = refine(base, text) or base

            elapsed = time.monotonic() - started
            if args.json:
                print(json.dumps({"t": round(elapsed, 1), "state": state,
                                  "moved": round(moved, 5),
                                  "colours": {k: round(v, 4)
                                              for k, v in colours.items()}}))
            elif args.all or state != lastState:
                print("[%6.1fs] %-12s %s%s" % (
                    elapsed, state, why,
                    " (unchanged x%d)" % unchanged if unchanged else ""))
                for line in text.strip().splitlines()[-6:]:
                    print("             | %s" % line.rstrip())
            sys.stdout.flush()

            reached = False
            if args.poll and normalise(args.poll) in normalise(text):
                print("screen: the screen says %r" % args.poll)
                reached = True
            if want and (state in want or base in want):
                print("screen: reached %s" % state)
                reached = True
            if args.stable and unchanged >= args.stable:
                print("screen: the screen has not changed over %d readings"
                      % unchanged)
                reached = True
            if reached:
                if args.keep:
                    os.makedirs(os.path.dirname(os.path.abspath(args.keep)),
                                exist_ok=True)
                    shutil.copyfile(ppm, args.keep)
                return 0

            # A bugcheck ends the question whatever was being waited for: the
            # machine is not going to reach anything else, and a caller that
            # waited out its whole timeout for a state this machine can no
            # longer produce would have learned nothing. It is also the one
            # screen whose text is worth reading unasked - the stop code is
            # the only thing on it that says what happened.
            if base == BUGCHECK:
                code = stopCode(text) if text else None
                if code is None:
                    code = stopCode(tryText(args, ppm))
                print("screen: bugcheck%s" % (": " + code if code
                                              else " (stop code unread)"))
                if args.keep:
                    os.makedirs(os.path.dirname(os.path.abspath(args.keep)),
                                exist_ok=True)
                    shutil.copyfile(ppm, args.keep)
                return 4

            # A screen that has stopped changing is poked before it is judged:
            # a machine that answers to a pointer or a key was drawing nothing,
            # not gone, and a machine that does not is stuck. The answer is
            # looked for with the whole frame, because what a pointer changes
            # is a dozen pixels and the sample that tells two frames apart
            # steps over them
            if args.stuck > 0:
                still = now - movedAt
                if not poked and still >= args.stuck:
                    if args.qmp_port is None:
                        print("screen: no change for %.0fs, and no monitor to "
                              "poke it through" % still)
                        poked = True
                        pokedAt = now
                        pokedFrame = frame
                    else:
                        pokedFrame = frame
                        sent = poke(args.qmp_port, args.poke)
                        print("screen: no change for %.0fs -- poked it (%s)"
                              % (still, "sent" if sent else "refused"))
                        poked = True
                        pokedAt = now
                elif poked and pokedAt is not None \
                        and now - pokedAt >= args.poke_grace:
                    if pokedFrame is not None and frame.differs(pokedFrame):
                        # It answered: the machine is running and was slow, so
                        # the clock starts again rather than the run ending
                        print("screen: it answered the poke -- still running, "
                              "just slow (%ds without a redraw)"
                              % (now - movedAt))
                        movedAt = now
                        poked = False
                        pokedAt = None
                        pokedFrame = None
                    else:
                        print("screen: nothing answered the poke after %.0fs "
                              "-- stuck" % (now - movedAt))
                        if args.keep:
                            os.makedirs(
                                os.path.dirname(os.path.abspath(args.keep)),
                                exist_ok=True)
                            shutil.copyfile(ppm, args.keep)
                        return 3

            if not looping:
                return 0

            previous = frame
            lastState = state

            if time.monotonic() >= deadline:
                asked = ", ".join(sorted(want)) or (repr(args.poll) if args.poll
                                                    else "a still screen")
                print("screen: did not reach %s within %g seconds"
                      % (asked, args.timeout), file=sys.stderr)
                return 1
            time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
