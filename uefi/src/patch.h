/*
 * Applying the configuration's patch table.
 *
 * The table is the path that goes around the scanner: it names an image, an
 * offset into it and the bytes to write there. It exists so a place in a
 * stage that has not been analysed yet can be reached anyway, which is what
 * makes it useful while bringing the driver up, and it is the same mechanism
 * a kernel version nothing else matches would be handled with later.
 *
 * An image named by the table may not be in memory yet, so nothing here is
 * one shot: every call writes whatever has become possible since the last
 * one, and leaves the rest pending.
 */

#ifndef US_PATCH_H
#define US_PATCH_H

#include <stdint.h>

#include "uefi/src/session.h"

/*
 * One bit per table entry lives in the session, so this is what bounds the
 * table. It comes from a file on the boot volume, which means anything at all
 * can be written in it.
 */
#define US_PATCH_MAX 32

/*
 * Writes every pending patch whose image the registry now holds. Returns how
 * many were written this time.
 *
 * It runs on whatever stack the caller is on, so it keeps to the console's
 * fixed size output and no formatted printing.
 */
int usPatchApplyPending(UsSession *s);

/*
 * Reports the entries that never found their image. Called once the boot has
 * got as far as it is going to, so that a typo in a target name is visible
 * instead of looking like a patch that quietly did nothing.
 */
void usPatchReportPending(const UsSession *s);

#endif
