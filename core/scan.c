/*
 * Finding things inside an image, see scan.h for the contract.
 */

#include "core/scan.h"

/* ---------------------------------------------------------------------------
 * The signatures.
 *
 * All of these are instruction encodings rather than text, so they are
 * written as the instruction word and laid out little endian by the macro.
 * Writing them as loose bytes invites getting the order wrong, which is not
 * the kind of mistake a scan will tell you about: it just finds nothing.
 *
 * None of them is allowed to be a heuristic. A locator that can match twice
 * is a locator that can pick the wrong place, so the callers insist on
 * exactly one.
 * ------------------------------------------------------------------------- */

#define US_LE32(w)                          \
    (uint8_t)((w) & 0xFF),                  \
    (uint8_t)(((w) >> 8) & 0xFF),           \
    (uint8_t)(((w) >> 16) & 0xFF),          \
    (uint8_t)(((w) >> 24) & 0xFF)

/* msr VBAR_EL1, xN. N is free, hence the wildcard in the low byte. */
static const uint8_t kMsrVbarEl1[] = { US_LE32(0xD518C000) };
static const uint8_t kMsrVbarEl1Mask[] = { US_LE32(0xFFFFFFE0) };

const UsPattern usPatMsrVbarEl1 = {
    .bytes = kMsrVbarEl1,
    .mask = kMsrVbarEl1Mask,
    .len = 4,
};

/*
 * stp x29,x30,[sp,#-0x10]! ; mov x29,sp ; mov sp,x2 ; mov x2,x0
 * mov x0,x1 ; blr x2 ; mov sp,x29 ; ldp x29,x30,[sp],#0x10 ; ret
 *
 * The first sixteen bytes are the patch site: they are all prologue, so
 * they can be replaced by a branch to a trampoline without cutting an
 * instruction in half.
 */
/*
 * The transfer to the kernel.
 *
 * This is the sequence that switches SP to the kernel's own stack and then
 * branches to the kernel's entry point. It is the point at which everything
 * before it is finished and nothing of the kernel has run yet, which is the
 * only moment the kernel image can be edited without disturbing it.
 *
 * The signature was derived from the register state at the kernel entry
 * rather than from a description of the function: the link register there
 * still held the address after the call that precedes the branch, which
 * identified the instruction. An earlier signature, taken from an analysis
 * note, matched a different function entirely -- one this path does not use,
 * which is why patching it changed nothing.
 *
 *   mov sp, x2          the kernel's stack, loaded two instructions earlier
 *   mov x19, x0         whatever the kernel entry wants
 *   mov x20, x1         the kernel entry point
 *   bl  <anywhere>      one more call before handing over
 *   mov x0, x19
 *   br  x20             the handover itself
 *
 * The call in the middle has a different target in every version, so it is
 * masked out. The rest is fixed, and the last instruction is what carries the
 * meaning: a branch through a register that was loaded with the entry point.
 */
static const uint8_t kTransferLeaf[] = {
    US_LE32(0x9100005F), US_LE32(0xAA0003F3), US_LE32(0xAA0103F4), US_LE32(0x94000000),
    US_LE32(0xAA1303E0), US_LE32(0xD61F0280),
};
static const uint8_t kTransferLeafMask[] = {
    US_LE32(0xFFFFFFFF), US_LE32(0xFFFFFFFF), US_LE32(0xFFFFFFFF), US_LE32(0xFC000000),
    US_LE32(0xFFFFFFFF), US_LE32(0xFFFFFFFF),
};

const UsPattern usPatTransferLeaf = {
    .bytes = kTransferLeaf,
    .mask = kTransferLeafMask,
    .len = sizeof(kTransferLeaf),
};

/*
 * The sequence that loads the final page tables and turns the MMU on, as it
 * appears in versions up to 26100. It is kept for reference and for the fast
 * path, but the locator no longer depends on it: 26h1 restructured this code
 * and the byte sequence is gone, so the shape is what gets matched.
 */
static const uint8_t kTtbrHandoffOld[] = {
    US_LE32(0xD5182001), US_LE32(0xD5033FDF),
    US_LE32(0xD5182022), US_LE32(0xD5033FDF),
    US_LE32(0xD5182043), US_LE32(0xD5033FDF),
    US_LE32(0xD518A204), US_LE32(0xD5033FDF),
    US_LE32(0xD508871F),
};

const UsPattern usPatTtbrHandoff = {
    .bytes = kTtbrHandoffOld,
    .mask = NULL,
    .len = sizeof(kTtbrHandoffOld),
};

/*
 * The LDAPR family. The size and the opcode live in the top two bytes and are
 * fixed; the two register fields are free, which the mask carries.
 */
static const uint8_t kLdaprMask[] = { US_LE32(0xFFFFFC00) };

static const uint8_t kLdaprWBytes[] = { US_LE32(0xB8BFC000) };
static const uint8_t kLdaprXBytes[] = { US_LE32(0xF8BFC000) };
static const uint8_t kLdaprBBytes[] = { US_LE32(0x38BFC000) };
static const uint8_t kLdaprHBytes[] = { US_LE32(0x78BFC000) };

const UsPattern usPatLdaprW = { .bytes = kLdaprWBytes, .mask = kLdaprMask, .len = 4 };
const UsPattern usPatLdaprX = { .bytes = kLdaprXBytes, .mask = kLdaprMask, .len = 4 };
const UsPattern usPatLdaprB = { .bytes = kLdaprBBytes, .mask = kLdaprMask, .len = 4 };
const UsPattern usPatLdaprH = { .bytes = kLdaprHBytes, .mask = kLdaprMask, .len = 4 };

/* "OSLOADER.XSL" as UTF-16LE. */
static const uint8_t kOsloaderXsl[] = {
    'O', 0, 'S', 0, 'L', 0, 'O', 0, 'A', 0, 'D', 0, 'E', 0, 'R', 0,
    '.', 0, 'X', 0, 'S', 0, 'L', 0,
};

const UsPattern usPatOsloaderXsl = {
    .bytes = kOsloaderXsl,
    .mask = NULL,
    .len = sizeof(kOsloaderXsl),
};

/* ------------------------------------------------------------------------ */

static bool patMatchAt(const uint8_t *at, const UsPattern *pat) {
    for (size_t i = 0; i < pat->len; i++) {
        uint8_t mask = pat->mask != NULL ? pat->mask[i] : 0xFFU;
        if ((at[i] & mask) != (pat->bytes[i] & mask)) {
            return false;
        }
    }
    return true;
}

void usScanRegion(const uint8_t *buf, size_t len, uint32_t rvaAt,
                  const UsPattern *pat, UsMatchList *out) {
    out->count = 0;
    out->total = 0;

    if (buf == NULL || pat == NULL || pat->len == 0 || len < pat->len) {
        return;
    }

    size_t limit = len - pat->len;
    for (size_t i = 0; i <= limit; i++) {
        if (!patMatchAt(buf + i, pat)) {
            continue;
        }
        out->total++;
        if (out->count < US_SCAN_MAX_MATCHES) {
            out->matches[out->count].offset = i;
            out->matches[out->count].rva = rvaAt + (uint32_t)i;
            out->count++;
        }
    }
}

bool usScanContains(const uint8_t *buf, size_t len, const UsPattern *pat) {
    if (buf == NULL || pat == NULL || pat->len == 0 || len < pat->len) {
        return false;
    }
    size_t limit = len - pat->len;
    for (size_t i = 0; i <= limit; i++) {
        if (patMatchAt(buf + i, pat)) {
            return true;
        }
    }
    return false;
}

UsMatchList usScanImage(UsImage *img, const UsPattern *pat) {
    UsMatchList all = { 0 };

    if (img == NULL || !img->valid) {
        return all;
    }

    /*
     * Only executable sections. A pattern that also matches data is not a
     * signature, and restricting the search is what turns a byte string into
     * one: LDAPR encodings occur in jump tables and literals, and instructions
     * that load a page table register occur in dead code that the linker kept.
     */
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPeSection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0) {
            continue;
        }
        if (s->virtualSize == 0) {
            continue;
        }
        p = usImageRvaSpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }

        UsMatchList one;
        usScanRegion(p, avail, s->virtualAddress, pat, &one);
        all.total += one.total;
        for (size_t k = 0; k < one.count && all.count < US_SCAN_MAX_MATCHES; k++) {
            all.matches[all.count++] = one.matches[k];
        }
    }

    return all;
}

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
     * does with the kernel's entry point before using it.
     */
    site.patchRva = m.matches[0].rva + (uint32_t)(sizeof(kTransferLeaf) - sizeof(uint32_t));
    /*
     * This is a branch inside a function, not a function entry, so the
     * exception directory has nothing to say about it. The check that used to
     * be made here was for the signature that matched a different function.
     */
    site.isFunctionStart = false;
    return site;
}

/* --- the page table handoff -------------------------------------------- */

/*
 * Reads the instruction word at an RVA, or 0 when it is not readable. Zero is
 * not a valid instruction we care about, so it doubles as "no instruction
 * here" without a separate status.
 */
static uint32_t readInsn(const UsImage *img, uint32_t rva) {
    const uint8_t *p = usImageRvaToPtr(img, rva);

    if (p == NULL) {
        return 0;
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* msr <sysreg>, xN. The register is free, so the low five bits are masked. */
#define US_MSR_MASK 0xFFFFFFE0U
#define US_MSR_VAL(word) ((word) & US_MSR_MASK)

#define US_IS_ISB(w) ((w) == 0xD5033FDFU)

#define US_SYSREG_TTBR0_EL1 0xD5182000U
#define US_SYSREG_TTBR1_EL1 0xD5182020U
#define US_SYSREG_TCR_EL1 0xD5182040U
#define US_SYSREG_MAIR_EL1 0xD518A200U
#define US_SYSREG_SCTLR_EL1 0xD5181000U

/*
 * Whether the handoff block starts here.
 *
 * The block loads the two translation base registers and the two translation
 * control registers one after another. How far apart they sit depends on
 * whether the value comes straight from a register or through a load, so the
 * spacing is not fixed, but the order and the small window are, and that is
 * what is matched. The registers themselves are not part of the test: that is
 * exactly what changed in 26h1.
 */
static bool isHandoffAt(const UsImage *img, uint32_t rva) {
    uint32_t next;
    bool sawMair = false;
    bool sawTcr = false;

    if (US_MSR_VAL(readInsn(img, rva)) != US_MSR_VAL(US_SYSREG_TTBR0_EL1)) {
        return false;
    }
    if (!US_IS_ISB(readInsn(img, rva + 4))) {
        return false;
    }

    /* TTBR1 follows, either immediately or after one load. */
    if (US_MSR_VAL(readInsn(img, rva + 8)) == US_MSR_VAL(US_SYSREG_TTBR1_EL1)) {
        next = rva + 12;
    } else if (US_MSR_VAL(readInsn(img, rva + 12)) == US_MSR_VAL(US_SYSREG_TTBR1_EL1)) {
        next = rva + 16;
    } else {
        return false;
    }

    /* Then the two control registers, in either order, within a short run of
     * isb and load instructions. */
    for (int step = 0; step < 8; step++, next += 4) {
        uint32_t w = readInsn(img, next);
        uint32_t v = US_MSR_VAL(w);

        if (US_IS_ISB(w)) {
            continue;
        }
        if (v == US_MSR_VAL(US_SYSREG_MAIR_EL1)) {
            sawMair = true;
        } else if (v == US_MSR_VAL(US_SYSREG_TCR_EL1)) {
            sawTcr = true;
        }
    }
    return sawMair && sawTcr;
}

/* Does an SCTLR_EL1 write follow within the given distance? That write is what
 * actually enables the MMU, so it marks the real handoff. */
static bool writesSctlrSoon(const UsImage *img, uint32_t rva, uint32_t within) {
    for (uint32_t off = 0; off < within; off += 4) {
        if (US_MSR_VAL(readInsn(img, rva + off)) == US_MSR_VAL(US_SYSREG_SCTLR_EL1)) {
            return true;
        }
    }
    return false;
}

UsHandoffSite usLocateTtbrHandoff(UsImage *img) {
    UsHandoffSite site = { 0 };
    UsHandoffSite inTrans = { 0 };
    UsHandoffSite beforeSctlr = { 0 };

    if (img == NULL || !img->valid) {
        return site;
    }

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPeSection *s = &img->sections[i];
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
             * there is the one we want. */
            if (s->nameLen == 6 && s->name[0] == '.' && s->name[1] == 't'
                && s->name[2] == 'r' && s->name[3] == 'a' && s->name[4] == 'n'
                && s->name[5] == 's') {
                inTrans.form = UsHandoffInTransSection;
                inTrans.rva = rva;
                inTrans.found = true;
                inTrans.candidates = site.candidates;
                break;
            }
            if (!beforeSctlr.found && writesSctlrSoon(img, rva, 0x90)) {
                beforeSctlr.form = UsHandoffBeforeSctlr;
                beforeSctlr.rva = rva;
                beforeSctlr.found = true;
            }
        }
        if (inTrans.found) {
            break;
        }
    }

    if (inTrans.found) {
        return inTrans;
    }
    beforeSctlr.candidates = site.candidates;
    return beforeSctlr;
}

/* --- the spare slot ----------------------------------------------------- */

/*
 * A hole in the loader's code: a run of zero words long enough to hold
 * something.
 *
 * The distinguishing feature has to be the length, not the shape. A vector
 * table entry is also a branch to itself followed by zeros, and the padding
 * there is bounded by the entry size, so a run that is longer than one entry
 * cannot be one. An earlier version matched the branch-and-zeros shape and
 * picked out the exception vector table, which is not space at all: writing
 * there replaces handlers the firmware still needs.
 *
 * A hole is also taken to be executable, since it is inside a code section,
 * and unreferenced, which is a judgement about the image rather than
 * something that can be checked from its bytes.
 */
#define US_HOLE_MIN_SLOP 256U
/* A hole is a hole, not a search for the longest run in the image. */
#define US_HOLE_MAX_BYTES 65536U

UsSpareSlot usLocateSpareSlot(UsImage *img, uint32_t minBytes) {
    UsSpareSlot best = { 0 };

    if (img == NULL || !img->valid || minBytes < 4) {
        return best;
    }

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPeSection *s = &img->sections[i];
        uint32_t span;
        uint32_t off;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0) {
            continue;
        }
        span = s->virtualSize;

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
                 * padding and less likely to be a buffer something fills. */
                if (!best.found || bytes > best.bytes) {
                    best.found = true;
                    best.rva = s->virtualAddress + off;
                    best.bytes = bytes;
                }
            }
            off += bytes != 0 ? bytes : 4;
        }
    }

    return best;
}

UsLdaprCounts usCountLdapr(UsImage *img) {    UsLdaprCounts c = { 0 };

    c.word = usScanImage(img, &usPatLdaprW).total;
    c.xword = usScanImage(img, &usPatLdaprX).total;
    c.byte = usScanImage(img, &usPatLdaprB).total;
    c.half = usScanImage(img, &usPatLdaprH).total;
    c.total = c.word + c.xword + c.byte + c.half;
    return c;
}

/* --- collecting the sites ------------------------------------------------ */

void usSiteListInit(UsSiteList *list) {
    list->count = 0;
    list->total = 0;
}

bool usSiteAdd(UsSiteList *list, UsSiteKind kind, UsImageKind image, uint32_t rva,
               uint32_t auxiliary) {
    list->total++;
    if (list->count >= US_SITE_MAX) {
        return false;
    }
    list->sites[list->count].kind = kind;
    list->sites[list->count].image = image;
    list->sites[list->count].rva = rva;
    list->sites[list->count].auxiliary = auxiliary;
    list->count++;
    return true;
}

const char *usSiteKindName(UsSiteKind kind) {
    switch (kind) {
    case UsSiteVbarWrite:
        return "vbar-write";
    case UsSiteTransferLeaf:
        return "transfer-leaf";
    case UsSiteTtbrHandoff:
        return "ttbr-handoff";
    }
    return "unknown";
}

/*
 * The vector table writes, found by walking the executable sections directly
 * rather than through usScanImage. That helper keeps at most sixty four
 * matches, which is a sensible bound for a locator that has to be unique but
 * the wrong one here: these are collected precisely because there are many of
 * them, and a list that stopped at the first sixty four would be a list that
 * silently missed some.
 */
static size_t collectVbarWrites(UsSiteList *list, UsImage *img, UsImageKind kind) {
    size_t before = list->total;

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPeSection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;
        size_t offset = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0) {
            continue;
        }
        p = usImageRvaSpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }

        while (offset + 4 <= avail) {
            if (patMatchAt(p + offset, &usPatMsrVbarEl1)) {
                uint32_t word = (uint32_t)p[offset] | ((uint32_t)p[offset + 1] << 8)
                                | ((uint32_t)p[offset + 2] << 16)
                                | ((uint32_t)p[offset + 3] << 24);
                uint32_t rva = s->virtualAddress + (uint32_t)offset;

                /* The low five bits are the register holding the address. */
                usSiteAdd(list, UsSiteVbarWrite, kind, rva, word & 0x1FU);
            }
            offset += 4;
        }
    }

    return list->total - before;
}

size_t usCollectSites(UsSiteList *list, UsImage *img, UsImageKind kind) {
    size_t before = list->total;

    if (img == NULL || !img->valid) {
        return 0;
    }

    collectVbarWrites(list, img, kind);

    /*
     * The other two only exist in the loader: the leaf is what hands control
     * to the kernel, and the handoff is what rebuilds the address space for
     * it. Neither appears in the kernel itself.
     */
    if (kind == UsImageWinload) {
        UsLeafSite leaf = usLocateTransferLeaf(img);
        UsHandoffSite handoff = usLocateTtbrHandoff(img);

        if (leaf.found) {
            usSiteAdd(list, UsSiteTransferLeaf, kind, leaf.patchRva, 0);
        }
        if (handoff.found) {
            usSiteAdd(list, UsSiteTtbrHandoff, kind, handoff.rva, (uint32_t)handoff.form);
        }
    }

    return list->total - before;
}

static int compareSites(const void *a, const void *b) {
    const UsSite *x = a;
    const UsSite *y = b;

    if (x->image != y->image) {
        return (int)x->image - (int)y->image;
    }
    if (x->rva != y->rva) {
        return x->rva < y->rva ? -1 : (x->rva > y->rva ? 1 : 0);
    }
    return (int)x->kind - (int)y->kind;
}

/*
 * Insertion sort, because the list is short and this avoids pulling in qsort,
 * which is a libcall the payload cannot make. The driver could, but the same
 * code is meant to be usable from either side of the handoff.
 */
void usSiteListSort(UsSiteList *list) {
    for (size_t i = 1; i < list->count; i++) {
        UsSite key = list->sites[i];
        size_t j = i;

        while (j > 0 && compareSites(&list->sites[j - 1], &key) > 0) {
            list->sites[j] = list->sites[j - 1];
            j--;
        }
        list->sites[j] = key;
    }
}
