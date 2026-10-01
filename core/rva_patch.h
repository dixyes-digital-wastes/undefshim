/*
 * Writing bytes into a loaded image.
 *
 * This is the mechanism behind the debug patch table: name an image, name an
 * offset into it, say what to write. It is deliberately the same code path
 * the shim will use later, so that a patch that works while bringing the
 * driver up keeps working once the real edits are switched on.
 *
 * Addresses are RVAs, never absolute, so a patch survives the loader putting
 * the image somewhere other than its preferred base, and so a table written
 * against one machine's addresses works on another.
 */

#ifndef US_RVA_PATCH_H
#define US_RVA_PATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/pe.h"

/* A byte range that was written, so the caller can flush caches over it. */
typedef struct UsPatchRange_t {
    void  *addr;
    size_t bytes;
} UsPatchRange;

/*
 * Describes where a patch goes. The tag is only for messages.
 *
 * `value` is written little endian and truncated to `width`, which the
 * configuration loader has already checked it fits.
 */
typedef struct UsPatchSpec_t {
    const char *target;
    uint32_t    rva;
    uint64_t    value;
    uint8_t     width;
    const char *tag;
} UsPatchSpec;

typedef enum UsPatchResult_e {
    UsPatchOk = 0,
    /* The RVA does not name a writable place inside this image. */
    UsPatchOutOfRange,
    /* The width is not one of 1, 2, 4 or 8. */
    UsPatchBadWidth,
} UsPatchResult;

const char *usPatchResultName(UsPatchResult r);

/*
 * Writes one patch, reporting the range that changed so the caller can make
 * the new instructions visible to the instruction fetcher.
 *
 * The caller is responsible for that flush, and for whatever the platform
 * requires before code can be written at all. This function only moves bytes,
 * which is what keeps it testable on a host.
 */
UsPatchResult usPatchApply(UsImage *img, const UsPatchSpec *spec, UsPatchRange *out);

#endif
