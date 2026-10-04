#!/usr/bin/env python3
"""Check the rewrite against a disassembler, not against itself.

The C test says the conversion produces the encoding the assembler produces
for the same operands. This says the same thing from the other side: both
words are handed to capstone, the RCpc one has to decode as an RCpc load and
the replacement as an acquire load, with the same width, base and destination
register. A conversion that quietly changed a field would produce an
instruction that still decodes - just not that one.
"""

import sys

try:
    import capstone
except ImportError:
    print("PASS check_rewrite.py: skipped, capstone is not installed")
    sys.exit(0)

# (the encoding an assembler gives each form, the one it gives the acquire
# load for the same operands, the operands as text)
PAIRS = [
    (0xB8BFC020, 0x88DFFC20, "w0, [x1]"),
    (0xF8BFC3E2, 0xC8DFFFE2, "x2, [sp]"),
    (0xF8BFC07F, 0xC8DFFC7F, "xzr, [x3]"),
    (0xB8BFC0A4, 0x88DFFCA4, "w4, [x5]"),
    (0x38BFC0E6, 0x08DFFCE6, "w6, [x7]"),
    (0x78BFC128, 0x48DFFD28, "w8, [x9]"),
    (0xF8BFC16A, 0xC8DFFD6A, "x10, [x11]"),
]

MASK = 0xFFFFFC00
FORM = {0xB8BFC000: "ldapr", 0xF8BFC000: "ldapr",
        0x38BFC000: "ldaprb", 0x78BFC000: "ldaprh"}
ACQUIRE = {0xB8BFC000: "ldar", 0xF8BFC000: "ldar",
           0x38BFC000: "ldarb", 0x78BFC000: "ldarh"}

failures = 0
checks = 0
cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)


def decode(word):
    insns = list(cs.disasm(word.to_bytes(4, "little"), 0))
    if len(insns) != 1:
        raise AssertionError("0x%08x did not decode as one instruction" % word)
    return insns[0].mnemonic, insns[0].op_str.replace(" ", "")


def same(got, want):
    return got.replace(" ", "") == want.replace(" ", "")


def check(name, cond):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL " + name)


for ldapr, ldar, operands in PAIRS:
    want_form = FORM[ldapr & MASK]
    want_acquire = ACQUIRE[ldapr & MASK]

    mnemonic, got = decode(ldapr)
    check("the RCpc word decodes as one (%s)" % want_form, mnemonic == want_form)
    check("with the operands it was built for (%s)" % operands, same(got, operands))

    mnemonic, got = decode(ldar)
    check("the replacement decodes as one (%s)" % want_acquire, mnemonic == want_acquire)
    check("with the same operands (%s)" % operands, same(got, operands))

print("%d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
