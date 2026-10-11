/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Reading and writing through a mapping that may refuse, and saying so
 *
 * See probe.h and payload.h for the interfaces. Only the state of the bank
 * lives elsewhere, in state.c, because a nested fault has to find it before
 * anything else on this processor can run
 */

#include "payload/probe.h"

#include "common/layout.h"
#include "core/ldapr.h"
#include "payload/payload.h"
#include "payload/state.h"

/*
 * Says that a fault on this address is an answer rather than a fault
 *
 * Only one access at a time, and only on the processor making it, which is
 * what a nested fault on this path already is. The flag is read by the
 * handler the entry reaches, so nothing here can be kept in a register across
 * the access: the store is volatile for that reason
 *
 * The width is recorded with the address because a fault reports the address
 * that failed, which for an access that spans two pages is the second one -
 * so the handler cannot tell "the access I armed" from "something of my own"
 * by comparing addresses unless it knows which addresses the access covered
 */
static bool probeArm(int cpu, uint64_t at, uint64_t bytes) {
    if (cpu < 0 || cpu >= (int)US_MAX_CPUS) {
        return false;
    }
    gActive[cpu].probeAt = at;
    gActive[cpu].probeBytes = bytes;
    gActive[cpu].probeRefused = false;
    gActive[cpu].probing = true;
    return true;
}

static bool probeDisarm(int cpu) {
    bool refused = gActive[cpu].probeRefused;

    gActive[cpu].probing = false;
    return !refused;
}

/*
 * Reads a word, saying whether the read happened. A read that faults leaves
 * the value alone rather than zeroing it, so a caller that ignores the answer
 * gets an old value rather than a plausible one
 */
bool usPayloadProbeRead(uint64_t at, uint64_t *value) {
    int cpu = currentCPU();
    uint64_t got;

    if (value == NULL || !probeArm(cpu, at, sizeof(uint64_t))) {
        return false;
    }
    got = *(const volatile uint64_t *)(uintptr_t)at;
    if (!probeDisarm(cpu)) {
        return false;
    }
    *value = got;
    return true;
}

/*
 * The same, one instruction wide. A site is four bytes and only has to be
 * four byte aligned, so reading eight from one that sits at the end of its
 * page reaches into the next - and a fault that comes back from there names
 * the page that failed rather than the address the read started at, which is
 * not the address the handler was told to expect
 */
bool usPayloadProbeReadWord(uint64_t at, uint32_t *value) {
    int cpu = currentCPU();
    uint32_t got;

    if (value == NULL || !probeArm(cpu, at, sizeof(uint32_t))) {
        return false;
    }
    got = *(const volatile uint32_t *)(uintptr_t)at;
    if (!probeDisarm(cpu)) {
        return false;
    }
    *value = got;
    return true;
}

/* Writes a word, saying whether the store happened */
bool usPayloadProbeWrite(uint64_t at, uint64_t value) {
    int cpu = currentCPU();

    if (!probeArm(cpu, at, sizeof(uint64_t))) {
        return false;
    }
    *(volatile uint64_t *)(uintptr_t)at = value;
    return probeDisarm(cpu);
}

/* The same, one instruction wide: a site is four bytes, and writing eight
 * would take the instruction after it with it */
bool usPayloadProbeWriteWord(uint64_t at, uint32_t value) {
    int cpu = currentCPU();

    if (!probeArm(cpu, at, sizeof(uint32_t))) {
        return false;
    }
    *(volatile uint32_t *)(uintptr_t)at = value;
    return probeDisarm(cpu);
}

bool usPayloadProbeAnswered(int cpu, uint64_t far) {
    if (!gActive[cpu].probing
        || !usAccessCovers(far, gActive[cpu].probeAt, gActive[cpu].probeBytes)) {
        return false;
    }
    gActive[cpu].probeRefused = true;
    gActive[cpu].probing = false;
    return true;
}
