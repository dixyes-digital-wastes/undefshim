#!/usr/bin/env python3
"""Check what the watching tool reads, and when it says a machine has halted.

Two things are worth pinning down, because both have been wrong before:

  - the pool is read only when an address for it is known. Read without one,
    the tool would ask the monitor about address zero and get an answer that
    means nothing.
  - a halted verdict comes from every processor standing still, not from one:
    a machine whose first processor is waiting on another one is not stopped.
"""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "deploy"))
import bugcheck_probe  # noqa: E402
import watch  # noqa: E402


def run(argv, processors, poolMemory=""):
    """Run the watcher with the monitor stubbed out, and return (rc, output)."""
    output = io.StringIO()
    commands = []

    def hmp(sock_file, command):
        commands.append(command)
        return poolMemory

    with patch.object(sys, "argv", argv), \
            patch.object(watch, "connect", return_value=(object(), object())), \
            patch.object(watch, "processors", return_value=processors), \
            patch.object(watch, "hmp", side_effect=hmp), \
            patch.object(watch.time, "sleep"), \
            contextlib.redirect_stdout(output):
        rc = watch.main()
    return rc, output.getvalue(), commands


def main():
    # Frozen from the first sample: every processor at the same place.
    rc, text, commands = run(["watch.py", "--frozen", "0", "--timeout", "1"],
                             (0x1000, 0x2000))
    assert rc == 3, rc
    assert "halted" in text, text
    assert "cpu0 pc = 0x1000" in text, text
    # No pool was named, so the monitor must not have been asked about one
    assert not any(c.startswith("xp ") for c in commands), commands

    # A pool that was named is read, and the figures come out of it
    # The entry starts at the pool plus one word, and its own first field
    # after the magic is entries: 43 of them in this answer
    rc, text, commands = run(["watch.py", "--frozen", "0", "--timeout", "1",
                              "--pool", "0x1380e0000"], (0x1000,),
                             "00000001380e0008: 0x000000000000002b"
                             " 0x000000000000002b\n"
                             "00000001380e0018: 0x0000000000000002"
                             " 0x0000000000000000\n")
    assert rc == 3, rc
    assert any(c.startswith("xp ") and "1380e0008" in c for c in commands), commands
    assert "entries=43" in text, text

    # Moving processors are not a halt, and the wait runs out instead
    moving = [(0x1000,), (0x2000,), (0x3000,), (0x4000,), (0x5000,)]
    calls = {"n": 0}

    def stepping(_monitor):
        p = moving[calls["n"] % len(moving)]
        calls["n"] += 1
        return p

    output = io.StringIO()
    with patch.object(sys, "argv", ["watch.py", "--frozen", "3", "--timeout", "0.2",
                                    "--interval", "0", "--report", "1"]), \
            patch.object(watch, "connect", return_value=(object(), object())), \
            patch.object(watch, "processors", side_effect=stepping), \
            patch.object(watch, "hmp", return_value=""), \
            patch.object(watch.time, "sleep"), \
            contextlib.redirect_stdout(output):
        rc = watch.main()
    assert rc == 1, rc
    assert "cpu" not in output.getvalue(), output.getvalue()

    # The bugcheck record, which is what a halt is read with
    commands = []

    def memoryReply(sock_file, request):
        commands.append(request["arguments"]["command-line"])
        return {"return": "00000001380e0008: 0x000000000000002b"
                          " 0xfffff80100000920\n"
                          "00000001380e0018: 0x0000000000000000"
                          " 0x0000000000000000\n"
                          "00000001380e0028: 0x0000000000000000\n"}

    with patch.object(bugcheck_probe, "cmd", side_effect=memoryReply):
        words = bugcheck_probe.readWords(object(), 0x1380e0008, 5, physical=True)
        assert commands[-1] == "xp /5gx 0x1380e0008"
        assert words == [0x2b, 0xfffff80100000920, 0, 0, 0]
        offset, record = bugcheck_probe.findBugCheckRecord(object(),
                                                          0xfffff80100000000)
        assert offset == bugcheck_probe.KI_BUGCHECK_DATA_RVA and record == words
        assert commands[-1] == "x /5gx 0xfffff80100dba5e0"
    with patch.object(bugcheck_probe, "readWords", return_value=[0] * 5):
        assert bugcheck_probe.findBugCheckRecord(object(), 0) == (None, None)

    print("PASS: watching reads the pool only when named, and halts on all CPUs")


if __name__ == "__main__":
    main()
