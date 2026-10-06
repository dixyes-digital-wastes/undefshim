/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Real placement and generator, with only firmware allocation/cache/IO mocked.
 * The AArch64 blob is copied and inspected, never executed on the host.
 * Keep host libc headers out: posix-uefi supplies its own libc declarations
 */
#include <uefi.h>

#include <assert.h>
#include <stddef.h>

#include "core/stackgen.h"
#include "payload/payload.h"
#include "payload_blob.h"
#include "uefi/cache.h"
#include "uefi/console.h"
#include "uefi/session.h"

#define US_CHECK_NAME "test_payload_place"
#include "check.h"

#define PAYLOAD_PAGES ((US_PAYLOAD_BYTES + US_PAGE_SIZE - 1) / US_PAGE_SIZE)

static _Alignas(US_PAGE_SIZE) uint8_t pages[PAYLOAD_PAGES * US_PAGE_SIZE];
static unsigned allocations;
static unsigned flushes;
static efi_status_t allocationStatus;
static bool zeroAddress;

static efi_status_t EFIAPI allocatePages(efi_allocate_type_t type,
                                        efi_memory_type_t memoryType,
                                        uintn_t count,
                                        efi_physical_address_t *address) {
    assert(type == AllocateAnyPages);
    assert(memoryType == EfiRuntimeServicesCode);
    assert(count == PAYLOAD_PAGES);
    assert(*address == 0);
    allocations++;
    *address = zeroAddress ? 0 : (efi_physical_address_t)(uintptr_t)pages;
    return allocationStatus;
}

static efi_boot_services_t bootServices = { .AllocatePages = allocatePages };
efi_boot_services_t *BS = &bootServices;

void usCacheFlushRange(const void *address, size_t bytes) {
    assert(address == pages);
    assert(bytes == US_PAYLOAD_BYTES);
    assert(allocations == 1);
    flushes++;
}

/* Report is linked but not called: its self-test is AArch64 code */
void usLog(UsLogLevel level, const char *tag, const char *fmt, ...) {
    (void)level;
    (void)tag;
    (void)fmt;
}
void usConsoleMilestone(const char *what) { (void)what; }

static void rejected(const UsACPICPUs *cpus) {
    UsSession session = { .cpus = *cpus };
    /* No pool is intentional: rejection must not reach config writes either */
    UsPayloadPlace out;
    UsPayloadPlace before;
    memset(&out, 0xA5, sizeof(out));
    before = out;
    memset(pages, 0xA5, sizeof(pages));
    allocations = flushes = 0;
    assert(!usPayloadPlace(&session, &out));
    assert(allocations == 0 && flushes == 0);
    assert(!session.payloadPlaced);
    assert(memcmp(&out, &before, sizeof(out)) == 0);
    for (size_t i = 0; i < sizeof(pages); i++) {
        assert(pages[i] == 0xA5);
    }
}

static void placed(const UsACPICPUs *cpus) {
    UsPool pool = {0};
    /*
     * The console the boot found. It is on the session rather than in the
     * configuration because the two are not the same question: a file may
     * say only that the firmware knows where its port is, and where that
     * turned out to be is worked out while the console is opened. The
     * payload reports through that port, so reading the file instead left
     * it printing to address zero
     */
    UsConfig config = { .el0InplaceRewrite = true };
    UsSession session = { .pool = &pool, .cpus = *cpus,
                          .uartOpen = true, .uartKind = UsUARTPL011,
                          .uartBase = 0x94080000ULL, .uartWidth = 32U,
                          .config = &config };
    UsPayloadPlace out;
    uint32_t expected[US_STACK_LOOKUP_WORDS];
    uint8_t snapshot[sizeof(pages)];
    for (unsigned i = 0; i < US_MAX_CPUS; i++) {
        pool.stackTop[i] = UINT64_C(0xFFFFF00040000000) + i * US_STACK_SIZE;
    }
    allocations = flushes = 0;
    allocationStatus = EFI_SUCCESS;
    zeroAddress = false;
    memset(pages, 0xA5, sizeof(pages));
    assert(usPayloadPlace(&session, &out));
    assert(allocations == 1 && flushes == 1);
    assert(session.payloadPlaced);
    assert(memcmp(&out, &session.payloadPlace, sizeof(out)) == 0);
    assert(out.basePA == (uint64_t)(uintptr_t)pages);
    assert(out.baseVA == out.basePA && out.bytes == US_PAYLOAD_BYTES);
    assert(out.entryVA == out.baseVA + US_PAYLOAD_ENTRY_OFFSET);
    assert(out.configVA == out.baseVA + US_PAYLOAD_CONFIG_OFFSET);

    const UsPayloadConfig *cfg = (const UsPayloadConfig *)(uintptr_t)out.configVA;
    assert(cfg->cpuCount == cpus->count);
    for (size_t i = 0; i < cpus->count; i++) {
        assert(cfg->cpus[i].mpidr == cpus->mpidr[i]);
        assert(cfg->cpus[i].index == i);
    }
    assert(cfg->cpus[cpus->count].mpidr == 0);
    assert(cfg->cpus[cpus->count].index == UINT64_MAX);
    assert(memcmp(cfg->stackTop, pool.stackTop, sizeof(cfg->stackTop)) == 0);
    assert(cfg->selfVA == out.baseVA);
    assert(cfg->poolBase == (uint64_t)(uintptr_t)&pool);
    assert(cfg->uartBase == 0x94080000ULL);
    assert(cfg->uartKind == 1U);
    assert(cfg->uartWidth == 32U);
    assert(cfg->quiet == 1);
    /*
     * The replacement is asked for and cannot happen: the configuration states
     * no base for the kernel's descriptor mapping, and without one every
     * attempt is refused. A payload told to try anyway would take the probe
     * bank - a stack the kernel owns - on every RCpc load in user code, to
     * reach the answer it is given here instead
     */
    assert(cfg->el0InPlace == 0);

    /* Generator semantics have their own interpreter test. Here the complete
     * published lookup must be generated from exactly the cfg/session table,
     * including its count, order and actual blob-relative stack addresses
     */
    assert(usGenerateStackLookup(cpus->mpidr, (uint32_t)cpus->count,
                                US_PAYLOAD_STACKLOOKUP_OFFSET,
                                US_PAYLOAD_CONFIG_OFFSET
                                    + offsetof(UsPayloadConfig, stackTop),
                                US_PAYLOAD_READY_OFFSET,
                                US_PAYLOAD_NOSTACK_OFFSET, expected) != 0);
    assert(memcmp(pages + US_PAYLOAD_STACKLOOKUP_OFFSET,
                  expected, sizeof(expected)) == 0);
    /* Outside those two patched regions, placement must copy the real blob */
    for (size_t i = 0; i < US_PAYLOAD_BYTES; i++) {
        bool lookup = i >= US_PAYLOAD_STACKLOOKUP_OFFSET
                      && i < US_PAYLOAD_STACKLOOKUP_OFFSET + sizeof(expected);
        bool config = i >= US_PAYLOAD_CONFIG_OFFSET
                      && i < US_PAYLOAD_CONFIG_OFFSET + sizeof(*cfg);
        if (!lookup && !config) {
            assert(pages[i] == kPayloadBlob[i]);
        }
    }
    memcpy(snapshot, pages, sizeof(pages));
    /* Even a now-invalid session table cannot replace an existing placement */
    session.cpus.count = 0;
    pool.stackTop[0]++;
    UsPayloadPlace again = {0};
    assert(usPayloadPlace(&session, &again));
    assert(allocations == 1 && flushes == 1);
    assert(memcmp(&out, &again, sizeof(out)) == 0);
    assert(memcmp(snapshot, pages, sizeof(pages)) == 0);
}

int main(void) {
    rejected(&(UsACPICPUs){ .count = 0 });
    rejected(&(UsACPICPUs){ .count = US_MAX_CPUS + 1 });
    rejected(&(UsACPICPUs){ .count = 1, .mpidr = { UINT64_C(1) << 24 } });
    rejected(&(UsACPICPUs){ .count = 2, .mpidr = { 0, UINT64_C(1) << 63 } });
    rejected(&(UsACPICPUs){ .count = 3, .mpidr = { 0x100, 0, 0x100 } });
    placed(&(UsACPICPUs){ .count = 1, .mpidr = { 0 } });
    placed(&(UsACPICPUs){ .count = 3,
                         .mpidr = { UINT64_C(0x8000010000), 0, 0x100 } });
    placed(&(UsACPICPUs){ .count = 8,
                         .mpidr = { 0x103, 2, 0x101, 0, 0x102, 1, 0x100, 3 } });

    /* Allocation failure is still reported cleanly after table preflight */
    for (unsigned zero = 0; zero < 2; zero++) {
        UsSession session = { .cpus = { .count = 1 } };
        UsPayloadPlace out = {0};
        allocations = flushes = 0;
        zeroAddress = zero != 0;
        allocationStatus = zero ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
        assert(!usPayloadPlace(&session, &out));
        assert(allocations == 1 && flushes == 0);
        assert(!session.payloadPlaced);
        assert(out.basePA == 0 && out.baseVA == 0);
    }
    return usCheckPassed("preflight, session cfg/lookup and idempotence passed");
}
