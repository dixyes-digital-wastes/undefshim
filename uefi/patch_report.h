/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What is said about a list that was not applied, and about the build an image
 * is
 *
 * A refusal is the one thing an operator has to be able to read off the log,
 * so the reason is written the way the list says what it is for: the build's
 * uuid in both spellings, because a debugger prints one of them and the image's
 * own record holds the other
 */

#ifndef US_UEFI_PATCH_REPORT_H
#define US_UEFI_PATCH_REPORT_H

#include "core/patchapply.h"
#include "core/patchlist.h"

/*
 * Why a list was refused. What a reader needs here is what the machine is, said
 * the way the list says what it is for
 */
void usPatchReportRefusal(const char *name, const UsPatchFile *file,
                          const UsPatchMatchers *matchers, const UsPatchStats *stats);

/*
 * The site a list could not use, at the level the count is worth reading: one
 * address is what tells a list written for another build from one the machine
 * has already had applied
 */
void usPatchReportSites(const UsPatchStats *stats);

#endif
