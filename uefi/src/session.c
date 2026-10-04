/*
 * The per boot state, see session.h
 */

#include "core/pool.h"
#include "uefi/src/acpi.h"
#include "uefi/src/session.h"

bool usSessionInit(UsSession *s) {
    usRegistryInit(&s->registry);
    s->config = NULL;
    s->patchApplied = 0;
    s->payloadPlaced = false;
    s->armEnabled = false;
    s->vamapEnabled = false;
    /* On unless the configuration says otherwise: this is the mechanism the
     * kernel depends on, and the driver sets it from the file afterwards */
    s->ldaprRewrite = true;

    /* Read once: the tables are the boot's and are gone with it */
    s->cpus = usACPIProbeCPUs();
    if (s->cpus.count == 0) {
        uint64_t mpidr;

        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        s->cpus.mpidr[0] = mpidr & US_MPIDR_AFFINITY_MASK;
        s->cpus.count = 1;
    }

    if (!usPoolAllocate(&s->poolAlloc)) {
        return false;
    }
    s->pool = s->poolAlloc.pool;

    /*
     * Slot zero is the boot CPU. The pool has a stack per possible CPU and the
     * payload maps them by MPIDR when it takes over; at this point only one
     * CPU is running, and it is the one whose stack this is
     */
    s->bootStackTop = usPoolStackTop(s->pool, 0);
    return s->bootStackTop != 0;
}
