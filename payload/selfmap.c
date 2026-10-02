/*
 * Finding our own mapping, see selfmap.h.
 */

#include "core/pgtable.h"
#include "payload/selfmap.h"

/*
 * Which half of the address space each base register describes. The two are
 * not interchangeable, and which one is in use depends on the moment: the
 * firmware maps everything through the lower one, while the kernel and the
 * loader that hands over to it describe their own regions in the upper one.
 */
#define US_VA_LOWER_FIRST 0x0000000000000000ULL
#define US_VA_LOWER_LAST 0x0000FFFFFFFFFFFFULL
#define US_VA_UPPER_FIRST 0xFFFF000000000000ULL
#define US_VA_UPPER_LAST 0xFFFFFFFFFFFFFFFFULL

/*
 * Reading a physical address.
 *
 * This works because of where it runs: at the handover, and during the boot
 * before it, physical memory is reachable at its own address. That is a
 * property of the moment, not of the code, which is why the walk is confined
 * to this moment.
 */
static uint64_t readPhys(void *ctx, uint64_t address) {
    (void)ctx;
    return *(const volatile uint64_t *)(uintptr_t)address;
}

static UsPageWalk walkWith(uint64_t base, uint64_t pa, uint64_t bytes,
                           uint64_t vaFirst, uint64_t vaLast) {
    if ((base & US_PAGE_ADDR_MASK) == 0) {
        /* No table at all. Walking from zero would read whatever happens to
         * be at the bottom of memory, which is a fault and not an answer. */
        UsPageWalk none = { 0 };

        return none;
    }
    return usPageWalkFind(readPhys, NULL, base, vaFirst, vaLast, pa, bytes);
}

UsSelfMap usSelfMapFind(uint64_t targetPa, uint64_t targetBytes) {
    UsSelfMap out = { 0 };
    uint64_t ttbr0;
    uint64_t ttbr1;
    uint64_t sctlr;
    UsPageWalk walk;

    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    /*
     * A regime with the MMU off has no tables, and an address is its own
     * physical address. Reporting that rather than walking keeps the caller
     * from having to know which kind of moment it is in, and keeps the walk
     * away from base registers that hold nothing.
     */
    if ((sctlr & 1) == 0) {
        out.found = true;
        out.va = targetPa;
        out.pa = targetPa;
        out.size = US_GRANULE_4K;
        return out;
    }

    /*
     * The kernel's own regions live in the upper half, so that is what is
     * looked at first. Before the kernel is running, the upper half is
     * described by nothing at all, and the pool is somewhere in the lower
     * one, so the search moves there.
     */
    walk = walkWith(ttbr1, targetPa, targetBytes, US_VA_UPPER_FIRST, US_VA_UPPER_LAST);
    if (!walk.found && !walk.budgetExhausted) {
        UsPageWalk lower = walkWith(ttbr0, targetPa, targetBytes,
                                    US_VA_LOWER_FIRST, US_VA_LOWER_LAST);

        if (lower.found) {
            walk = lower;
        } else {
            lower.entriesRead += walk.entriesRead;
            walk = lower;
        }
    }

    out.found = walk.found;
    out.va = walk.va;
    out.pa = walk.mappedPa;
    out.size = walk.size;
    out.entriesRead = walk.entriesRead;
    out.exhausted = walk.budgetExhausted;
    return out;
}
