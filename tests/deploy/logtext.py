"""A run's log, as the text it was written as.

The driver colours a line by putting an escape sequence around the tag and
around each value in it, so the bytes of a line are not the line: a pattern
like "pool: pa=" is not in the file at all once the tag has a colour, because
there is an escape between the two. Everything that reads a log to work
something out goes through here, and what it gets back is what was said.

The carriage returns a serial port needs are taken off for the same reason.
"""

import re

_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def plain(text):
    """The text of a log that has already been read."""
    return _ESCAPE.sub("", text).replace("\r\n", "\n")


def read(path):
    """A log file, as text. Latin-1 because a serial log is bytes, and a
    byte that is not UTF-8 should not stop the reader."""
    with open(path, "rb") as f:
        return plain(f.read().decode("latin1"))
