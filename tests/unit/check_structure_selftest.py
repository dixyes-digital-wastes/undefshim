#!/usr/bin/env python3
"""Whether the structure checks would notice, if the tree broke one of them.

A check that cannot fail is worse than no check, because it is believed. Every
rule in check_structure.py is therefore given a tree with exactly that one
thing wrong with it, and the rule has to report it. The trees are copies of
this one, edited in the copy: nothing here touches the working tree.

    python3 check_structure_selftest.py [root]

The edits are deliberately the smallest that break the rule - one repeated
guard, one include that points the wrong way, one key that is not a key - so
that what is being tested is the rule and not the size of the mistake.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check as checklib

REPORT = checklib.Check("check_structure_selftest")

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
CHECKER = Path(__file__).resolve().parent / "check_structure.py"

checks = 0
failures = 0


def fail(message):
    global failures
    failures += 1
    REPORT.fail(message)


def note(message):
    REPORT.note(message)


def run(tree):
    """The checker's verdict on a tree, as (failures, output)."""
    result = subprocess.run([sys.executable, str(CHECKER), str(tree)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    return result.returncode, result.stdout


def copyTree():
    temp = Path(tempfile.mkdtemp(prefix="us-structure-"))
    tree = temp / "undefshim-ex2"

    shutil.copytree(ROOT, tree, ignore=shutil.ignore_patterns(
        ".git", "build", "venv", "__pycache__", "*.o", "*.a"))
    # The design document lives beside the tree, and one rule reads it
    docs = tree.parent / "undefshim-ex2-docs"
    docs.mkdir()
    shutil.copy(ROOT.parent / "undefshim-ex2-docs" / "design.md",
                docs / "design.md")
    return temp, tree


def edit(tree, relative, change):
    path = tree / relative
    text = path.read_text()
    new = change(text)
    if new == text:
        fail("{}: the edit did nothing, so the test proves nothing".format(relative))
        return False
    path.write_text(new)
    return True


def expect(rule, tree, what):
    global checks, failures
    code, output = run(tree)
    checks += 1
    if code == 0:
        failures += 1
        REPORT.fail("{}: a tree with {} passed".format(rule, what))
        return
    if rule + ":" not in output:
        failures += 1
        REPORT.fail("{}: a tree with {} failed, but not on that rule".format(
            rule, what))
        note(output.strip().splitlines()[0] if output.strip() else "(no output)")
        return
    note("{}: caught {}".format(rule, what))


def clean():
    """The copy as it stands has to pass, or every other case means nothing."""
    global checks, failures
    code, output = run(ROOT)
    checks += 1
    if code != 0:
        failures += 1
        REPORT.fail("the tree itself does not pass its own structure check")
        for line in output.splitlines():
            if line.startswith("[FAIL]"):
                note(line)
    else:
        note("the tree passes as it stands")


def caseGuards():
    temp, tree = copyTree()
    try:
        if edit(tree, "payload/vamap.h",
                lambda t: t.replace("#define US_VAMAP_H", "#define US_UEFI_VAMAP_H")):
            expect("guards", tree, "two headers sharing one guard")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def caseLayers():
    temp, tree = copyTree()
    try:
        if edit(tree, "core/cache.c",
                lambda t: t.replace('#include "core/cache.h"',
                                    '#include "core/cache.h"\n#include "uefi/session.h"')):
            expect("layers", tree, "a core file including a driver header")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def caseOwnership():
    temp, tree = copyTree()
    try:
        if edit(tree, "core/thunk.c",
                lambda t: t.replace("// us-build: both", "// us-build: driver")):
            expect("ownership", tree, "a core file claiming the wrong side")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def caseConfig():
    temp, tree = copyTree()
    try:
        if edit(tree, "config/us.toml",
                lambda t: t.replace("notVamap", "notVamapp", 1)):
            expect("config", tree, "a misspelled key in a shipped configuration")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def caseMilestones():
    temp, tree = copyTree()
    try:
        # A name a script greps for to its end, so that renaming it in the
        # table leaves the script waiting for a line that is never printed
        if edit(tree, "uefi/stage.c",
                lambda t: t.replace('"M6.5 armed"', '"M6.5 armed and ready"')):
            expect("milestones", tree, "a milestone the boot can no longer print")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def casePayloadAggregate():
    temp, tree = copyTree()
    try:
        if edit(tree, "payload/drvhook.c",
                lambda t: t.replace(
                    "UsRcpcScan scan = { 0 };\n                UsRcpcScanResult found;",
                    "UsRcpcScan scan = { .visit = rewriteSite };\n"
                    "                UsRcpcScanResult found;")):
            expect("payload", tree, "a structure of function addresses built as "
                   "one aggregate, which the copy would leave pointing at the "
                   "link time address")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def caseTree():
    temp, tree = copyTree()
    try:
        # A path the listing names and the tree does not have: the listing is
        # asked about a file that is there, so take it away
        (tree / "core/pool.c").unlink()
        expect("tree", tree, "a file the design document lists and the tree lost")
    finally:
        shutil.rmtree(temp, ignore_errors=True)


def main():
    clean()
    caseGuards()
    caseLayers()
    caseOwnership()
    caseConfig()
    caseMilestones()
    casePayloadAggregate()
    caseTree()
    return REPORT.summary(checks, failures)


if __name__ == "__main__":
    sys.exit(main())
