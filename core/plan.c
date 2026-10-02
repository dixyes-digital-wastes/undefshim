/*
 * Building and printing the plan, see plan.h for the contract.
 */

#include "core/plan.h"

void usPlanInit(UsPlan *plan) {
    *plan = (UsPlan){ 0 };
    usSiteListInit(&plan->sites);
    plan->poolBytes = US_POOL_BYTES;
    plan->poolPages = US_POOL_PAGES;
}

bool usPlanBuild(UsPlan *plan, UsImage *winload, UsImage *ntoskrnl) {
    bool leaf = false;
    bool handoff = false;

    usPlanInit(plan);

    if (ntoskrnl != NULL && ntoskrnl->valid) {
        plan->imageCount++;
        plan->ntoskrnlSizeOfImage = ntoskrnl->sizeOfImage;
        plan->ldapr = usCountLdapr(ntoskrnl);
        usCollectSites(&plan->sites, ntoskrnl, UsImageNtoskrnl);
        plan->thunk = usLocateSpareSlot(ntoskrnl, US_THUNK_BYTES);
    }

    if (winload != NULL && winload->valid) {
        plan->imageCount++;
        plan->winloadSizeOfImage = winload->sizeOfImage;

        /* The loader's own LDAPR instructions matter as much as the kernel's:
         * it runs on the same CPU before the kernel does. */
        UsLdaprCounts l = usCountLdapr(winload);
        plan->ldapr.word += l.word;
        plan->ldapr.xword += l.xword;
        plan->ldapr.byte += l.byte;
        plan->ldapr.half += l.half;
        plan->ldapr.total += l.total;

        usCollectSites(&plan->sites, winload, UsImageWinload);
        plan->vbar = usFindVbarTables(winload);
    }

    usSiteListSort(&plan->sites);

    for (size_t i = 0; i < plan->sites.count; i++) {
        if (plan->sites.sites[i].kind == UsSiteTransferLeaf) {
            leaf = true;
        } else if (plan->sites.sites[i].kind == UsSiteTtbrHandoff) {
            handoff = true;
        }
    }

    /*
     * Complete means everything the boot cannot proceed without is there. The
     * vector table writes are not part of it: they are needed before an
     * exception can arrive, but the kernel is what installs the table, and by
     * then the plan has already been acted on.
     */
    plan->complete = leaf && handoff && ntoskrnl != NULL && ntoskrnl->valid
                     && plan->thunk.found;
    return plan->complete;
}

/* --- printing ----------------------------------------------------------- */

static void emitPuts(const UsSink *sink, const char *s) {
    sink->puts(sink->ctx, s);
}
static void emitHex(const UsSink *sink, uint64_t v) {
    sink->hex(sink->ctx, v);
}

static void emitDec(const UsSink *sink, uint64_t v) {
    sink->dec(sink->ctx, v);
}

void usPlanEmit(const UsPlan *plan, const UsSink *sink) {
    emitPuts(sink, "plan: images ");
    emitDec(sink, plan->imageCount);
    emitPuts(sink, " ntoskrnl=");
    emitHex(sink, plan->ntoskrnlSizeOfImage);
    emitPuts(sink, " winload=");
    emitHex(sink, plan->winloadSizeOfImage);
    emitPuts(sink, "\n");

    emitPuts(sink, "plan: ldapr w=");
    emitDec(sink, plan->ldapr.word);
    emitPuts(sink, " x=");
    emitDec(sink, plan->ldapr.xword);
    emitPuts(sink, " b=");
    emitDec(sink, plan->ldapr.byte);
    emitPuts(sink, " h=");
    emitDec(sink, plan->ldapr.half);
    emitPuts(sink, " total=");
    emitDec(sink, plan->ldapr.total);
    emitPuts(sink, "\n");

    emitPuts(sink, "plan: sites ");
    emitDec(sink, plan->sites.count);
    emitPuts(sink, " of ");
    emitDec(sink, plan->sites.total);
    emitPuts(sink, "\n");

    for (size_t i = 0; i < plan->sites.count; i++) {
        const UsSite *s = &plan->sites.sites[i];

        emitPuts(sink, "plan: site ");
        emitPuts(sink, usSiteKindName(s->kind));
        emitPuts(sink, " ");
        emitPuts(sink, usImageKindName(s->image));
        emitPuts(sink, " +");
        emitHex(sink, s->rva);
        if (s->kind == UsSiteVbarWrite) {
            emitPuts(sink, " x");
            emitDec(sink, s->auxiliary);
        }
        emitPuts(sink, "\n");
    }

    emitPuts(sink, "plan: pool bytes=");
    emitDec(sink, plan->poolBytes);
    emitPuts(sink, " pages=");
    emitDec(sink, plan->poolPages);
    emitPuts(sink, "\n");

    emitPuts(sink, "plan: thunk ntoskrnl ");
    if (plan->thunk.found) {
        emitPuts(sink, "+");
        emitHex(sink, plan->thunk.rva);
        emitPuts(sink, " room=");
        emitDec(sink, plan->thunk.bytes);
        emitPuts(sink, " of ");
        emitDec(sink, plan->thunk.matches);
    } else {
        emitPuts(sink, "none");
    }
    emitPuts(sink, "\n");

    emitPuts(sink, "plan: vbar sites=");
    emitDec(sink, plan->vbar.sites);
    emitPuts(sink, " unresolved=");
    emitDec(sink, plan->vbar.unresolved);
    emitPuts(sink, " tables=");
    emitDec(sink, plan->vbar.count);
    emitPuts(sink, "\n");

    for (size_t i = 0; i < plan->vbar.count; i++) {
        emitPuts(sink, "plan: vbar winload +");
        emitHex(sink, plan->vbar.rvas[i]);
        emitPuts(sink, plan->vbar.syncFree[i] ? " sync-free" : " sync-in-use");
        emitPuts(sink, "\n");
    }

    emitPuts(sink, "plan: complete=");
    emitDec(sink, plan->complete ? 1 : 0);
    emitPuts(sink, "\n");
}
