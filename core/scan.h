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

/* Counts of the instructions the shim has to emulate, per image. */
typedef struct UsLdaprCounts_t {
    size_t word;
    size_t xword;
    size_t byte;
    size_t half;
    size_t total;
} UsLdaprCounts;

UsLdaprCounts usCountLdapr(UsImage *img);

#endif
