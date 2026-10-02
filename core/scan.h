/*
 * Finding things inside an image.
 *
 * Every locator returns the number of matches it saw and, when asked, the
 * first one. Counts matter as much as offsets: a locator that finds two
 * candidates has found nothing trustworthy, and the callers reject anything
 * other than exactly one. Having the count available means that failure is
 * reported rather than silently resolved to whichever came first.
 *
 * All offsets and RVAs are relative to the image, never to a file, so the
 * same locator works on disk and in memory.
 */

#ifndef US_SCAN_H
#define US_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/pe.h"

/* A byte string where any byte may be marked as don't care. */
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
 * matching data.
 */
void usScanRegion(const uint8_t *buf, size_t len, uint32_t rvaAt,
                  const UsPattern *pat, UsMatchList *out);

/* Searches every executable section of an image. */
UsMatchList usScanImage(UsImage *img, const UsPattern *pat);

/* Searching only the bytes, with no image: for classifying a blob. */
bool usScanContains(const uint8_t *buf, size_t len, const UsPattern *pat);

/* --- the signatures this project knows about ---------------------------- */

/*
 * Direct branches to the vector table slots. The slot patch rewrites these,
 * so they have to be found before the kernel starts, when the table is still
 * where the loader put it.
 */
extern const UsPattern usPatMsrVbarEl1;

/*
 * The leaf that hands execution to the next stage. The same 36 byte body
 * appears once in bootmgfw and once in winload, where it is the last thing
 * that runs before the kernel, so patching its prologue gives a point at
 * which the kernel image is in memory and not yet running.
 */
extern const UsPattern usPatTransferLeaf;

/*
 * The sequence in winload that loads the final page tables and turns the MMU
 * on. This is the handoff to the kernel, and the point at which a mapping we
 * added earlier would otherwise be lost.
 *
 * It does not survive every version: the 26h1 image restructured this code
 * and the sequence is gone. Locators are therefore tried in order and the
 * caller is told which one answered.
 */
extern const UsPattern usPatTtbrHandoff;

/* LDAPR and friends: the instructions the shim exists to emulate. */
extern const UsPattern usPatLdaprW;
extern const UsPattern usPatLdaprX;
extern const UsPattern usPatLdaprB;
extern const UsPattern usPatLdaprH;

/* The load option string that only winload carries, in UTF-16. */
extern const UsPattern usPatOsloaderXsl;

typedef struct UsLeafSite_t {
    bool     found;
    uint32_t rva;          /* start of the leaf */
    uint32_t patchRva;     /* first sixteen bytes of it, the patch point */
    size_t   matches;      /* how many the locator saw; only 1 is usable */
    /*
     * True when the exception directory confirms the match is a function
     * start. A pattern that is not at a boundary is a coincidence, and
     * hooking it would corrupt whatever function it sits inside.
     */
    bool     isFunctionStart;
} UsLeafSite;

/*
 * Locates the transfer leaf. Reports the count so the caller can refuse a
 * result that is not unique, and whether the exception directory agrees it is
 * a function entry.
 */
UsLeafSite usLocateTransferLeaf(UsImage *img);

/*
 * Locates the page table handoff in winload.
 *
 * The handoff is recognised structurally rather than by a byte pattern: the
 * block that loads TTBR0, TTBR1 and the two translation control registers
 * back to back is the one that switches address spaces, and the registers it
 * uses differ between versions, so naming them would not survive. Matching
 * the shape finds the block in every version examined, including 26h1, whose
 * code was restructured and which no longer carries the byte sequence the
 * earlier analysis described.
 *
 * The shape alone matches more than one place, so it is narrowed in two
 * steps, most specific first:
 *
 *   - the linker gives the real handoff its own section, .trans, so a
 *     candidate there wins
 *   - failing that, the real handoff is the one that goes on to write
 *     SCTLR_EL1, which is what actually enables the MMU
 *
 * Both steps are properties of the code, not of a version, which is the point.
 */
typedef enum UsHandoffForm_e {
    UsHandoffNone = 0,
    /* Found in the .trans section. */
    UsHandoffInTransSection,
    /* Found by the SCTLR_EL1 write that follows it. */
    UsHandoffBeforeSctlr,
} UsHandoffForm;

typedef struct UsHandoffSite_t {
    bool          found;
    UsHandoffForm form;
    uint32_t      rva;          /* the msr TTBR0_EL1 */
    size_t        candidates;   /* how many the shape matched before narrowing */
} UsHandoffSite;

UsHandoffSite usLocateTtbrHandoff(UsImage *img);

/*
 * A hole in the loader's code, large enough to hold something.
 *
 * This is a run of zero words, not a run of branches to themselves with zeros
 * after. Both shapes look like padding, and the second is what a vector table
 * entry looks like when it is unused: an earlier version matched that shape
 * and picked out the exception vector table, which is not free space but the
 * firmware's own handlers.
 *
 * A run has to be longer than a vector entry to be considered, which is what
 * separates the two: an entry's padding cannot exceed the entry.
 *
 * The bytes are in an executable section, so they run; whether anything
 * refers to them is a judgement about the image rather than something that
 * can be read off it, and the longest run is preferred for that reason.
 */
typedef struct UsSpareSlot_t {
    bool     found;
    uint32_t rva;      /* where the run starts */
    uint32_t bytes;    /* how much room it offers */
    size_t   matches;  /* how many runs were long enough */
} UsSpareSlot;

UsSpareSlot usLocateSpareSlot(UsImage *img, uint32_t minBytes);

/* Counts of the instructions the shim has to emulate, per image. */
typedef struct UsLdaprCounts_t {
    size_t word;
    size_t xword;
    size_t byte;
    size_t half;
    size_t total;
} UsLdaprCounts;

UsLdaprCounts usCountLdapr(UsImage *img);

/* --- the sites a boot has to act on ------------------------------------- */

/*
 * A place in an image that the boot intends to do something about.
 *
 * These are collected before anything is written, because the decision to act
 * is worth being able to look at on its own: a site that was found in the
 * wrong place, or not found at all, is visible here rather than as a fault
 * later. LDAPR instructions are not sites: there are thousands of them, they
 * are handled when they fault rather than at boot, and enumerating them would
 * bury everything else.
 */
typedef enum UsSiteKind_e {
    /* msr vbar_el1, xN. Where a vector table is installed, and therefore the
     * point at which an exception can be taken over. */
    UsSiteVbarWrite = 0,
    /*
     * The leaf that hands control to the next stage. It is the last thing that
     * runs before the kernel, which makes it the one moment the kernel image
     * is in memory and not yet running.
     */
    UsSiteTransferLeaf,
    /* Where the final page tables are loaded. A mapping added before this is
     * gone after it. */
    UsSiteTtbrHandoff,
} UsSiteKind;

typedef struct UsSite_t {
    UsSiteKind  kind;
    UsImageKind image;
    uint32_t    rva;
    /*
     * Meaning depends on the kind. For a vector table write it is the Rt of
     * the store, that is which register holds the address of the table.
     */
    uint32_t    auxiliary;
} UsSite;

/*
 * Enough for the sites that are expected: the vector table writes are the
 * numerous ones and there are a few dozen of them, not thousands. A list that
 * overflows reports it rather than dropping entries silently.
 */
#define US_SITE_MAX 128

typedef struct UsSiteList_t {
    UsSite sites[US_SITE_MAX];
    size_t count;   /* stored, capped at US_SITE_MAX */
    size_t total;   /* seen, which may be larger */
} UsSiteList;

void usSiteListInit(UsSiteList *list);

/* Appends one site. Returns false when the list is full, in which case the
 * count of seen sites still goes up. */
bool usSiteAdd(UsSiteList *list, UsSiteKind kind, UsImageKind image, uint32_t rva,
               uint32_t auxiliary);

/*
 * Sorts by image and then by RVA, so that the same image always produces the
 * same order. The order sites are found in depends on the section table, and
 * comparing two dumps is the whole point of having them.
 */
void usSiteListSort(UsSiteList *list);

const char *usSiteKindName(UsSiteKind kind);

/*
 * Collects every site in one image. Locators that are not unique are not
 * reported as sites: a match that could be one of several places is not a
 * place to act on, and the count of what was seen is what says so.
 */
size_t usCollectSites(UsSiteList *list, UsImage *img, UsImageKind kind);

#endif
