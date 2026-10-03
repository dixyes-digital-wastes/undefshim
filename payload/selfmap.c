/*
 * Finding our own mapping, see selfmap.h.
 */

#include "core/translate.h"
#include "payload/selfmap.h"

UsSelfMap usSelfMapFind(uint64_t targetPa, uint64_t targetBytes, uint64_t nearVa) {
    UsSelfMap out = { 0 };
    uint64_t sctlr;
    uint64_t base;
    uint64_t va;

    if (targetBytes == 0) {
        return out;
    }

    /*
     * A regime with the MMU off has nothing to translate and a translation
     * attempt there is not defined to answer anything useful. An address is
     * its own physical address, which is the answer, and saying so keeps the
     * caller from having to know which kind of moment it is in.
     */
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1) == 0) {
        out.found = true;
        out.va = targetPa;
        out.pa = targetPa;
        out.size = US_PAGE_SIZE;
        return out;
    }

    /*
     * The window is placed relative to an address in the new space, so an
     * anchor below it cannot produce one.
     */
    if (nearVa < US_SELFMAP_WINDOW) {
        return out;
    }

    /*
     * The sweep is at page granularity, and that is not a refinement: it is
     * the only granularity that works.
     *
     * A region this size is not covered by a block entry, so it is mapped page
     * by page, and its start has no reason to fall on a block boundary. An
     * earlier version swept at block granularity and compared exact addresses,
     * which can only match when the mapping happens to be aligned the same way
     * at both ends: it found nothing, over the whole window, without a single
     * failed translation to suggest anything was wrong.
     */
    base = (nearVa & ~(US_PAGE_SIZE - 1)) - US_SELFMAP_WINDOW;

    for (va = base; va <= base + 2 * US_SELFMAP_WINDOW; va += US_PAGE_SIZE) {
        uint64_t pa;

        if (out.probes >= US_SELFMAP_MAX_PROBES) {
            out.exhausted = true;
            return out;
        }
        out.probes++;

        if (!usTranslateAddress(va, false, &pa)) {
            out.faulted = true;
            continue;
        }
        if (pa != (targetPa & ~(US_PAGE_SIZE - 1))) {
            continue;
        }

        /*
         * A page inside the target is mapped here. The first one found is the
         * answer: the search runs upwards, so it is the lowest address that
         * reaches the region, and the rest of the region follows it.
         */
        out.found = true;
        out.va = va;
        out.pa = targetPa & ~(US_PAGE_SIZE - 1);
        out.size = US_PAGE_SIZE;
        return out;
    }

    return out;
}
