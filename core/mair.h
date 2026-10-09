/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What a descriptor's attribute index means
 *
 * A descriptor does not hold the memory attributes itself: it holds three bits
 * naming one of the eight entries in MAIR_EL1, and what those entries mean is
 * the kernel's choice rather than the architecture's. The usual assignment is
 * conventional and not guaranteed, so an index is never assumed here -- it is
 * looked up by the attribute byte it is supposed to hold, and a machine that
 * assigned them differently is answered by where the byte actually is, or by
 * nothing at all
 *
 * Only the byte is needed rather than the whole register, so nothing here
 * reads a system register: the caller hands the value in, and everything below
 * is arithmetic that a host can run
 */

#ifndef US_MAIR_H
#define US_MAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Normal memory, non-cacheable, as an attribute byte
 *
 * This is what a frame buffer wants. It is memory the CPU writes and the
 * display engine reads, and neither caches it usefully: caching writes there
 * would leave them visible only to the CPU, which is the whole screen
 * appearing frozen while the machine is fine
 *
 * Cacheable variants are deliberately not accepted as an alternative. A wrong
 * choice here draws nothing and looks exactly like a machine that stopped, so
 * a machine whose attributes do not include this is one to report and not to
 * draw on
 */
#define US_MAIR_NORMAL_NON_CACHEABLE 0x44U

/*
 * Where an attribute byte sits in MAIR_EL1, or -1 when it is not there
 *
 * The eight entries are eight bytes, least significant first, and the index is
 * the position: entry zero is the low byte. Zero is a legitimate index, so the
 * "not found" answer is negative rather than zero
 */
static inline int usMAIRIndex(uint64_t mair, uint8_t attr) {
    for (unsigned i = 0; i < 8U; i++) {
        if ((uint8_t)(mair >> (i * 8U)) == attr) {
            return (int)i;
        }
    }
    return -1;
}

/* The same, for the one attribute this project has a reason to want */
static inline int usMAIRNormalNonCacheableIndex(uint64_t mair) {
    return usMAIRIndex(mair, US_MAIR_NORMAL_NON_CACHEABLE);
}

#endif
