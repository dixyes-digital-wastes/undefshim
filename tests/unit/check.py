"""How a check reports, the same way tests/unit/check.h does for the C ones.

    [PASS] check_pte: 70 checks, 0 failures
    [FAIL] check_pte: want 0x1000 got 0x2000
    [SKIP] check_rewrite: capstone is not installed
    [PASS] check_entry_frame: all 31 register slots, ... and both exits

Colour follows https://no-color.org: NO_COLOR, set to anything at all, turns
it off, and so does TERM being dumb or the output not being a terminal, which
is what keeps a log file and a pipe free of escapes.
"""

import os
import sys

GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
BOLD = "\033[1m"
RESET = "\033[0m"


def colour():
    """Whether to put escapes in the output."""
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("TERM") == "dumb":
        return False
    return sys.stdout.isatty()


def escape(sequence):
    """The escape, or nothing at all, so a line is written once."""
    return sequence if colour() else ""


class Check:
    """One check's report. The name is what a line is grepped by."""

    def __init__(self, name):
        self.name = name

    def fail(self, message):
        print("%s[FAIL]%s %s: %s" % (escape(RED), escape(RESET), self.name, message))

    def note(self, message):
        print("      %s: %s" % (self.name, message))

    def skip(self, reason):
        print("%s[SKIP]%s %s: %s" % (escape(YELLOW), escape(RESET), self.name, reason))
        return 0

    def summary(self, checks, failures):
        """The last line of a check that counts. Returns the exit status."""
        word = "failure" if failures == 1 else "failures"
        print("%s%s%s %s: %s%d%s checks, %d %s"
              % (escape(RED if failures else GREEN),
                 "[FAIL]" if failures else "[PASS]", escape(RESET), self.name,
                 escape(BOLD), checks, escape(RESET), failures, word))
        return 1 if failures else 0

    def passed(self, what):
        """The last line of a check that asserts instead of counting."""
        print("%s[PASS]%s %s: %s" % (escape(GREEN), escape(RESET), self.name, what))
        return 0
