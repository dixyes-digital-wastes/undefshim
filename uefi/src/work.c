/*
 * The work order, collected on the target. See work.h
 */

#include <uefi.h>

#include "core/plan.h"
#include "uefi/src/arm.h"
#include "uefi/src/console.h"
#include "uefi/src/rewrite.h"
#include "uefi/src/stack.h"
#include "uefi/src/work.h"

/*
 * The plan is a couple of kilobytes, most of it the site list. Keeping it
 * static rather than on the stack is not about the size: the build runs on a
 * stack we own precisely because the caller's is not ours to spend, and one
 * more static costs nothing next to having to think about it
 */
static UsPlan gPlan;

static void sinkPuts(void *ctx, const char *s) {
    (void)ctx;
    usConsolePuts(s);
}

static void sinkHex(void *ctx, uint64_t v) {
    (void)ctx;
    usConsolePutHex(v);
}

static void sinkDec(void *ctx, uint64_t v) {
    (void)ctx;
    usConsolePutDec(v);
}

static const UsSink gSink = {
    .ctx = NULL,
    .puts = sinkPuts,
    .hex = sinkHex,
    .dec = sinkDec,
};

typedef struct CollectRequest_t {
    UsSession *session;
    bool       complete;
} CollectRequest;

/*
 * On our own stack, because walking an image reads section tables and runs
 * the exception directory lookup, and this is reached from inside a firmware
 * service call
 */
static void collectOnOwnStack(void *arg) {
    CollectRequest *req = arg;
    UsSession *s = req->session;

    req->complete = usPlanBuild(&gPlan,
                                usRegistryGet(&s->registry, UsImageWinload),
                                usRegistryGet(&s->registry, UsImageNtoskrnl));
    usPlanEmit(&gPlan, &gSink);
}

void usWorkCollect(UsSession *session) {
    CollectRequest req = { .session = session, .complete = false };

    usConsolePuts("work: collecting\n");
    usStackRunOn(session->bootStackTop, collectOnOwnStack, &req);

    usConsolePuts(req.complete ? "US-M5-PLAN\n" : "US-M5-INCOMPLETE\n");

    /*
     * Drawing the exception path covers what the replacement cannot reach:
     * code generated after the boot, and images that were never scanned. It
     * is done from the boot because everything it needs -- the loader's table,
     * the payload's address -- is known there, and the memory it writes is
     * writable there
     */
    if (req.complete && session->armEnabled && usArmVectorTable(session)) {
        usConsolePuts("US-M6.5-ARMED\n");
    }
    /*
     * Replacing the instructions happens after the arming, not before.
     * The arming scans the image's own code to find its vector table and
     * its free space, and scanning a rewritten image gives it different
     * answers: doing it the other way round stopped inside the driver
     * while arming ntoskrnl, with all of its LDAPRs already an LDAR.
     * Both still happen before the kernel runs any of it
     */
    if (req.complete && session->ldaprRewrite) {
        size_t replaced = usRewriteLdapr(session);

        usConsolePuts(replaced != 0 ? "US-M7-REWRITTEN\n" : "US-M7-NOREWRITE\n");
    }
}
