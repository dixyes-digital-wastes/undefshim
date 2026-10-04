/*
 * Asking the hardware to translate an address, see translate.h.
 *
 * Two questions are answered here and they are not the same one. The first is
 * "where does the EL1&0 regime put this address", which is what the kernel
 * will see and what the handover searches. The second is "where does the
 * regime I am running in put it", which is what a load or store of mine
 * actually reaches, and at the boot the two disagree: the boot runs at EL2
 * under a translation regime that maps physical memory to itself, while the
 * EL1&0 tables are already the kernel's and no longer hold an image at the
 * address the loader put it at.
 *
 * AT S1E1R/W answers the first question at any exception level. AT S1E2R/W
 * answers the second from EL2 without VHE, and is not available from EL1, so
 * a caller at EL1 asking about its own access gets the EL1&0 answer, which is
 * the same thing there.
 */

#include "core/translate.h"

#include "common/layout.h"
#include "core/par.h"

/* HCR_EL2.E2H: EL2 shares the EL1&0 translation regime (VHE). */
#define US_HCR_E2H (1ULL << 34)

static bool queryAt(uint64_t va, bool write, bool el2, uint64_t *pa) {
    uint64_t savedPar;
    uint64_t par;

    __asm__ volatile("mrs %0, par_el1" : "=r"(savedPar));
    if (el2) {
        if (write) {
            __asm__ volatile("at s1e2w, %1\n\tisb\n\tmrs %0, par_el1"
                             : "=r"(par) : "r"(va) : "memory");
        } else {
            __asm__ volatile("at s1e2r, %1\n\tisb\n\tmrs %0, par_el1"
                             : "=r"(par) : "r"(va) : "memory");
        }
    } else {
        if (write) {
            __asm__ volatile("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1"
                             : "=r"(par) : "r"(va) : "memory");
        } else {
            __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1"
                             : "=r"(par) : "r"(va) : "memory");
        }
    }
    __asm__ volatile("msr par_el1, %0" :: "r"(savedPar) : "memory");

    UsPar decoded = usParDecode(par);

    if (!decoded.valid) {
        return false;
    }
    /* The low bits of the page are the virtual address's own. */
    *pa = decoded.pa | (va & (US_PAGE_SIZE - 1));
    return true;
}

bool usTranslateAddress(uint64_t va, bool write, uint64_t *pa) {
    uint64_t sctlr;

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1U) == 0) {
        *pa = va;
        return true;
    }
    return queryAt(va, write, false, pa);
}

bool usTranslateOwnAddress(uint64_t va, bool write, uint64_t *pa) {
    uint64_t el;
    uint64_t hcr;
    uint64_t sctlr;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if ((el >> 2) == 2) {
        __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
        if ((hcr & US_HCR_E2H) == 0) {
            __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
            if ((sctlr & 1U) == 0) {
                *pa = va;
                return true;
            }
            return queryAt(va, write, true, pa);
        }
    }
    return usTranslateAddress(va, write, pa);
}
