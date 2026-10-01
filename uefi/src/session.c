/*
 * The per boot state, see session.h.
 */

#include "core/pool.h"
#include "uefi/src/session.h"

bool usSessionInit(UsSession *s) {
    usRegistryInit(&s->registry);
    s->config = NULL;
    s->patchApplied = 0;

    if (!usPoolAllocate(&s->poolAlloc)) {
        return false;
    }
    s->pool = s->poolAlloc.pool;

    /*
     * Slot zero is the boot CPU. The pool has a stack per possible CPU and the
     * payload maps them by MPIDR when it takes over; at this point only one
     * CPU is running, and it is the one whose stack this is.
     */
    s->bootStackTop = usPoolStackTop(s->pool, 0);
    return s->bootStackTop != 0;
}
