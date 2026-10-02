/*
 * Replacing the instructions the hardware does not have.
 *
 * An image that is in memory and not yet running can simply have its RCpc
 * loads replaced by acquire loads. Nothing has to fault, nothing has to be
 * taken over, and the addresses do not matter: the instruction is the same
 * length, uses the same registers and is at least as strongly ordered.
 *
 * This is what makes the shim work for the kernel's own code. The kernel
 * installs its own vector table once it is running, and that table's
 * synchronous slot is not free, so taking it over means forwarding to a
 * handler whose address is only known once the address space has been
 * rebuilt. Replacing the instructions avoids the question rather than
 * answering it.
 *
 * The exception path is still what covers everything this cannot reach: code
 * that is generated after the boot, and images that were never scanned.
 */

#ifndef US_UEFI_REWRITE_H
#define US_UEFI_REWRITE_H

#include <stddef.h>

#include "uefi/src/session.h"

/*
 * Replaces every RCpc load in the images that have been registered. Returns
 * how many were replaced, which is worth reporting: zero means the scan found
 * nothing, and that is a different thing from the scan not having run.
 */
size_t usRewriteLdapr(UsSession *session);

/*
 * The same thing for one image, for the ones that arrive one at a time.
 *
 * An image the firmware loads -- ci.dll, a driver -- is in memory and not yet
 * running between LoadImage returning and the caller calling StartImage, which
 * is the same situation the kernel is in when this is done for it. Doing it
 * there means the exception path does not have to cover them at all.
 */
size_t usRewriteOne(UsImage *img);

#endif
