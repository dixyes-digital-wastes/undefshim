#!/usr/bin/env python3
"""Every hand-written encoding, checked against the assembler and a disassembler.

Some of this tree writes instructions as numbers: the slot stubs are built word
by word into memory that is already mapped, the stack lookup is generated, and
a few system register writes have no other spelling. A number is not an
instruction until something assembles it, and a comment beside it is not a
check - so each one is assembled from its own text and the two are compared,
and the text is handed to capstone to see that it decodes to the mnemonic the
comment claims.

The encoding is also looked for in the file it is said to live in. A constant
edited in the source and not here would otherwise pass: this check would still
be assembling the text it was told to, and nothing would be comparing it to
what the tree now says.

    python3 check_encodings.py [root]

An entry whose low bits are a displacement the encoder fills in carries a mask,
and only the fixed part is compared.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check as checklib

REPORT = checklib.Check("check_encodings")

try:
    import capstone
except ImportError:
    sys.exit(REPORT.skip("capstone is not installed"))

# The RCpc loads need v8.3, PAN needs v8.1, and everything here is in v8.0.
MARCH = "armv8.3-a"

# Imm19 of a conditional branch, which the encoder computes from a distance
IMM19 = 0x7FFFF << 5

# (file, the constant as the tree writes it, the text that has to produce it,
#  the mask of the bits that are fixed, what it decodes as)
ENCODINGS = [
    ("core/pan.h", "0xd500419f", "msr pan, #1", 0xFFFFFFFF, "msr"),
    ("core/pan.h", "0xd500409f", "msr pan, #0", 0xFFFFFFFF, "msr"),
    ("payload/entry.S", "0xd500419f", "msr pan, #1", 0xFFFFFFFF, "msr"),

    ("core/thunk.c", "0xF81F0FF2U", "str x18, [sp, #-16]!", 0xFFFFFFFF, "str"),
    ("core/thunk.c", "0xD5385212U", "mrs x18, esr_el1", 0xFFFFFFFF, "mrs"),
    ("core/thunk.c", "0xD35AFE52U", "lsr x18, x18, #26", 0xFFFFFFFF, "lsr"),
    ("core/thunk.c", "0xF84107F2U", "ldr x18, [sp], #16", 0xFFFFFFFF, "ldr"),
    ("core/thunk.c", "0xD538D092U", "mrs x18, tpidr_el1", 0xFFFFFFFF, "mrs"),
    ("core/thunk.c", "0x9274CE52U", "and x18, x18, #0xfffffffffffff000",
     0xFFFFFFFF, "and"),
    ("core/thunk.c", "0xF85F03F2U", "ldur x18, [sp, #-16]", 0xFFFFFFFF, "ldur"),
    ("core/thunk.c", "0xD61F0000U", "br x0", 0xFFFFFFFF, "br"),
    ("core/thunk.c", "0xB5000012U", "cbnz x18, .", 0xFFFFFFFF & ~IMM19, "cbnz"),

    ("core/thunk.h", "0x14000000U", "b .", 0xFFFFFFFF, "b"),
    ("core/thunk.h", "0xD503201FU", "nop", 0xFFFFFFFF, "nop"),

    ("core/stackgen.c", "0xD53800B2U", "mrs x18, mpidr_el1", 0xFFFFFFFF, "mrs"),
    ("core/stackgen.c", "0x9260DE52U", "and x18, x18, #0xffffffff00ffffff",
     0xFFFFFFFF, "and"),
    ("core/stackgen.c", "0x92409E52U", "and x18, x18, #0xffffffffff",
     0xFFFFFFFF, "and"),
    ("core/stackgen.c", "0xB5000072U", "cbnz x18, .", 0xFFFFFFFF & ~IMM19, "cbnz"),

    # The scan patterns are the same words with the size field left out, so the
    # text is the widest form of each family and the comparison is masked
    ("core/ldapr.c", "0x38BFC000U", "ldaprb w0, [x0]", 0xFFFFFFFF, "ldaprb"),
    ("core/ldapr.c", "0x08DFFC00U", "ldarb w0, [x0]", 0xFFFFFFFF, "ldarb"),

    ("tests/demo/lrcpc_demo.c", "0xB8BFC000u", "ldapr w0, [x0]", 0xFFFFFFFF,
     "ldapr"),
]

checks = 0
failures = 0


def check(name, cond):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        REPORT.fail(name)


def assemble(text):
    """The one instruction, as the assembler writes it."""
    with tempfile.TemporaryDirectory() as where:
        source = Path(where) / "one.s"
        source.write_text("    %s\n" % text)
        obj = Path(where) / "one.o"
        done = subprocess.run(
            ["clang", "--target=aarch64-none-elf", "-march=" + MARCH,
             "-c", str(source), "-o", str(obj)],
            capture_output=True, text=True)
        if done.returncode != 0:
            raise AssertionError("the assembler refused %r: %s"
                                 % (text, done.stderr.strip().splitlines()[0]))
        dump = subprocess.run(["llvm-objdump", "-d", str(obj)],
                              capture_output=True, text=True)
    for line in dump.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and re.fullmatch(r"[0-9a-f]+:", fields[0]):
            return int(fields[1], 16)
    raise AssertionError("no instruction came out of %r" % text)


cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)


def decodes(word):
    found = list(cs.disasm(word.to_bytes(4, "little"), 0))
    if len(found) != 1:
        return None
    return found[0].mnemonic


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "../..")
    sources = {}

    for where, constant, text, mask, mnemonic in ENCODINGS:
        where = str(where)
        want = int(constant.rstrip("Uu"), 16)

        # The tree still has to say what this was told to check
        if where not in sources:
            try:
                sources[where] = (root / where).read_text()
            except OSError as why:
                check("%s: %s" % (where, why), False)
                sources[where] = ""
        check("%s does not carry %s any more" % (where, constant),
              constant in sources[where])

        try:
            got = assemble(text)
        except AssertionError as why:
            check("%s: %s" % (where, why), False)
            continue

        check("%s: %s wants 0x%08x, the assembler makes 0x%08x"
              % (where, text, want, got), (got & mask) == (want & mask))

        name = decodes(want)
        check("%s: 0x%08x is a %s, the comment says %s"
              % (where, want, name, mnemonic), name == mnemonic)

    return REPORT.summary(checks, failures)


if __name__ == "__main__":
    sys.exit(main())
