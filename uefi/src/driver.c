/*
 * undefshim boot service driver.
 *
 * Loaded before the Windows boot manager runs. This is the smallest version
 * that proves the deployment path: it announces itself on the console and the
 * UART, then returns so the firmware keeps it resident.
 *
 * Everything here prints through console.h, never printf. The shell starts
 * this driver on its own stack, and printf's formatted output path wants tens
 * of kilobytes of it; overflowing that stack corrupts firmware memory and the
 * boot then dies later, somewhere unrelated.
 */

#include <uefi.h>

#include "common/layout.h"
#include "common/version.h"
#include "uefi/src/config.h"
#include "uefi/src/console.h"
#include "uefi/src/gmm_hook.h"
#include "uefi/src/loadimage_hook.h"
#include "uefi/src/registry.h"
#include "uefi/src/session.h"

/* The driver's state. One instance, because there is one driver. */
static UsSession gSession;

static void printConfig(const UsConfig *cfg) {
    usConsolePuts("log.level=");
    usConsolePutDec((uint64_t)cfg->logLevel);
    usConsolePuts(" ldapr_rewrite=");
    usConsolePutDec((uint64_t)cfg->ldaprRewrite);
    usConsolePuts(" debug.enabled=");
    usConsolePutDec((uint64_t)cfg->debugEnabled);
    usConsolePuts(" patches=");
    usConsolePutDec((uint64_t)cfg->patchCount);
    usConsolePuts("\n");

    for (uint32_t i = 0; i < cfg->patchCount; i++) {
        const UsPatch *p = &cfg->patches[i];
        usConsolePuts("patch[");
        usConsolePutDec((uint64_t)i);
        usConsolePuts("] ");
        usConsolePuts(p->target != NULL ? p->target : "-");
        usConsolePuts(" + ");
        usConsolePutHex((uint64_t)p->rva);
        usConsolePuts(" = ");
        usConsolePutHex(p->value);
        usConsolePuts("/");
        usConsolePutDec((uint64_t)p->width);
        usConsolePuts(" ");
        usConsolePuts(p->tag != NULL ? p->tag : "-");
        usConsolePuts("\n");
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    usConsoleInit();
    usConsolePuts("undefshim " US_VERSION_STRING "\n");

    {
        UsConfig *cfg = NULL;
        char msg[192];
        UsConfigLoad result = usConfigLoad(&cfg, msg, sizeof(msg));

        switch (result) {
        case UsConfigLoaded:
            usConsolePuts("config: loaded\n");
            break;
        case UsConfigAbsent:
            usConsolePuts("config: absent, using defaults\n");
            break;
        case UsConfigBroken:
            usConsolePuts("config: broken: ");
            usConsolePuts(msg);
            usConsolePuts("\nconfig: not arming\n");
            usConsolePuts("US-M2-FAIL\n");
            return 0;
        }

        printConfig(cfg);
        usConfigFree(cfg);
        usConsolePuts("US-M2-DONE\n");
    }

    /* From here on the driver has to be resident to be useful, so this is
     * where the work of staying in the loop starts. */
    if (!usSessionInit(&gSession)) {
        usConsolePuts("session: no pool\n");
        usConsolePuts("US-M4-FAIL\n");
        return 0;
    }
    usConsolePuts("pool: pa=");
    usConsolePutHex(gSession.poolAlloc.basePa);
    usConsolePuts(" va=");
    usConsolePutHex(gSession.poolAlloc.baseVa);
    usConsolePuts(" bytes=");
    usConsolePutHex(gSession.poolAlloc.bytes);
    usConsolePuts(" slots=");
    usConsolePutDec((uint64_t)gSession.pool->stackSlots);
    usConsolePuts("\n");

    usConsolePuts("pool: stack[0]=");
    usConsolePutHex(gSession.pool->stackTop[0]);
    usConsolePuts(" stack[7]=");
    usConsolePutHex(gSession.pool->stackTop[US_MAX_CPUS - 1]);
    usConsolePuts("\npool: ready\n");

    if (!usLoadImageHookInstall(&gSession)) {
        usConsolePuts("loadimage: hook failed\n");
        usConsolePuts("US-M4-FAIL\n");
        return 0;
    }
    usConsolePuts("loadimage: hooked\n");

    /* winload and the kernel arrive without a protocol, so a point in the
     * boot where they are both in memory has to be waited for. */
    if (!usGmmHookInstall(&gSession)) {
        usConsolePuts("gmm: hook failed\n");
        usConsolePuts("US-M4-FAIL\n");
        return 0;
    }
    usConsolePuts("gmm: armed\n");

    usConsolePuts("US-M4-SETUP\n");
    return 0;
}
