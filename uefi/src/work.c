/*
 * The work order, collected on the target. See work.h.
 */

#include <uefi.h>

#include "core/plan.h"
#include "uefi/src/arm.h"
#include "uefi/src/console.h"
#include "uefi/src/stack.h"
#include "uefi/src/work.h"

/*
 * The plan is a couple of kilobytes, most of it the site list. Keeping it
 * static rather than on the stack is not about the size: the build runs on a
 * stack we own precisely because the caller's is not ours to spend, and one
 * more static costs nothing next to having to think about it.
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
 * service call.
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
     * Arming is off until the payload can survive the handover.
     *
     * It gets there: the branch lands, the stub runs, the payload is called.
     * What it cannot do there is read the page tables, because they are not
     * reachable from the address space that is in force by then, and reading
     * them is the whole point. Until that is answered a different way --
     * asking the hardware to translate rather than walking itself -- arming
     * only turns a working boot into one that stops at the handover.
     */
    if (req.complete && session->armEnabled && usArmTransfer(session)) {
        usConsolePuts("US-M6.5-ARMED\n");
    }
}
