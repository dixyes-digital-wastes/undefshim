/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding things inside an image
 *
 * Every locator returns the number of matches it saw and, when asked, the
 * first one. Counts matter as much as offsets: a locator that finds two
 * candidates has found nothing trustworthy, and the callers reject anything
 * other than exactly one. Having the count available means that failure is
 * reported rather than silently resolved to whichever came first
 *
 * All offsets and RVAs are relative to the image, never to a file, so the
 * same locator works on disk and in memory
 */

#ifndef US_SCAN_H
#define US_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/pe.h"

/* A byte string where any byte may be marked as don't care */
typedef struct UsPattern_t {
    const uint8_t *bytes;
    const uint8_t *mask;  /* NULL means every byte must match */
    size_t         len;
} UsPattern;

typedef struct UsMatch_t {
    uint32_t rva;     /* RVA of the match */
    size_t   offset;  /* offset into the view the scan ran over */
} UsMatch;

#define US_SCAN_MAX_MATCHES 64

typedef struct UsMatchList_t {
    UsMatch matches[US_SCAN_MAX_MATCHES];
    size_t  count;    /* matches stored, capped at US_SCAN_MAX_MATCHES */
    size_t  total;    /* matches seen, which may be larger */
} UsMatchList;

/*
 * Searches one region. The caller supplies the bytes so that a scan can be
 * limited to an executable section, which is what keeps a pattern from
 * matching data
 */
void usScanRegion(const uint8_t *buf, size_t len, uint32_t rvaAt,
                  const UsPattern *pat, UsMatchList *out);

/* Searches every executable section of an image */
UsMatchList usScanImage(UsImage *img, const UsPattern *pat);

/*
 * Whether a pattern matches at one place. A locator that walks a section
 * itself, rather than through usScanImage, needs the test without the walk,
 * and one idea of what a match is rather than two
 */
bool patMatchAt(const uint8_t *at, const UsPattern *pat);

/* --- the signatures this project knows about ---------------------------- */

/*
 * Direct branches to the vector table slots. The slot patch rewrites these,
 * so they have to be found before the kernel starts, when the table is still
 * where the loader put it
 */
extern const UsPattern usPatMsrVBAREL1;

/*
 * The leaf that hands execution to the next stage. The same 36 byte body
 * appears once in bootmgfw and once in winload, where it is the last thing
 * that runs before the kernel, so patching its prologue gives a point at
 * which the kernel image is in memory and not yet running
 */
extern const UsPattern usPatTransferLeaf;

/* LDAPR and friends: the instructions the shim exists to emulate */
extern const UsPattern usPatLDAPRW;
extern const UsPattern usPatLDAPRX;
extern const UsPattern usPatLDAPRB;
extern const UsPattern usPatLDAPRH;

typedef struct UsLeafSite_t {
    bool     found;
    uint32_t rva;          /* start of the leaf */
    uint32_t patchRVA;     /* first sixteen bytes of it, the patch point */
    size_t   matches;      /* how many the locator saw; only 1 is usable */
} UsLeafSite;

/*
 * Locates the page table handoff in winload
 *
 * The handoff is recognised structurally rather than by a byte pattern: the
 * block that loads TTBR0, TTBR1 and the two translation control registers
 * back to back is the one that switches address spaces, and the registers it
 * uses differ between versions, so naming them would not survive. Matching
 * the shape finds the block in every version tried, including ones whose
 * code was restructured and no longer carries the byte sequence a byte
 * pattern would need
 *
 * The shape alone matches more than one place, so it is narrowed in two
 * steps, most specific first:
 *
 *   - the linker gives the real handoff its own section, .trans, so a
 *     candidate there wins
 *   - failing that, the real handoff is the one that goes on to write
 *     SCTLR_EL1, which is what actually enables the MMU
 *
 * Both steps are properties of the code, not of a version, which is the point
 */
typedef enum UsHandoffForm_e {
    UsHandoffNone = 0,
    /* Found in the .trans section */
    UsHandoffInTransSection,
    /* Found by the SCTLR_EL1 write that follows it */
    UsHandoffBeforeSCTLR,
} UsHandoffForm;

typedef struct UsHandoffSite_t {
    bool          found;
    UsHandoffForm form;
    uint32_t      rva;          /* the msr TTBR0_EL1 */
    size_t        candidates;   /* how many the shape matched before narrowing */
} UsHandoffSite;

/*
 * A hole in a mapped, executable section, large enough to hold something
 *
 * This is a run of zero words, not a run of branches to themselves with zeros
 * after. Both shapes look like padding, and the second is what a vector table
 * entry looks like when it is unused, so a locator that matched the shape
 * would pick out the exception vector table, which is not free space but the
 * firmware's own handlers
 *
 * A run has to be longer than a vector entry to be considered, which is what
 * separates the two: an entry's padding cannot exceed the entry
 *
 * Zero words are `udf #0`, and the section is mapped executable, so what is
 * written here runs. Whether anything refers to the run is a judgement about
 * the image rather than something that can be read off it, and the longest
 * run is preferred for that reason
 *
 * Note what this is not: a section with no raw data at all is only an entry
 * in the section table and is not mapped, however large it declares itself to
 * be. Only runs inside sections that carry data can be used
 */
typedef struct UsSpareSlot_t {
    bool     found;
    uint32_t rva;      /* where the run starts */
    uint32_t bytes;    /* how much room it offers */
    size_t   matches;  /* how many runs were long enough */
    /* The section it was found in, for the log */
    char        section[9];
} UsSpareSlot;

/*
 * MmPteBase, the base of the kernel's mapping of its own page descriptors
 *
 * The value is the kernel's to choose and lives in a variable, so the only
 * thing that survives a rebuild is the function that reads it. That function
 * is four instructions, and its literal is the base, which makes the variable
 * findable without naming any address (core/scan.c has the shape, and why
 * each constraint in it is there)
 *
 * `found` is false whenever the question is not answered: no body, more than
 * one body, a literal that is not a base, or more than one variable holding
 * it. The counts are reported either way, because which of those happened is
 * what says whether a build is unrecognised or the search is wrong
 */
typedef struct UsPteBaseSite_t {
    bool     found;
    uint32_t rva;        /* the variable holding the base */
    uint32_t helperRVA;  /* the body it was derived from */
    uint64_t base;       /* the literal the body carries */
    size_t   helpers;    /* bodies seen; only one is usable */
    size_t   candidates; /* loads that named the literal */
} UsPteBaseSite;

/*
 * The tables an image actually installs, found by following the register that
 * each `msr vbar_el1, xN` writes
 *
 * Shape alone does not answer this. An image can carry more than one table,
 * and the one in use need not be the one that looks most like a table: the
 * table winload installs has its interrupt entries written out in full rather
 * than branched to, so a search for sixteen slots that begin with a branch
 * finds a different table and reports it with no hint that it made a choice.
 * Following the register needs no such assumption
 *
 * Three sources are followed:
 *
 *   adrp + add     the address is a constant in the image, resolved here
 *   ldr            the value is a runtime one, so only its existence is known
 *
 * A site whose register is written from memory cannot be resolved without
 * running, and is counted rather than guessed at. For the tables that matter
 * the first form is what appears, so the count being nonzero is information
 * rather than a failure
 */
#define US_VBAR_MAX_TABLES 8

typedef struct UsVBARTables_t {
    uint32_t rvas[US_VBAR_MAX_TABLES];
    uint32_t syncWord[US_VBAR_MAX_TABLES];
    bool     syncUsable[US_VBAR_MAX_TABLES];
    size_t   count;       /* distinct tables resolved, in the order found */
    size_t   sites;       /* msr vbar_el1 sites seen */
    size_t   unresolved;  /* sites whose register came from memory */
    bool     overflow;    /* more distinct tables than this can hold */
} UsVBARTables;

/*
 * The slots that can matter, as the architecture orders them
 *
 * The offsets are the ones a running exception selects between: a synchronous
 * exception taken at EL1 with SP_EL1 uses the second group, and that is the
 * shape of every exception this project exists to handle
 *
 * The asynchronous three of the second group are taken over as well. They are
 * not the exceptions that carry an RCpc load, but they are the ones that reach
 * the kernel's own "an exception on the exception stack" handler, which is a
 * bugcheck with no way to see afterwards what arrived; taking them over is
 * what makes that answerable
 */
typedef enum UsVectorSlot_e {
    UsVectorSlotEL1tSync = 0,  /* +0x000 */
    UsVectorSlotEL1hSync = 4,  /* +0x200 */
    UsVectorSlotEL1hIrq = 5,   /* +0x280 */
    UsVectorSlotEL1hFiq = 6,   /* +0x300 */
    UsVectorSlotEL1hSError = 7, /* +0x380 */
    UsVectorSlotEL0Sync32 = 8, /* +0x400, the kernel's lower EL entry */
} UsVectorSlot;

/* Counts of the instructions the shim has to emulate, per image */
typedef struct UsLDAPRCounts_t {
    size_t word;
    size_t xword;
    size_t byte;
    size_t half;
    size_t total;
} UsLDAPRCounts;

/* --- the sites a boot has to act on ------------------------------------- */

/*
 * A place in an image that the boot intends to do something about
 *
 * These are collected before anything is written, because the decision to act
 * is worth being able to look at on its own: a site that was found in the
 * wrong place, or not found at all, is visible here rather than as a fault
 * later. LDAPR instructions are not sites: there are thousands of them, they
 * are handled when they fault rather than at boot, and enumerating them would
 * bury everything else
 */
typedef enum UsSiteKind_e {
    /* msr vbar_el1, xN. Where a vector table is installed, and therefore the
     * point at which an exception can be taken over */
    UsSiteVBARWrite = 0,
    /*
     * The leaf that hands control to the next stage. It is the last thing that
     * runs before the kernel, which makes it the one moment the kernel image
     * is in memory and not yet running
     */
    UsSiteTransferLeaf,
    /* Where the final page tables are loaded. A mapping added before this is
     * gone after it */
    UsSiteTTBRHandoff,
} UsSiteKind;

typedef struct UsSite_t {
    UsSiteKind  kind;
    UsImageKind image;
    uint32_t    rva;
    /*
     * Meaning depends on the kind. For a vector table write it is the Rt of
     * the store, that is which register holds the address of the table
     */
    uint32_t    auxiliary;
} UsSite;

/*
 * Enough for the sites that are expected: the vector table writes are the
 * numerous ones and there are a few dozen of them, not thousands. A list that
 * overflows reports it rather than dropping entries silently
 */
#define US_SITE_MAX 128

typedef struct UsSiteList_t {
    UsSite sites[US_SITE_MAX];
    size_t count;   /* stored, capped at US_SITE_MAX */
    size_t total;   /* seen, which may be larger */
} UsSiteList;

/*
 * Where the kernel loads a driver
 *
 * This is the one moment a driver can be covered without handling its
 * exceptions: the file has been read, the sections are built and mapped, the
 * import table is resolved, and none of its code has run. It is the same
 * situation the kernel itself is in when the boot rewrites it, which is why
 * the same replacement can be done here
 *
 * Nothing about it is at a fixed address, so it is found by name. The anchor
 * is the exported MmLoadSystemImage, a wrapper whose body is one handover to
 * the worker that does the work - a call on some builds and a tail branch on
 * others, and the two mean the same thing here. The worker's body is a
 * sequence of calls, and its identity is that the image exports the name one
 * entry of the sequence goes to
 *
 * That entry is the whole of the search. Every other entry is an address that
 * means nothing on its own - a helper, another wrapper, a function whose name
 * is not exported - so one entry with a name attached is what turns a list of
 * addresses into a shape, and it is checked rather than assumed
 *
 * The position of that entry belongs to the build and not to the shape, so two
 * builds that agree on it say nothing about a third. The search below checks
 * the position it finds rather than assuming it, and refuses a build whose
 * sequence does not match at that position instead of arming the wrong call,
 * which is the failure it exists to prevent
 */
#define US_DRIVER_LOAD_ANCHOR "MmLoadSystemImage"
#define US_DRIVER_LOAD_WITNESS "RtlImageNtHeader"
/*
 * Which call of the worker's body is the driver load, as a position in the
 * sequence. The position belongs to the build and not to the shape: it is 19
 * on the builds the locator serves, and a build that puts it elsewhere is
 * refused for that reason alone. The refusal does not separate such a build
 * from one whose body is not a wrapper at all
 */
#define US_DRIVER_LOAD_CALL_INDEX 19U

typedef struct UsDriverLoadHook_t {
    bool     found;
    uint32_t anchorRVA;    /* the exported anchor */
    uint32_t anchorStart;  /* the function the anchor's name resolves into */
    uint32_t functionRVA;  /* the worker the anchor calls */
    uint32_t functionEnd;  /* where the next function begins */
    uint32_t callRVA;      /* the call to take over */
    uint32_t witnessRVA;   /* where that call goes */
    uint32_t callIndex;    /* which call of the sequence callRVA is */
    size_t   calls;        /* how many calls the worker's body carries */
} UsDriverLoadHook;

/*
 * Fields other than found are what was learned before the refusal, so a boot
 * that could not be armed can say how far it got. Only a found answer may be
 * used
 */

/*
 * The two headers below declare the functions that work on these types. They
 * are included here because this is the header the tree includes: which of the
 * three files a declaration is in is the reader's business, not the caller's
 */
#include "core/locate.h"
#include "core/site.h"

#endif
