/*
 * undefshim boot service driver
 *
 * Loaded before the Windows boot manager runs. This is the smallest version
 * that proves the deployment path: it announces itself on the console and the
 * UART, then returns so the firmware keeps it resident
 *
 * Everything here prints through console.h, never printf. The shell starts
 * this driver on its own stack, and printf's formatted output path wants tens
 * of kilobytes of it; overflowing that stack corrupts firmware memory and the
 * boot then dies later, somewhere unrelated
 */

#include <uefi.h>

#include "common/layout.h"
#include "common/version.h"
#include "uefi/config.h"
#include "uefi/console.h"
#include "uefi/screen.h"
#include "uefi/gmm_hook.h"
#include "uefi/loadimage_hook.h"
#include "uefi/payload_place.h"
#include "uefi/registry.h"
#include "uefi/session.h"
#include "uefi/vamap.h"

/* The driver's state. One instance, because there is one driver */
static UsSession gSession;

static void printConfig(const UsConfig *cfg) {
    usConsoleLog("config", UsLogVerbose);
    usConsolePuts("level=");
    usConsolePutDec((uint64_t)cfg->logLevel);
    usConsolePuts(" rewrite=");
    usConsolePutDec((uint64_t)cfg->ldaprRewrite);
    usConsolePuts(" debug=");
    usConsolePutDec((uint64_t)cfg->debugEnabled);
    usConsolePuts(" patches=");
    usConsolePutDec((uint64_t)cfg->patchCount);
    usConsolePuts("\n");

    for (uint32_t i = 0; i < cfg->patchCount; i++) {
        const UsPatch *p = &cfg->patches[i];

        usConsoleLog("patch", UsLogVerbose);
        usConsolePuts(p->target != NULL ? p->target : "-");
        usConsolePuts(" +");
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
    char msg[192];
    UsConfigLoad result;

    (void)argc;
    (void)argv;

    /*
     * The configuration is read before anything is said, because it is what
     * says where to say it: a machine that does not name a UART wants no
     * serial output at all, and until it is read the console stays silent.
     * That includes the report of a broken configuration, which is why the
     * error path is going to have to write to the screen instead
     */
    /* Before the configuration is read: a configuration that cannot be read
     * is the case where the screen is the only way to say so */
    usScreenInit();

    result = usConfigLoad(&cfg, msg, sizeof(msg));
    if (cfg != NULL && cfg->hasUART) {
        usConsoleUse(cfg->uartType[0] == 'p' ? UsUARTPL011 : UsUART8250,
                     cfg->uartBase, cfg->uartWidth);
    }
    if (cfg != NULL) {
        usConsoleColour(cfg->uartColour);
    }

    usConsoleLog("undefshim", UsLogInfo);
    usConsolePuts(US_VERSION_STRING "\n");
    switch (result) {
    case UsConfigLoaded:
        usConsoleLog("config", UsLogInfo);
        usConsolePuts("loaded\n");
        break;
    case UsConfigAbsent:
        usConsoleLog("config", UsLogInfo);
        usConsolePuts("absent, using defaults\n");
        break;
    case UsConfigBroken:
        usConsoleLog("config", UsLogError);
        usConsolePuts("broken: ");
        usConsolePuts(msg);
        usConsoleLog("config", UsLogError);
        usConsolePuts("not arming\n");
        usConsoleMilestone("M2 failed");
        return 0;
    }

    usConsoleLevel(cfg->logLevel);
    printConfig(cfg);
    usConsoleMilestone("M2 done");

    /* From here on the driver has to be resident to be useful, so this is
     * where the work of staying in the loop starts */
    if (!usSessionInit(&gSession)) {
        usConsoleLog("session", UsLogError);
        usConsolePuts("no pool\n");
        usConsoleMilestone("M4 failed");
        return 0;
    }

    /* After the session is initialised, which establishes its own defaults.
     * The switches that change what the boot does are debugging decisions
     * like any other, so they come from the configuration file */
    gSession.armEnabled = usConfigDebugBool(cfg, "arm", false);
    gSession.armSlot0 = usConfigDebugBool(cfg, "armSlot0", true);
    gSession.spxStack = usConfigDebugBool(cfg, "spxStack", false);
    gSession.vamapEnabled = usConfigDebugBool(cfg, "vamap", gSession.armEnabled);
    /*
     * Not from the debug section: this one is a scanning decision and is
     * parsed into the configuration proper, under [scan]. Reading it here as
     * a debug key would silently override whatever the file said with the
     * default, which is how a check that turns the replacement off ended up
     * running with it on
     */
    gSession.ldaprRewrite = cfg->ldaprRewrite;

    /* Handed over rather than freed: the patch table names stages that are
     * loaded long after this function has returned */
    gSession.config = cfg;
    usConsoleLog("pool", UsLogVerbose);
    usConsolePuts("pa=");
    usConsolePutHex(gSession.poolAlloc.basePA);
    usConsolePuts(" va=");
    usConsolePutHex(gSession.poolAlloc.baseVA);
    usConsolePuts(" bytes=");
    usConsolePutHex(gSession.poolAlloc.bytes);
    usConsolePuts(" slots=");
    usConsolePutDec((uint64_t)gSession.pool->stackSlots);
    usConsolePuts("\n");

    usConsoleLog("pool", UsLogVerbose);
    usConsolePuts("stack[0]=");
    usConsolePutHex(gSession.pool->stackTop[0]);
    usConsolePuts(" stack[");
    usConsolePutDec(US_MAX_CPUS - 1U);
    usConsolePuts("]=");
    usConsolePutHex(gSession.pool->stackTop[US_MAX_CPUS - 1]);
    usConsolePuts("\n");
    usConsoleLog("pool", UsLogInfo);
    usConsolePuts("ready\n");

    /* The payload goes into place before anything is armed against it: if it
     * cannot be placed there is nothing to enter, and every later step would
     * be arming something that is not there */
    {
        UsPayloadPlace place;

        if (!usPayloadPlace(&gSession, &place)) {
            usConsoleLog("payload", UsLogError);
            usConsolePuts("no executable memory\n");
            usConsoleMilestone("M6 failed");
            return 0;
        }
        usPayloadReport(&gSession);
    }

    if (!usLoadImageHookInstall(&gSession)) {
        usConsoleLog("loadimage", UsLogError);
        usConsolePuts("hook failed\n");
        usConsoleMilestone("M4 failed");
        return 0;
    }
    usConsoleLog("loadimage", UsLogInfo);
    usConsolePuts("hooked\n");

    /* winload and the kernel arrive without a protocol, so a point in the
     * boot where they are both in memory has to be waited for */
    if (!usGmmHookInstall(&gSession)) {
        usConsoleLog("gmm", UsLogError);
        usConsolePuts("hook failed\n");
        usConsoleMilestone("M4 failed");
        return 0;
    }
    usConsoleLog("gmm", UsLogInfo);
    usConsolePuts("armed\n");

    /* The notification publishes high-VA exception targets before low VAs retire */
    if (gSession.vamapEnabled && !usVAMapArm(&gSession)) {
        usConsoleLog("vamap", UsLogError);
        usConsolePuts("cannot arm\n");
        usConsoleMilestone("M6 failed");
        return 0;
    }

    usConsoleMilestone("M4 setup");
    return 0;
}
