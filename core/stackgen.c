#include "core/stackgen.h"

#include <stddef.h>

uint32_t usGenerateStackLookup(const uint64_t *mpidr, uint32_t cpuCount,
                               uint32_t codeRva, uint32_t stackTopTableRva,
                               uint32_t readyRva, uint32_t haltRva,
                               uint32_t out[US_STACK_LOOKUP_WORDS]) {
    uint64_t ids[US_MAX_CPUS];
    uint32_t loadWords[US_MAX_CPUS];
    uint32_t readyWords[US_MAX_CPUS];
    uint32_t used = 0;

    if (mpidr == NULL || out == NULL || cpuCount == 0 || cpuCount > US_MAX_CPUS
        || ((codeRva | readyRva | haltRva) & 3U) != 0
        || (stackTopTableRva & 7U) != 0
        || (uint64_t)codeRva + US_STACK_LOOKUP_WORDS * 4U > UINT64_C(0x100000000)
        || (uint64_t)stackTopTableRva + cpuCount * 8U > UINT64_C(0x100000000)) {
        return 0;
    }

    /* Preflight every candidate before publishing any instruction */
    for (uint32_t i = 0; i < cpuCount; i++) {
        ids[i] = mpidr[i];
        if ((ids[i] & ~US_MPIDR_AFFINITY_MASK) != 0) {
            return 0;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (ids[i] == ids[j]) {
                return 0;
            }
        }
        uint32_t bitCount = 0;
        for (uint64_t bits = ids[i]; bits != 0; bits &= bits - 1) {
            bitCount++;
        }
        uint64_t loadPc = (uint64_t)codeRva + (used + 4U + bitCount) * 4U;
        int64_t loadDelta = (int64_t)((uint64_t)stackTopTableRva + i * 8U)
                            - (int64_t)loadPc;
        int64_t readyDelta = (int64_t)readyRva - (int64_t)(loadPc + 4U);
        if (loadDelta < -1'048'576 || loadDelta > 1'048'572
            || readyDelta < -134'217'728 || readyDelta > 134'217'724) {
            return 0;
        }
        loadWords[i] = 0x58000012U | (((uint32_t)(loadDelta / 4) & 0x7FFFFU) << 5);
        readyWords[i] = 0x14000000U | ((uint32_t)(readyDelta / 4) & 0x03FFFFFFU);
        used += 6U + bitCount;
    }
    int64_t haltDelta = (int64_t)haltRva - ((int64_t)codeRva + used * 4U);
    if (haltDelta < -134'217'728 || haltDelta > 134'217'724
        || used + 1U > US_STACK_LOOKUP_WORDS) {
        return 0;
    }
    uint32_t haltWord = 0x14000000U | ((uint32_t)(haltDelta / 4) & 0x03FFFFFFU);

    uint32_t n = 0;
    for (uint32_t i = 0; i < cpuCount; i++) {
        out[n++] = 0xD53800B2U; /* mrs x18, mpidr_el1 */
        out[n++] = 0x9260DE52U; /* clear bits 24..31 */
        out[n++] = 0x92409E52U; /* clear bits 40..63 */
        for (uint32_t bit = 0; bit < 40; bit++) {
            if ((ids[i] & (UINT64_C(1) << bit)) != 0) {
                out[n++] = 0xD2400252U | (((64U - bit) & 63U) << 16);
            }
        }
        out[n++] = 0xB5000072U; /* cbnz x18, next candidate */
        out[n++] = loadWords[i];
        out[n++] = readyWords[i];
    }
    out[n++] = haltWord;
    used = n;
    while (n < US_STACK_LOOKUP_WORDS) {
        out[n++] = 0xD503201FU;
    }
    return used;
}
