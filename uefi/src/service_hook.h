/*
 * Replacing an entry in the boot services table.
 *
 * Both hooks this driver installs work the same way: the firmware's table is
 * shared, so an entry is swapped for ours, the original is kept and called
 * through, and the header checksum is recomputed because the firmware may
 * verify it.
 *
 * The table lives in memory the firmware wrote, and the pointer to it comes
 * from the system table, so a hook is only as good as the assumption that the
 * entry really points where it did. The install reports whether the write took,
 * by reading the slot back.
 */

#ifndef US_SERVICE_HOOK_H
#define US_SERVICE_HOOK_H

#include <stdbool.h>

/*
 * The slots hold function pointers, which the language does not let be
 * converted to and from object pointers portably. Every target this builds for
 * has one pointer representation, and the alternative is a separate struct per
 * service signature, so the cast is made in one place instead of everywhere.
 */
typedef struct UsServiceHook_t {
    void **slot;        /* address of the table entry */
    void  *original;    /* what was there before */
    void  *replacement; /* ours */
    bool   installed;
} UsServiceHook;

/*
 * Swaps the entry at slot for replacement, after recomputing the checksum of
 * the table's first page. Returns false when the write did not take, which is
 * a reason to stop rather than to carry on without the hook.
 */
bool usServiceHookInstall(UsServiceHook *hook, void *const *slot, void *replacement);

/*
 * Puts the original back. Safe to call when not installed, so a hook can be
 * undone from a cleanup path without checking.
 */
void usServiceHookRemove(UsServiceHook *hook);

#endif
