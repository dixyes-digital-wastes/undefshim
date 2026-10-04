/*
 * Intercepting GetMemoryMap, and the reason it is the right place
 *
 * winload is loaded by bootmgfw, which does not go through the boot services
 * table to do it, so nothing tells us when winload appears. What it does do is
 * call back into the table once it is running, and one of those calls is
 * GetMemoryMap: it has to ask for the memory map before it can leave boot
 * services and hand over to the kernel
 *
 * So the call is an event, not a service we need to change. When it arrives we
 * are inside the boot with winload already in memory and still running, which
 * is the one window in which the kernel can be reached before it starts
 *
 * The map is also handed to us for free, and it is exactly the list of regions
 * worth searching: the images we are after are in loader memory
 */

#ifndef US_GMM_HOOK_H
#define US_GMM_HOOK_H

#include <stdbool.h>

#include "uefi/src/session.h"

/*
 * Installs the hook. Returns false when the table entry could not be written
 *
 * Once the images have been found the hook removes itself, so the cost is paid
 * once rather than on every memory map query for the rest of the boot
 */
bool usGmmHookInstall(UsSession *session);

#endif
