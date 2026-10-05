/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
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

/* The two words a boolean is written with, so the dump reads like the file */
static const char *boolWord(bool value) {
    return value ? "true" : "false";
}

/*
 * The configuration as it was understood rather than as it was written
 *
 * Every key the driver read is printed, the ones the file did not mention
 * included, so what a machine is doing can be read off this alone - without
 * also having to know what the defaults are. The shape is the file's own, one
 * table to a line, because the next question after reading this is almost
 * always what the file said, and two documents that look alike are easier to
 * compare than two that do not
 */
static void printConfig(const UsConfig *cfg) {
    usLogV("config", "[log] level=\"" US_VALUE("%s") "\"\n",
           usConfigLogLevelName(cfg->logLevel));

    if (cfg->hasUART) {
        usLogV("config", "[uart] baseAddr=" US_VALUE("%#llx") " type=\""
               US_VALUE("%s") "\" width=" US_VALUE("%u") " color=" US_VALUE("%s")
               "\n",
               (unsigned long long)cfg->uartBase, cfg->uartType,
               (unsigned)cfg->uartWidth, boolWord(cfg->uartColour));
    } else {
        usLogV("config", "[uart] absent\n");
    }

    usLogV("config", "[ldapr] imageInplaceRewrite=" US_VALUE("%s")
           " el0InplaceRewrite=" US_VALUE("%s") "\n",
           boolWord(cfg->imageInplaceRewrite), boolWord(cfg->el0InplaceRewrite));

    usLogV("config", "[stats] enabled=" US_VALUE("%s") "\n",
           boolWord(cfg->statsEnabled));

    if (cfg->hasDescriptorBase) {
        usLogV("config", "[kernel] descriptorBaseRVA=" US_VALUE("%#x") "\n",
               (unsigned)cfg->descriptorBaseRVA);
    } else {
        usLogV("config", "[kernel] absent\n");
    }

    usLogV("config", "[patch] dir=\"" US_VALUE("%s") "\"\n", cfg->patchDir);

    /* Every switch, so a machine that has taken something out of the way says
     * so here rather than leaving it to be worked out from what it did not do */
    usLogV("config", "[debug] notArmVectors=" US_VALUE("%s")
           " notArmVectorsEl1t=" US_VALUE("%s") " notArmHandover=" US_VALUE("%s")
           " notVamap=" US_VALUE("%s") " spxStack=" US_VALUE("%s")
           " patches=" US_VALUE("%u") "\n",
           boolWord(cfg->notArmVectors), boolWord(cfg->notArmVectorsEl1t),
           boolWord(cfg->notArmHandover), boolWord(cfg->notVamap),
           boolWord(cfg->spxStack), (unsigned)cfg->patchCount);
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

    usLogI("undefshim", US_VERSION_STRING "\n");
    switch (result) {
    case UsConfigLoaded:
        usLogI("config", "loaded\n");
        break;
    case UsConfigAbsent:
        usLogI("config", "absent, using defaults\n");
        break;
    case UsConfigBroken:
        usLogE("config", "broken: %s\n", msg);
        usLogE("config", "not arming\n");
        usConsoleMilestone("M2 failed");
        return 0;
    }

    usConsoleLevel(cfg->logLevel);
    printConfig(cfg);
    usConsoleMilestone("M2 done");

    /* From here on the driver has to be resident to be useful, so this is
     * where the work of staying in the loop starts */
    if (!usSessionInit(&gSession)) {
        usLogE("session", "no pool\n");
        usConsoleMilestone("M4 failed");
        return 0;
    }

    /*
     * The switches come from the configuration, each named for what it stops,
     * so a document that does not mention them gets the whole mechanism. They
     * are read here rather than inside usSessionInit because the file has to
     * be read first, and because reading one twice is how a check that turned
     * a thing off ended up running with it on
     */
    gSession.stubVectors = !cfg->notArmVectors;
    gSession.stubVectorsEl1t = !cfg->notArmVectorsEl1t;
    gSession.stubHandover = !cfg->notArmHandover;
    gSession.vamap = !cfg->notVamap;
    gSession.spxStack = cfg->spxStack;
    gSession.imageInplaceRewrite = cfg->imageInplaceRewrite;

    /* Handed over rather than freed: the patch table names stages that are
     * loaded long after this function has returned */
    gSession.config = cfg;
    usLogV("pool", "pa=" US_VALUE("%#llx") " va=" US_VALUE("%#llx")
           " bytes=" US_VALUE("%#llx") " slots=" US_VALUE("%u") "\n",
           (unsigned long long)gSession.poolAlloc.basePA,
           (unsigned long long)gSession.poolAlloc.baseVA,
           (unsigned long long)gSession.poolAlloc.bytes,
           (unsigned)gSession.pool->stackSlots);
    usLogV("pool", "stack[0]=" US_VALUE("%#llx") " stack[" US_VALUE("%u") "]=" US_VALUE("%#llx") "\n",
           (unsigned long long)gSession.pool->stackTop[0], (unsigned)US_MAX_CPUS - 1U,
           (unsigned long long)gSession.pool->stackTop[US_MAX_CPUS - 1]);
    usLogI("pool", "ready\n");

    /* The payload goes into place before anything is armed against it: if it
     * cannot be placed there is nothing to enter, and every later step would
     * be arming something that is not there */
    {
        UsPayloadPlace place;

        if (!usPayloadPlace(&gSession, &place)) {
            usLogE("payload", "no executable memory\n");
            usConsoleMilestone("M6 failed");
            return 0;
        }
        usPayloadReport(&gSession);
    }

    if (!usLoadImageHookInstall(&gSession)) {
        usLogE("loadimage", "hook failed\n");
        usConsoleMilestone("M4 failed");
        return 0;
    }
    usLogI("loadimage", "hooked\n");

    /* winload and the kernel arrive without a protocol, so a point in the
     * boot where they are both in memory has to be waited for */
    if (!usGmmHookInstall(&gSession)) {
        usLogE("gmm", "hook failed\n");
        usConsoleMilestone("M4 failed");
        return 0;
    }
    usLogI("gmm", "armed\n");

    /* The notification publishes high-VA exception targets before low VAs retire */
    if (gSession.vamap && !usVAMapArm(&gSession)) {
        usLogE("vamap", "cannot arm\n");
        usConsoleMilestone("M6 failed");
        return 0;
    }

    usConsoleMilestone("M4 setup");
    return 0;
}
