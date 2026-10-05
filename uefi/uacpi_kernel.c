/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What uACPI asks of the thing it runs on
 *
 * uACPI is a general ACPI reader: it locates the tables the firmware
 * published, and it parses and runs the AML in the DSDT and the SSDTs. What
 * it does not carry is any of the machine underneath -- a heap, a clock, a
 * way to find the root pointer. Those are here.
 *
 * Every entry point is one of three things: something this boot genuinely
 * has (the root pointer, a clock, memory), something a boot-time reader does
 * not need (interrupts, PCI config, work scheduling), or a lock. The ones a
 * boot does not need are stubs with the answer that keeps uACPI going, and
 * they are named as such rather than left out, because the linker is what
 * finds a missing one and it finds it at the worst moment
 *
 * The heap is TLSF over one pool from the firmware, and it is only brought up
 * when something asks for it: a lookup of the serial tables is done through
 * uACPI's early table access, which needs no heap at all, and that is the
 * case this project is in almost all of the time
 */

#include <uefi.h>

/*
 * uACPI reads _MSC_VER to decide which compiler it is being built by, and
 * this target defines it whether or not the toolchain is MSVC's. Hiding it
 * for the length of the include is what its own porting notes suggest
 */
#pragma push_macro("_MSC_VER")
#undef _MSC_VER
#define UACPI_OVERRIDE_TYPES
#include "uacpi/uacpi.h"
#pragma pop_macro("_MSC_VER")

#include "third_party/tlsf/tlsf.h"
#include "uefi/acpi.h"
#include "uefi/console.h"
#include "uefi/uacpi_kernel.h"

/* --- finding the tables -------------------------------------------------------- */

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    const void *rsdp = usACPIFindRSDP();

    if (rsdp == NULL) {
        return UACPI_STATUS_NOT_FOUND;
    }
    /* The boot runs with the identity mapping, so the address the firmware
     * published is the address to read it at. It is the physical address
     * uACPI wants, which is the same number here */
    *out_rsdp_address = (uacpi_phys_addr)(uintptr_t)rsdp;
    return UACPI_STATUS_OK;
}

/*
 * A table is mapped where the firmware left it, so there is nothing to do.
 * The length is ignored for the same reason: nothing here unmaps anything
 */
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    (void)len;
    return (void *)(uintptr_t)addr;
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
}

/* --- saying what it is doing --------------------------------------------------- */

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *message) {
    UsLogLevel ours;

    switch (level) {
    case UACPI_LOG_ERROR:
        ours = UsLogError;
        break;
    case UACPI_LOG_WARN:
        ours = UsLogWarn;
        break;
    case UACPI_LOG_INFO:
        ours = UsLogVerbose;
        break;
    default:
        ours = UsLogDebug;
        break;
    }
    /* The message arrives with its newline already, and a line of the log is
     * one call. Trimming it here is what keeps uACPI's lines looking like
     * every other line: the log writes the terminator itself */
    {
        char line[US_UACPI_LOG_BYTES];
        size_t length = 0;

        while (message[length] != '\0' && length + 1U < sizeof(line)) {
            char c = message[length];

            line[length++] = c == '\n' || c == '\r' ? ' ' : c;
        }
        while (length != 0 && line[length - 1U] == ' ') {
            length--;
        }
        line[length] = '\0';
        if (length != 0) {
            usLog(ours, "acpi", "%s\n", line);
        }
    }
}

/* --- the heap ------------------------------------------------------------------ */

/*
 * One pool, taken from the firmware the first time something asks for
 * memory. That moment is not "when uACPI is brought up": the serial tables
 * are read without a heap, and a machine that only ever does that never
 * allocates anything
 */
static tlsf_t gHeap;

static bool heapUp(void) {
    void *arena;

    if (gHeap != NULL) {
        return true;
    }
    arena = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiBootServicesData, US_UACPI_HEAP_BYTES,
                                   &arena)) || arena == NULL) {
        return false;
    }
    gHeap = tlsf_create_with_pool(arena, US_UACPI_HEAP_BYTES);
    if (gHeap == NULL) {
        BS->FreePool(arena);
        return false;
    }
    return true;
}

uacpi_status uacpi_kernel_initialize(uacpi_init_level level) {
    (void)level;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_deinitialize(void) {
}

void *uacpi_kernel_alloc(uacpi_size size) {
    if (!heapUp()) {
        return NULL;
    }
    return tlsf_malloc(gHeap, size);
}

void *uacpi_kernel_alloc_zeroed(uacpi_size size) {
    void *p = uacpi_kernel_alloc(size);

    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void uacpi_kernel_free(void *memory) {
    if (memory != NULL && gHeap != NULL) {
        tlsf_free(gHeap, memory);
    }
}

/* --- the clock ----------------------------------------------------------------- */

void uacpi_kernel_stall(uacpi_u8 usec) {
    BS->Stall(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    BS->Stall((uintn_t)(msec * 1000U));
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    static uacpi_u64 frequency;
    uacpi_u64 ticks;

    if (frequency == 0) {
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    }
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return frequency != 0 ? (ticks * 1000000000ULL) / frequency : 0;
}

/* --- locks --------------------------------------------------------------------- */

/*
 * There is one thing running and it is doing all of this itself: every lock
 * is a handle with no state and every acquire succeeds at once. uACPI only
 * asks that a handle is not NULL
 */
static uacpi_handle nonNullHandle(void) {
    return (uacpi_handle)1;
}

uacpi_handle uacpi_kernel_create_mutex(void) {
    return nonNullHandle();
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    (void)handle;
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    (void)handle;
    (void)timeout;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    (void)handle;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    return nonNullHandle();
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    (void)handle;
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    (void)handle;
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    (void)handle;
    (void)flags;
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id)1;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    return 0;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    (void)state;
}

/* --- events -------------------------------------------------------------------- */

/*
 * Nothing here waits for anything: the boot is single threaded and runs to
 * the end of the parse before anything else happens, so an event that is
 * waited for has already happened
 */
uacpi_handle uacpi_kernel_create_event(void) {
    return nonNullHandle();
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    (void)handle;
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    (void)handle;
    (void)timeout;
    return UACPI_TRUE;
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    (void)handle;
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    (void)handle;
}

/* --- port IO and PCI ----------------------------------------------------------- */

/*
 * The port is where the address says it is, so a handle is the address. Only
 * reachable from an opregion, which a read of the serial tables does not have
 */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len,
                                 uacpi_handle *out_handle) {
    (void)len;
    *out_handle = (uacpi_handle)(uintptr_t)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    (void)handle;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset,
                                   uacpi_u8 *out_value) {
    *out_value = *(volatile uacpi_u8 *)((uacpi_u8 *)(uintptr_t)handle + offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u16 *out_value) {
    *out_value = *(volatile uacpi_u16 *)((uacpi_u8 *)(uintptr_t)handle + offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u32 *out_value) {
    *out_value = *(volatile uacpi_u32 *)((uacpi_u8 *)(uintptr_t)handle + offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 in_value) {
    *(volatile uacpi_u8 *)((uacpi_u8 *)(uintptr_t)handle + offset) = in_value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 in_value) {
    *(volatile uacpi_u16 *)((uacpi_u8 *)(uintptr_t)handle + offset) = in_value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 in_value) {
    *(volatile uacpi_u32 *)((uacpi_u8 *)(uintptr_t)handle + offset) = in_value;
    return UACPI_STATUS_OK;
}

/*
 * There is no configuration space to read: this is reduced hardware mode, so
 * the tables describe memory and not a PCI bus, and nothing here asks. The
 * answers are the ones a bus with nothing on it gives
 */
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out_handle) {
    (void)address;
    *out_handle = nonNullHandle();
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    (void)handle;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset,
                                    uacpi_u8 *value) {
    (void)device;
    (void)offset;
    *value = 0xFF;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset,
                                     uacpi_u16 *value) {
    (void)device;
    (void)offset;
    *value = 0xFFFF;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset,
                                     uacpi_u32 *value) {
    (void)device;
    (void)offset;
    *value = 0xFFFFFFFFU;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset,
                                     uacpi_u8 value) {
    (void)device;
    (void)offset;
    (void)value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset,
                                      uacpi_u16 value) {
    (void)device;
    (void)offset;
    (void)value;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset,
                                      uacpi_u32 value) {
    (void)device;
    (void)offset;
    (void)value;
    return UACPI_STATUS_OK;
}

/* --- the rest ------------------------------------------------------------------ */

/*
 * Interrupt handlers, work, and firmware requests are for a general purpose
 * OS holding the tables for its lifetime. This reads them once and stops
 * existing, and none of the three can be reached from that
 */
uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context,
    uacpi_handle *out_irq_handle) {
    (void)irq;
    (void)handler;
    (void)context;
    *out_irq_handle = NULL;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    (void)handler;
    (void)irq_handle;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    (void)request;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler handler,
                                        uacpi_handle context) {
    (void)type;
    (void)handler;
    (void)context;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK;
}
