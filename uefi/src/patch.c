/*
 * The debug patch table, see patch.h for why it works this way.
 */

#include <uefi.h>

#include "core/rva_patch.h"
#include "uefi/src/cache.h"
#include "uefi/src/console.h"
#include "uefi/src/patch.h"

static bool isApplied(const UsSession *s, uint32_t i) {
    return (s->patchApplied & (1u << i)) != 0;
}

static void markApplied(UsSession *s, uint32_t i) {
    s->patchApplied |= (1u << i);
}

static void reportName(const UsPatch *p) {
    usConsolePuts(p->tag != NULL ? p->tag : p->target);
}

int usPatchApplyPending(UsSession *s) {
    const UsConfig *cfg = s->config;
    int applied = 0;

    if (cfg == NULL) {
        return 0;
    }

    for (uint32_t i = 0; i < cfg->patchCount; i++) {
        const UsPatch *p;
        UsPatchSpec spec;
        UsPatchRange range;
        UsPatchResult r;
        UsImage *img;

        if (i >= US_PATCH_MAX) {
            /* Reported once by usPatchReportPending. */
            break;
        }
        if (isApplied(s, i)) {
            continue;
        }

        p = &cfg->patches[i];
        img = usRegistryByName(&s->registry, p->target);
        if (img == NULL) {
            /* Its stage has not been loaded yet, or the name is wrong. Both
             * look the same from here, and both are answered by the report at
             * the end of the boot. */
            continue;
        }

        spec.target = p->target;
        spec.rva = p->rva;
        spec.value = p->value;
        spec.width = p->width;
        spec.tag = p->tag;

        r = usPatchApply(img, &spec, &range);
        if (r != UsPatchOk) {
            usConsolePuts("patch: ");
            reportName(p);
            usConsolePuts(" failed: ");
            usConsolePuts(usPatchResultName(r));
            usConsolePuts("\n");
            /* The image is here, so retrying would fail the same way. */
            markApplied(s, i);
            continue;
        }

        /*
         * The image the patch lands in is about to run, and on AArch64 a
         * store does not reach the instruction fetcher by itself.
         */
        usCacheFlushRange(range.addr, range.bytes);

        usConsolePuts("patch: ");
        reportName(p);
        usConsolePuts(" at ");
        usConsolePutHex((uint64_t)(uintptr_t)range.addr);
        usConsolePuts(" = ");
        usConsolePutHex(p->value);
        usConsolePuts("/");
        usConsolePutDec((uint64_t)p->width);
        usConsolePuts("\n");

        markApplied(s, i);
        applied++;
    }

    /*
     * Printed once everything has been said. The deployment checks stop the
     * machine the moment they see this, and stopping on the report itself
     * would cut the report in half.
     */
    if (applied > 0) {
        usConsolePuts("US-M4-PATCH\n");
    }

    return applied;
}

void usPatchReportPending(const UsSession *s) {
    const UsConfig *cfg = s->config;

    if (cfg == NULL) {
        return;
    }

    for (uint32_t i = 0; i < cfg->patchCount; i++) {
        if (i >= US_PATCH_MAX) {
            usConsolePuts("patch: table longer than ");
            usConsolePutDec(US_PATCH_MAX);
            usConsolePuts(", the rest is ignored\n");
            return;
        }
        if (isApplied(s, i)) {
            continue;
        }
        usConsolePuts("patch: ");
        reportName(&cfg->patches[i]);
        usConsolePuts(" never matched an image\n");
    }
}
