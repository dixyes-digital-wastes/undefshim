#!/usr/bin/env python3
"""Execute the entry's save/restore instructions with distinct register values.

The generated stack lookup is tested separately. Start at its ready label and
check each SPSR branch, both SP banks, and every possible emulation destination.
"""

from pathlib import Path
import re
import sys

MASK = (1 << 64) - 1


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


def roundTrip(code, labels, offsets, mode, destination, claimed=True):
    original = {"x%d" % i: 0x10000000 + i * 0x100 for i in range(31)}
    system = {"sp_el0": 0x400000, "tpidr_el1": 0x120123,
              "spsr_el1": 0xA00003C0 | mode, "elr_el1": 0x20000000,
              "esr_el1": 0x02000000, "far_el1": 0x500000}
    entrySp, stackTop = 0x300000, 0x800000
    if mode == 4:
        original["x18"] = system["tpidr_el1"] & ~0xFFF
    regs = dict(original, sp=entrySp, x18=stackTop)
    memory = {}
    if mode in (0, 5):
        regs["sp"] -= 16
        memory[regs["sp"]] = original["x18"]
    expectedSp = entrySp if mode == 5 else system["sp_el0"]
    expectedElr = system["elr_el1"] + 4
    expectedSpsr = system["spsr_el1"]
    expectedGpr = dict(original)
    pc, equal, called = labels["usSyncStackReady"], False, False

    def value(operand):
        if operand.startswith("#"):
            operand = operand[1:]
            return offsets[operand] if operand in offsets else int(operand, 0)
        return 0 if operand == "xzr" else regs[operand]

    for _ in range(256):
        op, args = code[pc]
        pc += 1
        if op in ("str", "stp", "ldr", "ldp"):
            m = re.fullmatch(r"\[(\w+)(?:,\s*(#[^\]]+))?\](!?)", args[-1])
            assert m, "unsupported memory operand: %s" % args[-1]
            base, offset, update = m.groups()
            address = regs[base] + (value(offset) if offset else 0)
            if update:
                regs[base] = address
            for i, register in enumerate(args[:-1]):
                if op in ("str", "stp"):
                    memory[address + i * 8] = value(register)
                else:
                    regs[register] = memory[address + i * 8]
        elif op in ("add", "sub", "and"):
            left, right = value(args[1]), value(args[2])
            result = left + right if op == "add" else left - right if op == "sub" else left & right
            regs[args[0]] = result & MASK
        elif op == "mov":
            regs[args[0]] = value(args[1])
        elif op == "mrs":
            regs[args[0]] = system[args[1]]
        elif op == "msr":
            if args[0] != "daifset":
                system[args[0]] = value(args[1])
        elif op == "cmp":
            equal = value(args[0]) == value(args[1])
        elif op == "b":
            pc = labels[args[0]]
        elif op == "b.eq":
            if equal:
                pc = labels[args[0]]
        elif op in ("cbz", "cbnz"):
            if (value(args[0]) == 0) == (op == "cbz"):
                pc = labels[args[1]]
        elif op == "bl":
            assert args == ["usPayloadHandle"] and not called
            called = True
            frame = regs["x0"]
            assert frame == regs["sp"] == regs["x19"], "handler frame pointer differs"
            for i in range(31):
                assert memory[frame + offsets["US_FRAME_X%d" % i]] == original["x%d" % i], "incorrect save of x%d" % i
            assert memory[frame + offsets["US_FRAME_SP"]] == expectedSp, "wrong interrupted SP bank"
            assert memory[frame + offsets["US_FRAME_STRIDE"]] == entrySp, "wrong saved entry SP"
            for name in ("ELR", "SPSR", "ESR", "FAR"):
                assert memory[frame + offsets["US_FRAME_" + name]] == system[name.lower() + "_el1"], "incorrect " + name
            if destination is not None:
                replacement = 0xB0000000 + destination
                memory[frame + offsets["US_FRAME_X%d" % destination]] = replacement
                expectedGpr["x%d" % destination] = replacement
            memory[frame + offsets["US_FRAME_ELR"]] = expectedElr
            for i in list(range(19)) + [30]:
                regs["x%d" % i] = 0xDEAD0000 + i
            regs["x0"] = int(claimed)
            system["elr_el1"], system["spsr_el1"] = 0, 0
        elif op == "wfi":
            assert called and not claimed, "unexpected halt"
            assert code[pc] == ("b", ["usSyncNoForward"]), "halt does not loop quietly"
            return
        elif op == "eret":
            assert called and claimed
            for register, expected in expectedGpr.items():
                assert regs[register] == expected, "incorrect restore of " + register
            assert regs["sp"] == entrySp, "entry SP_EL1 was not restored"
            assert system["sp_el0"] == 0x400000, "SP_EL0 was changed"
            assert system["elr_el1"] == expectedElr, "ELR was not restored"
            assert system["spsr_el1"] == expectedSpsr, "SPSR was not restored"
            return
        else:
            raise AssertionError("unsupported entry instruction: " + op)
    raise AssertionError("entry did not return or halt")


def check(text):
    code, labels = instructions(text)
    offsets = frameOffsets()
    assert labels["usSyncEntry"] == labels["usSyncEntrySp0"], "entries do not share the checked path"
    assert code[labels["usSyncEntry"]:labels["usSyncStackReady"]] == [
        ("msr", ["daifset", "#0xF"]), ("b", ["usStackLookup"])], "entry spends GPRs before the lookup"
    assert code[labels["usSyncNoStack"]:labels["usSyncNoStack"] + 2] == [
        ("wfi", [""]), ("b", ["usSyncNoStack"])], "unknown CPU does not halt quietly"
    for mode in (0, 4, 5):
        for destination in [None] + list(range(31)):
            roundTrip(code, labels, offsets, mode, destination)
        roundTrip(code, labels, offsets, mode, None, claimed=False)


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "payload/entry.S")
    try:
        check(path.read_text())
    except (AssertionError, KeyError, ValueError, IndexError) as exc:
        print("FAIL %s: %s" % (path, exc))
        return 1
    print("PASS %s: all 31 register slots, three SP paths, and unclaimed halt" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
