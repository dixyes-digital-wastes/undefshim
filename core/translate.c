#include "core/translate.h"

bool usTranslateAddress(uint64_t va, bool write, uint64_t *pa) {
    uint64_t sctlr;
    uint64_t savedPar;
    uint64_t par;

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & 1U) == 0) {
        *pa = va;
        return true;
    }
    __asm__ volatile("mrs %0, par_el1" : "=r"(savedPar));
    if (write) {
        __asm__ volatile("at s1e1w, %1\n\tisb\n\tmrs %0, par_el1"
                         : "=r"(par) : "r"(va) : "memory");
    } else {
        __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1"
                         : "=r"(par) : "r"(va) : "memory");
    }
    __asm__ volatile("msr par_el1, %0" :: "r"(savedPar) : "memory");
    if ((par & 1U) != 0) {
        return false;
    }
    *pa = (par & UINT64_C(0x0000FFFFFFFFF000)) | (va & 0xFFFU);
    return true;
}
