#!/usr/bin/env python3
"""Check that register-only watching neither reads nor classifies a screen."""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "deploy"))
import machine_watch  # noqa: E402
import bugcheck_probe  # noqa: E402


def main():
    output = io.StringIO()
    with tempfile.TemporaryDirectory() as shots:
        argv = ["machine_watch.py", "--no-screen", "--frozen", "0",
                "--timeout", "1", "--shots", shots]
        with patch.object(sys, "argv", argv), \
                patch.object(machine_watch, "connect", return_value=(object(), object())), \
                patch.object(machine_watch, "processors", return_value=(0x1000, 0)), \
                patch.object(machine_watch, "cmd", side_effect=AssertionError("screen read")), \
                patch.object(machine_watch, "blueFraction", side_effect=AssertionError("pixel check")), \
                patch.object(machine_watch.time, "sleep"), \
                contextlib.redirect_stdout(output):
            assert machine_watch.main() == 3
        assert not list(Path(shots).iterdir())
    assert "screen not inspected" in output.getvalue()
    assert "no bugcheck" not in output.getvalue()
    commands = []

    def memoryReply(sock_file, request):
        commands.append(request["arguments"]["command-line"])
        return {"return": "00000001380e0008: 0x000000000000002b 0xfffff80100000920\n"
                          "00000001380e0018: 0x0000000000000000 0x0000000000000000\n"
                          "00000001380e0028: 0x0000000000000000\n"}

    with patch.object(bugcheck_probe, "cmd", side_effect=memoryReply):
        words = bugcheck_probe.readWords(object(), 0x1380e0008, 5, physical=True)
        assert commands[-1] == "xp /5gx 0x1380e0008"
        assert words == [0x2b, 0xfffff80100000920, 0, 0, 0]
        offset, record = bugcheck_probe.findBugCheckRecord(object(), 0xfffff80100000000)
        assert offset == bugcheck_probe.KI_BUGCHECK_DATA_RVA and record == words
        assert commands[-1] == "x /5gx 0xfffff80100dba5e0"
    with patch.object(bugcheck_probe, "readWords", return_value=[0] * 5):
        assert bugcheck_probe.findBugCheckRecord(object(), 0) == (None, None)
    print("PASS: register-only watch, physical pool reads and one-pointer bugchecks")


if __name__ == "__main__":
    main()
