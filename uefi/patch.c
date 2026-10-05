/*
 * The debug patch table, see patch.h for why it works this way
 */

#include <uefi.h>

#include "core/rva_patch.h"
#include "uefi/cache.h"
#include "uefi/console.h"
#include "uefi/patch.h"

static bool isApplied(const UsSession *s, uint32_t i) {
    return (s->patchApplied & (1u << i)) != 0;
}

static void markApplied(UsSession *s, uint32_t i) {
    s->patchApplied |= (1u << i);
}

/* The patch's own name: what the configuration called it, or the image it
 * names when it was not given one */
static const char *patchName(const UsPatch *p) {
    return p->tag != NULL ? p->tag : p->target;
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
            /* Reported once by usPatchReportPending */
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
             * the end of the boot */
            continue;
        }

        spec.target = p->target;
        spec.rva = p->rva;
        spec.value = p->value;
        spec.width = p->width;
        spec.tag = p->tag;

        r = usPatchApply(img, &spec, &range);
        if (r != UsPatchOk) {
            usLogE("patch", "%s failed: %s\n", patchName(p), usPatchResultName(r));
            /* The image is here, so retrying would fail the same way */
            markApplied(s, i);
            continue;
        }

        /*
         * The image the patch lands in is about to run, and on AArch64 a
         * store does not reach the instruction fetcher by itself
         */
        usCacheFlushRange(range.addr, range.bytes);

        usLogV("patch", "%s at " US_VALUE("%#llx") " = " US_VALUE("%#llx") "/"
               US_VALUE("%u") "\n", patchName(p),
               (unsigned long long)(uintptr_t)range.addr,
               (unsigned long long)p->value, (unsigned)p->width);

        markApplied(s, i);
        applied++;
    }

    /*
     * Printed once everything has been said. The deployment checks stop the
     * machine the moment they see this, and stopping on the report itself
     * would cut the report in half
     */
    if (applied > 0) {
        usConsoleMilestone("M4 patched");
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
            usLogW("patch", "table longer than " US_VALUE("%u") ", the rest is ignored\n",
                   (unsigned)US_PATCH_MAX);
            return;
        }
        if (isApplied(s, i)) {
            continue;
        }
        usLogW("patch", "%s never matched an image\n", patchName(&cfg->patches[i]));
    }
}
