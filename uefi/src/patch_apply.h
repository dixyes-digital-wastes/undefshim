/*
 * Applying the patch lists named by the configuration to the image they are
 * for, at the moment the image is loaded and before anything of it has run
 *
 * That moment is the point of the whole exercise: the list is in place when
 * the kernel takes its first instruction, so whatever integrity check it
 * makes later is made against what the list left behind, and there is no
 * change for it to notice
 */
#ifndef US_PATCH_APPLY_H
#define US_PATCH_APPLY_H

#include "core/pe.h"
#include "uefi/src/session.h"

/*
 * Reads every file in the configured directory, applies the ones written for
 * this image, and reports what happened on the console. Never fatal: a list
 * that cannot be read or does not match is a message, not a reason to stop a
 * boot that would otherwise work
 */
void usPatchApplyLists(UsSession *session, UsImage *image);

#endif
