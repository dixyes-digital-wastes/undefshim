#!/usr/bin/env python3
"""Check that every register slot the entry writes into the frame is read back.

The entry's frame is written field by field and read back the same way, and
the two lists are not in one place: the stores are in the middle of the file
and the loads near the end, with the stack switch and the call to the handler
between them. A slot written and never read is therefore invisible in both
lists, and it is the worst kind of invisible: the interrupted code continues
with a live register holding an address from this handler's own code, and the
first instruction that treats it as a pointer writes wherever it points. That
is what `ldr x11, [x9, #US_FRAME_X11]` was missing, and it cost several runs
to find.

The slots are counted from the field name rather than from the register the
instruction names, because the two are not always the same: the three the
bootstrap spends are stored out of a register that is not the one they belong
to (`str x17, [x16, #US_FRAME_X16]`), and counting registers would leave those
three out of the check entirely. A pair covers the slot it names and the one
after it, which is what `stp` does.

This is a text scan rather than a test of the generated code because the
mistake is a missing line, and a line that is not there cannot be assembled
to be examined.

Usage: check_entry_frame.py [path to entry.S]
Exit is non-zero when a slot is written and not read back.
"""

import re
import sys

STORE = re.compile(r"^\s*(str|stp)\s+[^[]*\[\s*x16,\s*#US_FRAME_X([0-9]+)\]")
LOAD = re.compile(r"^\s*(ldr|ldp)\s+[^[]*\[\s*x9,\s*#US_FRAME_X([0-9]+)\]")


def slots(match):
    """The frame slots one instruction covers.

    A pair covers two: it is written to the slot it names and the slot after
    it, which is the layout the C structure has and the reason `stp` can be
    used at all.
    """
    first = int(match.group(2))
    return {first, first + 1} if match.group(1) in ("stp", "ldp") else {first}


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "payload/entry.S"
    stored = set()
    read = set()

    for line in open(path):
        m = STORE.match(line)
        if m:
            stored |= slots(m)
            continue
        m = LOAD.match(line)
        if m:
            read |= slots(m)

    missing = sorted(stored - read)
    print("%s: slots written %s" % (path, sorted(stored)))
    print("%s: slots read back %s" % (" " * len(path), sorted(read)))
    if missing:
        print("FAIL: %s written and never read back"
              % ", ".join("x%d" % n for n in missing))
        return 1
    print("PASS: every slot the entry writes is read back")
    return 0


if __name__ == "__main__":
    sys.exit(main())
