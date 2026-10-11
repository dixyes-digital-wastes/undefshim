/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The exception registers, read
 *
 * Every one of these is a system register holding part of the exception that
 * is being handled, and reading it is the same instruction wherever it is
 * read. They live here rather than beside either user because there are two:
 * the handler, which needs them to carry out what faulted, and the panic,
 * which needs them to say what was happening when the machine stopped
 */

#ifndef US_REGS_H
#define US_REGS_H

#include <stdint.h>

static inline uint64_t currentESR(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(value));
    return value;
}

static inline uint64_t currentFAR(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, far_el1" : "=r"(value));
    return value;
}

static inline uint64_t currentSPSR(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, spsr_el1" : "=r"(value));
    return value;
}

static inline uint64_t currentVBAR(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, vbar_el1" : "=r"(value));
    return value;
}

static inline uint64_t currentELR(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, elr_el1" : "=r"(value));
    return value;
}

static inline void usSetELR(uint64_t elr) {
    __asm__ volatile("msr elr_el1, %0" ::"r"(elr));
}

/*
 * The platform register, which the kernel keeps its per-processor block in and
 * the compiler is told not to allocate. What the entry gives back there is
 * checked against TPIDR_EL1, so both are read here
 */
static inline uint64_t currentTPIDR(void) {
    uint64_t value;

    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(value));
    return value;
}

static inline uint64_t usReadX18(void) {
    uint64_t value;

    __asm__ volatile("mov %0, x18" : "=r"(value));
    return value;
}

#endif
