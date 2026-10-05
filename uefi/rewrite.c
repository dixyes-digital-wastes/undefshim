/*
 * Carrying out the RCpc loads by replacing them, see rewrite.h
 */

#include <uefi.h>

#include "core/ldapr.h"
#include "core/pe.h"
#include "core/scan.h"
#include "uefi/cache.h"
#include "uefi/console.h"
#include "uefi/registry.h"
#include "uefi/rewrite.h"
#include "uefi/session.h"

/*
 * Walks the executable sections of one image and replaces each RCpc load
 *
 * The scan is over sections that carry data, for the same reason the other
 * locators use that rule: a section that is only an entry in the section table
 * is not mapped, and reading it produces zeros rather than code
 *
 * The whole section is flushed once rather than each instruction: this is a
 * boot time pass over a large image, and per-instruction flushes would spend
 * all of their time on barrier overhead
 */
static size_t rewriteImage(UsImage *img) {
    size_t changed = 0;

    if (img == NULL || !img->valid) {
        return 0;
    }

    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        uint8_t *base;
        uint32_t span;
        bool touched = false;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0 || s->virtualSize == 0
            || s->rawSize == 0) {
            continue;
        }

        base = (uint8_t *)(uintptr_t)usImageRVAToPtr(img, s->virtualAddress);
        if (base == NULL) {
            continue;
        }

        span = s->virtualSize;
        /* Anything past the file's own bytes is zero fill, which cannot be an
         * instruction, but it is still inside the section and the read has to
         * be bounded by the image rather than by the section */
        if (s->rawSize < span) {
            span = s->rawSize;
        }

        for (uint32_t off = 0; off + 4 <= span; off += 4) {
            uint32_t insn = (uint32_t)base[off] | ((uint32_t)base[off + 1] << 8)
                            | ((uint32_t)base[off + 2] << 16)
                            | ((uint32_t)base[off + 3] << 24);
            uint32_t replacement;

            if (!usLDAPRToLDAR(insn, &replacement)) {
                continue;
            }
            base[off] = (uint8_t)replacement;
            base[off + 1] = (uint8_t)(replacement >> 8);
            base[off + 2] = (uint8_t)(replacement >> 16);
            base[off + 3] = (uint8_t)(replacement >> 24);
            changed++;
            touched = true;
        }

        if (touched) {
            usCacheFlushRange(base, span);
        }
    }

    return changed;
}

size_t usRewriteOne(UsImage *img) {
    size_t changed = rewriteImage(img);

    if (changed != 0) {
        usConsolePuts("rewrite: ");
        usConsolePutDec(changed);
        usConsolePuts(" in an image at ");
        usConsolePutHex((uint64_t)(uintptr_t)img->base);
        usConsolePuts("\n");
    }
    return changed;
}

size_t usRewriteLDAPR(UsSession *session) {
    static const UsImageKind kinds[] = {
        UsImageNtoskrnl,
        UsImageWinload,
    };
    size_t total = 0;

    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        UsImage *img = usRegistryGet(&session->registry, kinds[i]);
        size_t changed;

        if (img == NULL) {
            continue;
        }
        /* How many instructions the image carried, whether or not any were
         * replaced: a scan that found nothing and a scan that did not run
         * look the same otherwise, and they mean opposite things */
        usConsolePuts("rewrite: ");
        usConsolePuts(usImageKindName(kinds[i]));
        usConsolePuts(" holds ");
        usConsolePutDec((uint64_t)usCountLDAPR(img).total);
        usConsolePuts("\n");
        changed = rewriteImage(img);
        if (changed != 0) {
            usConsolePuts("rewrite: ");
            usConsolePutDec(changed);
            usConsolePuts(" in ");
            usConsolePuts(usImageKindName(kinds[i]));
            usConsolePuts("\n");
        }
        total += changed;
    }

    return total;
}
