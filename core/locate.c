/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The locators, see locate.h for the contract
 */

// us-build: driver

#include <string.h>
#include "core/locate.h"
#include "core/thunk.h"

UsLeafSite usLocateTransferLeaf(UsImage *img) {
    UsLeafSite site = { 0 };
    UsMatchList m = usScanImage(img, &usPatTransferLeaf);

    site.matches = m.total;
    if (m.total != 1 || m.count != 1) {
        return site;
    }

    site.found = true;
    site.rva = m.matches[0].rva;
    /*
     * The branch is the last instruction of the sequence, and it is what has
     * to be replaced: the four bytes before it are the last thing the loader
     * does with the kernel's entry point before using it
     */
    site.patchRVA = m.matches[0].rva + (uint32_t)(usPatTransferLeaf.len - sizeof(uint32_t));
    return site;
}

/* --- the page table handoff -------------------------------------------- */

/*
 * Reads the instruction word at an RVA, or 0 when it is not readable. Zero is
 * not a valid instruction we care about, so it doubles as "no instruction
 * here" without a separate status
 */
static uint32_t readInsn(const UsImage *img, uint32_t rva) {
    const uint8_t *p = usImageRVAToPtr(img, rva);

    if (p == NULL) {
        return 0;
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* msr <sysreg>, xN. The register is free, so the low five bits are masked */
#define US_MSR_MASK 0xFFFFFFE0U
#define US_MSR_VAL(word) ((word) & US_MSR_MASK)

#define US_IS_ISB(w) ((w) == 0xD5033FDFU)

#define US_SYSREG_TTBR0_EL1 0xD5182000U
#define US_SYSREG_TTBR1_EL1 0xD5182020U
#define US_SYSREG_TCR_EL1 0xD5182040U
#define US_SYSREG_MAIR_EL1 0xD518A200U
#define US_SYSREG_SCTLR_EL1 0xD5181000U

/*
 * Whether the handoff block starts here
 *
 * The block loads the two translation base registers and the two translation
 * control registers one after another. How far apart they sit depends on
 * whether the value comes straight from a register or through a load, so the
 * spacing is not fixed, but the order and the small window are, and that is
 * what is matched. The registers themselves are not part of the test: that is
 * exactly what changed between versions
 */
static bool isHandoffAt(const UsImage *img, uint32_t rva) {
    uint32_t next;
    bool sawMAIR = false;
    bool sawTCR = false;

    if (US_MSR_VAL(readInsn(img, rva)) != US_MSR_VAL(US_SYSREG_TTBR0_EL1)) {
        return false;
    }
    if (!US_IS_ISB(readInsn(img, rva + 4))) {
        return false;
    }

    /* TTBR1 follows, either immediately or after one load */
    if (US_MSR_VAL(readInsn(img, rva + 8)) == US_MSR_VAL(US_SYSREG_TTBR1_EL1)) {
        next = rva + 12;
    } else if (US_MSR_VAL(readInsn(img, rva + 12)) == US_MSR_VAL(US_SYSREG_TTBR1_EL1)) {
        next = rva + 16;
    } else {
        return false;
    }

    /* Then the two control registers, in either order, within a short run of
     * isb and load instructions */
    for (int step = 0; step < 8; step++, next += 4) {
        uint32_t w = readInsn(img, next);
        uint32_t v = US_MSR_VAL(w);

        if (US_IS_ISB(w)) {
            continue;
        }
        if (v == US_MSR_VAL(US_SYSREG_MAIR_EL1)) {
            sawMAIR = true;
        } else if (v == US_MSR_VAL(US_SYSREG_TCR_EL1)) {
            sawTCR = true;
        }
    }
    return sawMAIR && sawTCR;
}

/* Does an SCTLR_EL1 write follow within the given distance? That write is what
 * actually enables the MMU, so it marks the real handoff */
static bool writesSCTLRSoon(const UsImage *img, uint32_t rva, uint32_t within) {
    for (uint32_t off = 0; off < within; off += 4) {
        if (US_MSR_VAL(readInsn(img, rva + off)) == US_MSR_VAL(US_SYSREG_SCTLR_EL1)) {
            return true;
        }
    }
    return false;
}

UsHandoffSite usLocateTTBRHandoff(UsImage *img) {
    UsHandoffSite site = { 0 };
    UsHandoffSite inTrans = { 0 };
    UsHandoffSite beforeSCTLR = { 0 };

    if (img == NULL || !img->valid) {
        return site;
    }

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        uint32_t end;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0) {
            continue;
        }
        end = s->virtualAddress + s->virtualSize;
        for (uint32_t rva = s->virtualAddress; rva + 32 <= end; rva += 4) {
            if (!isHandoffAt(img, rva)) {
                continue;
            }
            site.candidates++;

            /* The linker gave the real handoff its own section; a candidate
             * there is the one we want */
            if (s->nameLen == 6 && s->name[0] == '.' && s->name[1] == 't'
                && s->name[2] == 'r' && s->name[3] == 'a' && s->name[4] == 'n'
                && s->name[5] == 's') {
                inTrans.form = UsHandoffInTransSection;
                inTrans.rva = rva;
                inTrans.found = true;
                inTrans.candidates = site.candidates;
                break;
            }
            if (!beforeSCTLR.found && writesSCTLRSoon(img, rva, 0x90)) {
                beforeSCTLR.form = UsHandoffBeforeSCTLR;
                beforeSCTLR.rva = rva;
                beforeSCTLR.found = true;
            }
        }
        if (inTrans.found) {
            break;
        }
    }

    if (inTrans.found) {
        return inTrans;
    }
    beforeSCTLR.candidates = site.candidates;
    return beforeSCTLR;
}

/* --- the spare slot ----------------------------------------------------- */

/*
 * A hole in the loader's code: a run of zero words long enough to hold
 * something
 *
 * The distinguishing feature has to be the length, not the shape. A vector
 * table entry is also a branch to itself followed by zeros, and the padding
 * there is bounded by the entry size, so a run that is longer than one entry
 * cannot be one. Matching the branch-and-zeros shape would pick out the
 * exception vector table, which is not space at all: writing there replaces
 * handlers the firmware still needs
 *
 * A hole is also taken to be executable, since it is inside a code section,
 * and unreferenced, which is a judgement about the image rather than
 * something that can be checked from its bytes
 *
 * In particular, a section must carry data. One that declares only a virtual
 * size is an entry in the section table and nothing else -- the loader does
 * not map it, so there is no code there to overwrite and none to run, and
 * reading it produces zeros like any other unwritten range
 */
#define US_HOLE_MIN_SLOP 256U
/* A hole is a hole, not a search for the longest run in the image */
#define US_HOLE_MAX_BYTES 65536U

UsSpareSlot usLocateSpareSlot(UsImage *img, uint32_t minBytes) {
    UsSpareSlot best = { 0 };

    if (img == NULL || !img->valid || minBytes < 4) {
        return best;
    }

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        uint32_t span;
        uint32_t off;


        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0) {
            continue;
        }
        if (s->rawSize == 0) {
            continue;
        }
        /*
         * Only the part the file carries. Past the raw size the section is
         * zeroes until something writes it, and the thing that writes it is
         * the running kernel: a stub put there is the kernel's own data by the
         * time the first exception arrives
         */
        /*
         * Not the pageable family and not the discardable one. Their pages are
         * not all there when this runs: what is read as zeroes is a page that
         * has not been read in yet, and the kernel fills it from the file the
         * moment it touches it. A stub written there is gone by the time the
         * first exception arrives, and the slot that branches to it lands in
         * whatever the file held. KSCP is one of those: it is where the kernel
         * keeps its own patched code
         */
        if (strncmp(s->name, "PAGE", 4U) == 0 || strncmp(s->name, "INIT", 4U) == 0
            || strncmp(s->name, "KSCP", 4U) == 0) {
            continue;
        }
        span = s->rawSize < s->virtualSize ? s->rawSize : s->virtualSize;

        for (off = 0; off + 4 <= span;) {
            uint32_t bytes = 0;

            if (readInsn(img, s->virtualAddress + off) != 0) {
                off += 4;
                continue;
            }
            while (off + bytes + 4 <= span && bytes < US_HOLE_MAX_BYTES
                   && readInsn(img, s->virtualAddress + off + bytes) == 0) {
                bytes += 4;
            }
            if (bytes >= minBytes && bytes >= US_HOLE_MIN_SLOP) {
                best.matches++;
                /* The longest hole wins: a longer run is more likely to be
                 * padding and less likely to be a buffer something fills */
                if (!best.found || bytes > best.bytes) {
                    best.found = true;
                    best.rva = s->virtualAddress + off;
                    best.bytes = bytes;
                    memcpy(best.section, s->name, sizeof(best.section));
                    best.section[sizeof(best.section) - 1U] = 0;
                }
            }
            off += bytes != 0 ? bytes : 4;
        }
    }

    return best;
}

/* --- the kernel's driver load call ------------------------------------- */

/*
 * The target of a branch at an RVA, as an RVA
 *
 * link selects BL from B. The chain of calls depends on the distinction: a
 * function's tail branches outnumber its calls, and a sequence that counted
 * them would be a different sequence in every build. The handover out of the
 * anchor is the one hop where either will do, and its caller asks for both
 *
 * False when the word is not that branch, and false when the target leaves
 * the image: a branch out of the image is not one this can follow, and an
 * answer that wrapped around the address space would be worse than none
 */
static bool branchTargetRVA(const UsImage *img, uint32_t rva, bool link,
                            uint32_t *out) {
    uint32_t word = readInsn(img, rva);
    int64_t offset = 0;
    int64_t target;

    if (link ? !usDecodeBranchLink(word, &offset) : !usDecodeBranch(word, &offset)) {
        return false;
    }
    target = (int64_t)rva + offset;
    if (target < 0 || target >= (int64_t)img->sizeOfImage) {
        return false;
    }
    *out = (uint32_t)target;
    return true;
}

UsDriverLoadHook usLocateDriverLoadHook(UsImage *img) {
    UsDriverLoadHook hook = { 0 };
    uint32_t anchor = 0;
    uint32_t anchorEnd = 0;
    uint32_t witness = 0;
    uint32_t call19 = 0;
    size_t index = 0;
    size_t witnessAt = 0;
    size_t witnessCount = 0;
    bool have19 = false;

    if (img == NULL || !img->valid) {
        return hook;
    }
    /*
     * The anchor is a name. An exported function is a function entry, so the
     * table has to agree with the name, and a disagreement is a reason to
     * stop rather than something to work around
     */
    if (!usImageExportRVA(img, US_DRIVER_LOAD_ANCHOR, &anchor)) {
        return hook;
    }
    hook.anchorRVA = anchor;
    /*
     * The wrapper's body runs from the exported address to whatever the table
     * says comes next. The anchor is not required to have an entry of its own:
     * some wrappers carry no frame of their own, so the table says nothing
     * about them and their body would otherwise read as part of the function
     * before them. Where the export points is the fact, and the scan starts
     * there
     */
    if (!usImageFunctionRange(img, anchor, &hook.anchorStart, &anchorEnd)) {
        return hook;
    }
    /*
     * Its body is the handover to the worker, and the handover is a call on
     * some builds and a tail branch on others. Both are followed, because
     * the two say the same thing here: the wrapper's own body is over and
     * the worker runs in its place. Which form a build uses is a property
     * of that build and not of the shape
     *
     * What is not followed is a branch back into the anchor, which is how the
     * wrapper reaches its own return, and a target that is not a function in
     * its own right. That second test is what keeps this honest: a call into
     * the middle of another function is not the entry the worker has
     */
    for (uint32_t at = anchor; at + 4 <= anchorEnd; at += 4) {
        uint32_t target = 0;
        uint32_t start = 0;
        uint32_t end = 0;

        if (!branchTargetRVA(img, at, true, &target)
            && !branchTargetRVA(img, at, false, &target)) {
            continue;
        }
        if (target == anchor) {
            continue;
        }
        /* The worker is a function in its own right, entered at its start */
        if (!usImageFunctionRange(img, target, &start, &end) || start != target) {
            continue;
        }
        hook.functionRVA = target;
        hook.functionEnd = end;
        break;
    }
    if (hook.functionRVA == 0) {
        return hook;
    }

    /*
     * The sequence of calls the worker makes, in address order, and the one
     * answerable question about it: the image exports the name that one entry
     * of it goes to
     *
     * This is the only link in the chain that carries its own truth. Every
     * other entry is an address that means nothing on its own, so the entry
     * that has to be the witness is what fixes the meaning of all of them,
     * and it is checked rather than assumed
     */
    if (!usImageExportRVA(img, US_DRIVER_LOAD_WITNESS, &witness)) {
        return hook;
    }
    hook.witnessRVA = witness;
    for (uint32_t at = hook.functionRVA; at + 4 <= hook.functionEnd; at += 4) {
        uint32_t target = 0;

        if (!branchTargetRVA(img, at, true, &target)) {
            continue;
        }
        if (index == US_DRIVER_LOAD_CALL_INDEX) {
            call19 = at;
            have19 = true;
        }
        if (target == witness) {
            witnessAt = index;
            witnessCount++;
        }
        index++;
    }
    hook.calls = index;
    if (!have19 || witnessCount != 1 || witnessAt != US_DRIVER_LOAD_CALL_INDEX) {
        return hook;
    }
    hook.callRVA = call19;
    hook.callIndex = US_DRIVER_LOAD_CALL_INDEX;
    hook.found = true;
    return hook;
}

/*
 * A synchronous slot, and the layout of the table it lives in: sixteen slots
 * of 0x80 bytes, fixed by the architecture
 */
#define US_VECTOR_SLOT_BYTES 0x80U
#define US_VECTOR_SLOTS 16U

/*
 * Following the register a vector table write puts in VBAR_EL1
 *
 * The forms that appear are short: an adrp and an add two or three
 * instructions ahead of the write, or a load from a structure. Looking back a
 * handful of instructions covers the first without pretending to cover a
 * compiler's whole repertoire, and anything not recognised is counted rather
 * than guessed at
 */
#define US_DEFUSE_LOOKBACK 8U

/* adrp: 1 at bit 31, opcode 10 at 30..29, so the fixed bits are 0x90000000 */
#define US_ADRP_MASK 0x9F000000U
#define US_ADRP_OPCODE 0x90000000U
/* add xD, xN, #imm12, 64 bit */
#define US_ADD_IMM_MASK 0xFF000000U
#define US_ADD_IMM_OPCODE 0x91000000U

#define US_RD(word) ((word) & 0x1FU)
#define US_RN(word) (((word) >> 5) & 0x1FU)
#define US_IMM12(word) (((word) >> 10) & 0xFFFU)

/*
 * The page an adrp refers to. The immediate is a signed 21 bit count of
 * pages, counted from the page the instruction itself is on
 */
static uint64_t adrpPage(uint32_t at, uint32_t word) {
    int64_t immlo = (int64_t)((word >> 29) & 3U);
    int64_t imm = (int64_t)((word >> 5) & 0x7FFFFU);

    imm = (imm << 2) | immlo;
    /* Sign extend from bit 20 without shifting into the sign bit */
    imm = (imm ^ 0x100000) - 0x100000;
    return (uint64_t)(int64_t)(at & ~0xFFFU) + ((uint64_t)imm << 12);
}

/* --- the descriptor base ----------------------------------------------- */

/*
 * MmPteBase, the base of the kernel's mapping of its own page descriptors
 *
 * payload/rewrite.c needs this to make a read-only text page writable where it
 * stands, and it cannot be computed: the kernel chooses the value, and it is a
 * variable rather than a link-time constant, which is why naming its RVA in a
 * configuration key has to be maintained by hand for every build. What is
 * stable across builds is the one function that reads it, and the shape the
 * compiler gives that function
 *
 * The kernel's translate functions carry it as four instructions:
 *
 *   ubfx xA, xB, #12, #48    the index of the descriptor inside its table
 *   ldr  xC, <literal>       the base, from a read-only pool in the same section
 *   add  xD, xC, xA, lsl #3  base plus index times eight, into the result
 *   ret
 *
 * Its literal is the base the kernel starts with. The variable lives in a
 * section that is read-only once the kernel has initialised, so this is what
 * it is initialised to rather than a copy of it, and the load that reads it
 * names it
 *
 * Both steps are shape and content, never an address: the first is a body of
 * four instructions, the second is a page-relative load whose target holds the
 * first step's literal. Neither needs a build to have been seen before
 *
 * Uniqueness is required of the second step, but not of the first one's uses:
 * more than one instruction loading the same variable is ordinary, since every
 * use site gets its own adrp and ldr. What has to be one is the number of
 * distinct variables the loads name. Two of those means the question is not
 * answered, and the caller is told so rather than handed whichever was found
 * first -- a wrong base is a rewrite that does not happen, or worse, a store
 * into a page that is not a descriptor table
 *
 * The other translate function, the one that answers for a page directory
 * entry, is this same body with the shift and the width changed. Its literal
 * differs, so a build that emitted it identically would be refusing rather
 * than answering
 */

/* ubfx x0, x0, #12, #36. The field is bits 12 up to 47, which is the whole
 * index of a descriptor in a 48 bit address space, and imms is the top bit of
 * the field rather than its width, so this reads #36 and not #48. The
 * registers are free and are compared as a chain below */
#define US_PTE_UBFX_FIXED 0xD34CBC00U
#define US_PTE_UBFX_MASK 0xFFFFFC00U
/* ldr x0, <literal>: opc 01, so the 64 bit form, with rt and imm19 free */
#define US_PTE_LDRLIT_FIXED 0x58000000U
#define US_PTE_LDRLIT_MASK 0xFF000000U
/* add x0, x0, x0, lsl #3: the shift type, the N bit and the flag bit are part
 * of the fixed word, the operands are not */
#define US_PTE_ADD_FIXED 0x8B000C00U
#define US_PTE_ADD_MASK 0xFFE0FC00U
/* ret */
#define US_PTE_RET_FIXED 0xD65F03C0U
#define US_PTE_RET_MASK 0xFFFFFFFFU
/* ldr x0, [x0]: the unsigned offset form, with rt, rn and imm12 free */
#define US_PTE_LDROFF_FIXED 0xF9400000U
#define US_PTE_LDROFF_MASK 0xFFC00000U

#define US_RM(word) (((word) >> 16) & 0x1FU)

/* The byte offset a literal load reaches, relative to the instruction */
static int64_t ldrLiteralOffset(uint32_t word) {
    int64_t imm = (int64_t)((word >> 5) & 0x7FFFFU);

    /* Sign extend from bit 18 without shifting into the sign bit */
    return (((imm ^ 0x40000) - 0x40000) * 4);
}

/* Eight bytes at an RVA, or zero when they are not all there. A base is never
 * zero, so zero doubles as "nothing readable here" */
static uint64_t readU64(const UsImage *img, uint32_t rva) {
    const uint8_t *p;
    size_t avail = 0;
    uint64_t v = 0;

    p = usImageRVASpan(img, rva, &avail);
    if (p == NULL || avail < 8U) {
        return 0;
    }
    for (size_t i = 0; i < 8U; i++) {
        v |= (uint64_t)p[i] << (i * 8U);
    }
    return v;
}

/* The literal a four-instruction translate body at an RVA carries, or zero
 * when the words there are not that body */
static uint64_t pteHelperLiteral(const UsImage *img, uint32_t rva) {
    uint32_t w[4];
    int64_t at;

    for (size_t i = 0; i < 4U; i++) {
        w[i] = readInsn(img, rva + (uint32_t)i * 4U);
    }
    if ((w[0] & US_PTE_UBFX_MASK) != US_PTE_UBFX_FIXED) {
        return 0;
    }
    if ((w[1] & US_PTE_LDRLIT_MASK) != US_PTE_LDRLIT_FIXED) {
        return 0;
    }
    if ((w[2] & US_PTE_ADD_MASK) != US_PTE_ADD_FIXED) {
        return 0;
    }
    if ((w[3] & US_PTE_RET_MASK) != US_PTE_RET_FIXED) {
        return 0;
    }
    /*
     * The four are a chain rather than four instructions that happen to sit
     * together: the add takes its base from what the load wrote and its index
     * from what the ubfx wrote, and it writes the register the return carries.
     * A body that is similar but not this one does not satisfy all three, and
     * requiring them is what keeps the match from being a coincidence
     */
    if (US_RN(w[2]) != US_RD(w[1]) || US_RM(w[2]) != US_RD(w[0])
        || US_RD(w[2]) != 0U) {
        return 0;
    }
    at = (int64_t)rva + 4 + ldrLiteralOffset(w[1]);
    if (at < 0 || (uint64_t)at + 8U > (uint64_t)img->sizeOfImage) {
        return 0;
    }
    return readU64(img, (uint32_t)at);
}

/* A kernel half address on a page boundary, which is the only thing the base
 * of a mapping can be. Both of the literals the x64 form of this function
 * carries are in the same range, so this only rules out a body that is not
 * one of them at all */
static bool isPlausibleBase(uint64_t v) {
    return (v & 0xFFFU) == 0 && (v >> 47) == 0x1FFFFU;
}

UsPteBaseSite usLocatePteBase(UsImage *img) {
    UsPteBaseSite site = { 0 };
    uint64_t base = 0;
    uint32_t target = 0;
    size_t distinct = 0;

    if (img == NULL || !img->valid) {
        return site;
    }

    /* Step one: the four-instruction body and the literal it carries */
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0
            || s->virtualSize == 0 || s->rawSize == 0) {
            continue;
        }
        p = usImageRVASpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }
        for (size_t off = 0; off + 16U <= avail; off += 4U) {
            uint32_t rva = s->virtualAddress + (uint32_t)off;
            uint64_t literal = pteHelperLiteral(img, rva);

            if (literal == 0) {
                continue;
            }
            site.helpers++;
            if (site.helpers == 1U) {
                base = literal;
                site.helperRVA = rva;
            }
        }
    }
    if (site.helpers != 1U || !isPlausibleBase(base)) {
        return site;
    }
    site.base = base;

    /* Step two: the loads that read that literal, and the variable they name */
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0
            || s->virtualSize == 0 || s->rawSize == 0) {
            continue;
        }
        p = usImageRVASpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }
        for (size_t off = 0; off + 8U <= avail; off += 4U) {
            uint32_t rva = s->virtualAddress + (uint32_t)off;
            uint32_t w0 = readInsn(img, rva);
            uint32_t w1 = readInsn(img, rva + 4U);
            uint32_t at;

            if ((w0 & US_ADRP_MASK) != US_ADRP_OPCODE
                || (w1 & US_PTE_LDROFF_MASK) != US_PTE_LDROFF_FIXED
                || US_RN(w1) != US_RD(w0)) {
                continue;
            }
            at = (uint32_t)adrpPage(rva, w0) + US_IMM12(w1) * 8U;
            if (readU64(img, at) != base) {
                continue;
            }
            site.candidates++;
            if (distinct == 0) {
                target = at;
                distinct = 1;
            } else if (target != at) {
                /* A second variable holding the same literal: which one the
                 * kernel uses is not something this can answer, so neither is
                 * the answer given */
                distinct = 2;
            }
        }
    }
    if (distinct != 1U) {
        site.rva = 0;
        return site;
    }

    site.found = true;
    site.rva = target;
    return site;
}

UsVBARTables usFindVBARTables(UsImage *img) {
    UsVBARTables out = { 0 };

    if (img == NULL || !img->valid) {
        return out;
    }

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;
        size_t offset = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0
            || s->rawSize == 0) {
            continue;
        }
        p = usImageRVASpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }

        while (offset + 4 <= avail) {
            uint32_t rva;
            uint32_t word;
            uint32_t reg;
            uint32_t tableRVA = 0;
            bool resolved = false;

            if (!patMatchAt(p + offset, &usPatMsrVBAREL1)) {
                offset += 4;
                continue;
            }

            rva = s->virtualAddress + (uint32_t)offset;
            word = readInsn(img, rva);
            reg = word & 0x1FU;
            out.sites++;

            for (uint32_t back = 1; back <= US_DEFUSE_LOOKBACK && back <= rva / 4; back++) {
                uint32_t at = rva - back * 4;
                uint32_t w = readInsn(img, at);

                if ((w & US_ADD_IMM_MASK) == US_ADD_IMM_OPCODE && US_RD(w) == reg
                    && US_RN(w) == reg) {
                    uint32_t prev = readInsn(img, at - 4);

                    if ((prev & US_ADRP_MASK) == US_ADRP_OPCODE && US_RD(prev) == reg) {
                        tableRVA = (uint32_t)adrpPage(at - 4, prev) + US_IMM12(w);
                        resolved = true;
                    }
                    /* The definition is this add either way; anything before
                     * it is a different value */
                    break;
                }
                if ((w & 0xFF000000U) == 0xF9000000U || (w & 0xFFC00000U) == 0xF9400000U
                    || (w & 0xFFC00000U) == 0xF8400000U) {
                    /* A load into this register: the address is a runtime
                     * value, so it is counted and not guessed at */
                    break;
                }
                if (((w & 0x9F000000U) == 0x10000000U || (w & 0x9F000000U) == 0x90000000U)
                    && US_RD(w) == reg) {
                    break;
                }
            }

            if (!resolved) {
                out.unresolved++;
                offset += 4;
                continue;
            }

            {
                bool seen = false;

                for (size_t k = 0; k < out.count; k++) {
                    if (out.rvas[k] == tableRVA) {
                        seen = true;
                    }
                }
                if (!seen) {
                    if (out.count < US_VBAR_MAX_TABLES) {
                        uint32_t slot = readInsn(img, tableRVA
                                                 + (uint32_t)UsVectorSlotEL1hSync
                                                       * US_VECTOR_SLOT_BYTES);

                        out.syncWord[out.count] = slot;
                        out.syncUsable[out.count] = usDecodeBranch(slot, NULL);
                        out.rvas[out.count++] = tableRVA;
                    } else {
                        out.overflow = true;
                    }
                }
            }
            offset += 4;
        }
    }

    return out;
}

UsLDAPRCounts usCountLDAPR(UsImage *img) {
    UsLDAPRCounts c = { 0 };

    c.word = usScanImage(img, &usPatLDAPRW).total;
    c.xword = usScanImage(img, &usPatLDAPRX).total;
    c.byte = usScanImage(img, &usPatLDAPRB).total;
    c.half = usScanImage(img, &usPatLDAPRH).total;
    c.total = c.word + c.xword + c.byte + c.half;
    return c;
}

bool usVectorSlotBranch(UsImage *img, uint32_t tableRVA, UsVectorSlot slot,
                        int32_t *displacement) {
    uint32_t word;
    int64_t offset;

    if (img == NULL || !img->valid || (uint32_t)slot >= US_VECTOR_SLOTS) {
        return false;
    }
    word = readInsn(img, tableRVA + (uint32_t)slot * US_VECTOR_SLOT_BYTES);
    if (!usDecodeBranch(word, &offset)) {
        return false;
    }
    if (displacement != NULL) {
        *displacement = (int32_t)(offset / 4);
    }
    return true;
}
