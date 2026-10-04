#!/usr/bin/env python3
"""Execute the entry's instructions with distinct register values.

The generated stack lookup is tested separately. Start at its ready label and
check each SPSR branch, both SP banks, the nesting test that tells a fault of
the payload's own from an exception of the interrupted code's, every possible
emulation destination, and both ways out: eret back to the interrupted code,
or a branch to the handler the slot originally held.
"""

from pathlib import Path
import re
import sys

MASK = (1 << 64) - 1

# Where the frame the fault path restores lives. It is arbitrary: what is
# checked is that the entry restores what the handler answered with.
FAULT_FRAME = 0x500000
LANDING = 0xB0000000 + 7


def layoutConstants():
    """The pool's own constants, which the entry's nesting test needs."""
    header = Path(__file__).resolve().parents[2] / "common/layout.h"
    text = header.read_text()
    out = {}
    for name in ("US_STACK_SIZE",):
        m = re.search(r"#define\s+%s\s+(0[xX][0-9a-fA-F]+|\d+)" % name, text)
        assert m, "missing " + name
        out[name] = int(m.group(1), 0)
    return out


def frameOffsets():
    header = Path(__file__).resolve().parents[2] / "payload/payload.h"
    text = re.sub(r"/\*.*?\*/", "", header.read_text(), flags=re.S)
    body = re.search(r"typedef struct UsFrame_t\s*\{(.*?)\}", text, re.S).group(1)
    offsets = {}
    size = 0
    for name, count in re.findall(r"uint64_t\s+(\w+)(?:\[(\d+)\])?\s*;", body):
        if count:
            assert name == "x"
            for i in range(int(count)):
                offsets["US_FRAME_X%d" % i] = size
                size += 8
        else:
            offsets["US_FRAME_" + name.upper()] = size
            size += 8
    offsets["US_FRAME_STRIDE"] = size
    offsets.update(layoutConstants())
    return offsets


def instructions(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    code, labels = [], {}
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith((".macro", ".if", ".else", ".endif")):
            raise AssertionError("expand conditional assembly before checking")
        if line.startswith("."):
            continue
        if line.endswith(":"):
            labels[line[:-1]] = len(code)
            continue
        op, _, operands = line.partition(" ")
        args = re.split(r",\s*(?![^\[]*\])", operands.strip())
        code.append((op, args))
    return code, labels


class Machine:
    """Just enough of the entry's instructions to run it."""

    def __init__(self, code, labels, offsets, mode, faulted):
        self.code = code
        self.labels = labels
        self.offsets = offsets
        self.original = {"x%d" % i: 0x10000000 + i * 0x100 for i in range(31)}
        self.system = {"sp_el0": 0x400000, "tpidr_el1": 0x120123,
                       "spsr_el1": 0xA00003C0 | mode, "elr_el1": 0x20000000,
                       "esr_el1": 0x02000000, "far_el1": 0x500000}
        self.entrySp = 0x300000
        self.stackTop = 0x800000
        self.faulted = faulted
        self.memory = {}
        self.equal = False
        self.below = False
        self.called = None
        self.ret = None
        self.landing = LANDING
        self.destination = None
        self.originalElr = self.system["elr_el1"]
        self.expectedElr = self.originalElr
        self.expectedSpsr = self.system["spsr_el1"]
        self.expectedSp = self.entrySp if mode == 5 else self.system["sp_el0"]

        # x18 is the per-CPU block in kernel mode and is rebuilt from
        # TPIDR_EL1, so that is what the interrupted code is taken to have
        # had, and what the restore has to put back.
        self.original["x18"] = self.system["tpidr_el1"] & ~0xFFF
        self.expectedGpr = dict(self.original)
        self.regs = dict(self.original, sp=self.entrySp, x18=self.stackTop)
        if faulted:
            # The payload's own stack: the nesting test is what sends the
            # entry here.
            self.regs["sp"] = self.stackTop - 0x40
        self.pc = self.labels["usSyncFaultInside"] if faulted else self.labels["usSyncStackReady"]

    def value(self, operand):
        if operand.startswith("#"):
            operand = operand[1:]
            return self.offsets[operand] if operand in self.offsets else int(operand, 0)
        return 0 if operand == "xzr" else self.regs[operand]

    def clobber(self):
        """What the payload is allowed to destroy: the scratch registers."""
        for i in list(range(19)) + [30]:
            self.regs["x%d" % i] = 0xDEAD0000 + i

    def enterHandler(self, name, ret, faultFrame):
        assert self.called is None, "the handler is entered twice"
        self.called = name
        self.ret = ret
        if name == "usPayloadHandle":
            frame = self.regs["x0"]
            assert frame == self.regs["sp"] == self.regs["x19"], "handler frame pointer differs"
            for i in range(31):
                assert self.memory[frame + self.offsets["US_FRAME_X%d" % i]] == self.original["x%d" % i], \
                    "incorrect save of x%d" % i
            assert self.memory[frame + self.offsets["US_FRAME_SP"]] == self.expectedSp, \
                "wrong interrupted SP bank"
            assert self.memory[frame + self.offsets["US_FRAME_STRIDE"]] == self.entrySp, \
                "wrong saved entry SP"
            for field in ("ELR", "SPSR", "ESR", "FAR"):
                assert self.memory[frame + self.offsets["US_FRAME_" + field]] == self.system[field.lower() + "_el1"], \
                    "incorrect " + field
            if ret == 1 and self.destination is not None:
                replacement = 0xB0000000 + self.destination
                self.memory[frame + self.offsets["US_FRAME_X%d" % self.destination]] = replacement
                self.expectedGpr["x%d" % self.destination] = replacement
            # An emulated instruction is resumed after itself; a frame handed
            # back is resumed at the instruction that faulted.
            self.expectedElr = self.originalElr + (4 if ret == 1 else 0)
            if ret == 1:
                self.memory[frame + self.offsets["US_FRAME_ELR"]] = self.expectedElr
            if ret == 2:
                # The payload answers 2 by naming where to hand the frame on.
                self.memory[frame + self.offsets["US_FRAME_LANDING"]] = self.landing
            self.clobber()
            self.regs["x0"] = ret
        else:
            assert name == "usPayloadFault"
            frame = faultFrame
            for i in range(31):
                self.memory[frame + self.offsets["US_FRAME_X%d" % i]] = self.expectedGpr["x%d" % i]
            self.memory[frame + self.offsets["US_FRAME_ELR"]] = self.expectedElr
            self.memory[frame + self.offsets["US_FRAME_SPSR"]] = self.expectedSpsr
            self.memory[frame + self.offsets["US_FRAME_STRIDE"]] = self.entrySp
            self.memory[frame + self.offsets["US_FRAME_LANDING"]] = self.landing
            self.clobber()
            self.regs["x0"] = frame
        self.system["elr_el1"], self.system["spsr_el1"] = 0, 0

    def step(self, ret, faultFrame):
        op, args = self.code[self.pc]
        self.pc += 1
        if op in ("str", "stp", "ldr", "ldp"):
            m = re.fullmatch(r"\[(\w+)(?:,\s*(#[^\]]+))?\](!?)", args[-1])
            assert m, "unsupported memory operand: %s" % args[-1]
            base, offset, update = m.groups()
            address = self.regs[base] + (self.value(offset) if offset else 0)
            if update:
                self.regs[base] = address
            for i, register in enumerate(args[:-1]):
                if op in ("str", "stp"):
                    self.memory[address + i * 8] = self.value(register)
                else:
                    self.regs[register] = self.memory[address + i * 8]
        elif op in ("add", "sub", "and"):
            left, right = self.value(args[1]), self.value(args[2])
            result = left + right if op == "add" else left - right if op == "sub" else left & right
            self.regs[args[0]] = result & MASK
        elif op == "mov":
            self.regs[args[0]] = self.value(args[1])
        elif op == "mrs":
            self.regs[args[0]] = self.system[args[1]]
        elif op == "msr":
            if args[0] != "daifset":
                self.system[args[0]] = self.value(args[1])
        elif op == "cmp":
            lhs, rhs = self.value(args[0]), self.value(args[1])
            self.equal = lhs == rhs
            self.below = lhs < rhs
        elif op == "b":
            self.pc = self.labels[args[0]]
        elif op == "b.eq":
            if self.equal:
                self.pc = self.labels[args[0]]
        elif op == "b.lo":
            if self.below:
                self.pc = self.labels[args[0]]
        elif op in ("cbz", "cbnz"):
            if (self.value(args[0]) == 0) == (op == "cbz"):
                self.pc = self.labels[args[1]]
        elif op == "bl":
            self.enterHandler(args[0], ret, faultFrame)
        elif op == "wfi":
            return "halt"
        elif op == "eret":
            return "eret"
        elif op == "br":
            return "br"
        else:
            raise AssertionError("unsupported entry instruction: " + op)
        return None

    def checkRestored(self):
        for register, expected in self.expectedGpr.items():
            assert self.regs[register] == expected, "incorrect restore of " + register
        assert self.regs["sp"] == self.entrySp, "entry SP was not restored"
        assert self.system["sp_el0"] == 0x400000, "SP_EL0 was changed"
        assert self.system["elr_el1"] == self.expectedElr, "ELR was not restored"
        assert self.system["spsr_el1"] == self.expectedSpsr, "SPSR was not restored"


def roundTrip(code, labels, offsets, mode, destination, ret=1, faulted=False):
    m = Machine(code, labels, offsets, mode, faulted)
    m.destination = destination
    for _ in range(512):
        outcome = m.step(ret, FAULT_FRAME)
        if outcome is None:
            continue
        if outcome == "halt":
            assert m.called is not None, "halted before any handler ran"
            assert not faulted and ret == 0, "unexpected halt"
            assert code[m.pc] == ("b", ["usSyncNoForward"]), "halt does not loop quietly"
            return
        if outcome == "eret":
            assert m.called == "usPayloadHandle" and ret == 1, "eret on a frame that was not resumed"
            m.checkRestored()
            return
        if outcome == "br":
            assert m.called is not None, "branched before any handler ran"
            assert m.code[m.pc - 1] == ("br", ["x18"]), "the hand-back branches through x18"
            assert m.regs["x18"] == m.landing, "the branch does not go where the payload said"
            assert faulted or ret == 2, "a frame was branched with that was not handed on"
            for register, expected in m.expectedGpr.items():
                if register != "x18":
                    assert m.regs[register] == expected, "incorrect restore of " + register
            assert m.regs["sp"] == m.entrySp, "entry SP was not restored"
            assert m.system["elr_el1"] == m.expectedElr, "ELR was not restored"
            assert m.system["spsr_el1"] == m.expectedSpsr, "SPSR was not restored"
            return
    raise AssertionError("entry did not return or halt")


def check(text):
    code, labels = instructions(text)
    offsets = frameOffsets()
    assert labels["usSyncEntry"] == labels["usSyncEntrySp0"], "entries do not share the checked path"
    assert code[labels["usSyncEntry"]:labels["usSyncStackReady"]] == [
        ("msr", ["daifset", "#0xF"]), ("b", ["usStackLookup"])], "entry spends GPRs before the lookup"
    assert code[labels["usSyncNoStack"]:labels["usSyncNoStack"] + 2] == [
        ("wfi", [""]), ("b", ["usSyncNoStack"])], "unknown CPU does not halt quietly"
    # Only the two vectors that are wired: EL0 is a separate piece of work,
    # and its x18 is user state, which this entry does not keep yet.
    for mode in (4, 5):
        for destination in [None] + list(range(31)):
            roundTrip(code, labels, offsets, mode, destination)
        for ret in (0, 1, 2):
            roundTrip(code, labels, offsets, mode, None, ret=ret)
        # The path taken when the payload's own access faulted.
        roundTrip(code, labels, offsets, mode, None, faulted=True)


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "payload/entry.S")
    try:
        check(path.read_text())
    except (AssertionError, KeyError, ValueError, IndexError) as exc:
        print("FAIL %s: %s" % (path, exc))
        return 1
    print("PASS %s: all 31 register slots, both SP paths, the fault entry, "
          "and both exits" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
