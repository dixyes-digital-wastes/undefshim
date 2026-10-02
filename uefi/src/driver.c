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
#include "uefi/src/payload_place.h"
#include "uefi/src/registry.h"
#include "uefi/src/session.h"
#include "uefi/src/vamap.h"

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
    UsConfig *cfg = NULL;

    (void)argc;
    (void)argv;

    usConsoleInit();
    usConsolePuts("undefshim " US_VERSION_STRING "\n");

    {
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
        usConsolePuts("US-M2-DONE\n");
    }

    /* From here on the driver has to be resident to be useful, so this is
     * where the work of staying in the loop starts. */
    if (!usSessionInit(&gSession)) {
        usConsolePuts("session: no pool\n");
        usConsolePuts("US-M4-FAIL\n");
        return 0;
    }

    /* After the session is initialised, which establishes its own defaults.
     * The switches that change what the boot does are debugging decisions
     * like any other, so they come from the configuration file. */
    gSession.armEnabled = usConfigDebugBool(cfg, "arm", false);
    gSession.vamapEnabled = usConfigDebugBool(cfg, "vamap", false);
    /*
     * Not from the debug section: this one is a scanning decision and is
     * parsed into the configuration proper, under [scan]. Reading it here as
     * a debug key would silently override whatever the file said with the
     * default, which is how a check that turns the replacement off ended up
     * running with it on.
     */
    gSession.ldaprRewrite = cfg->ldaprRewrite;

    /* Handed over rather than freed: the patch table names stages that are
     * loaded long after this function has returned. */
    gSession.config = cfg;
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

    /* The payload goes into place before anything is armed against it: if it
     * cannot be placed there is nothing to enter, and every later step would
     * be arming something that is not there. */
    {
        UsPayloadPlace place;

        if (!usPayloadPlace(&gSession, &place)) {
            usConsolePuts("payload: no executable memory\n");
            usConsolePuts("US-M6-FAIL\n");
            return 0;
        }
        usPayloadReport(&gSession);
    }

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

    /*
     * Off by default, and it is worth saying why the default is not the
     * interesting direction: the record this leaves behind is readable from
     * physical memory after the fact, but nothing consumes the addresses it
     * finds yet. See vamap.h and the milestone notes.
     */
    if (gSession.vamapEnabled && !usVaMapArm(&gSession)) {
        usConsolePuts("vamap: cannot arm\n");
        usConsolePuts("US-M6-FAIL\n");
        return 0;
    }

    usConsolePuts("US-M4-SETUP\n");
    return 0;
}
