/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The site list: what a boot has decided to act on, collected before anything
 * is written
 *
 * A site is a decision recorded rather than a patch applied, so that a place
 * found wrongly, or not found at all, is visible in a dump instead of as a
 * fault later. That is also why the list is kept apart from the locators that
 * fill it: which sites an image offers, and what a boot does about them, are
 * two subjects, and only the first one lives here
 *
 * The kinds a site can have and the list itself are in core/scan.h, which
 * includes this header, so a caller that wants either keeps including
 * core/scan.h
 */

#ifndef US_SITE_H
#define US_SITE_H

#include "core/scan.h"

void usSiteListInit(UsSiteList *list);

/* Appends one site. Returns false when the list is full, in which case the
 * count of seen sites still goes up */
bool usSiteAdd(UsSiteList *list, UsSiteKind kind, UsImageKind image, uint32_t rva,
               uint32_t auxiliary);

const char *usSiteKindName(UsSiteKind kind);

/*
 * Collects every site in one image. Locators that are not unique are not
 * reported as sites: a match that could be one of several places is not a
 * place to act on, and the count of what was seen is what says so
 */
size_t usCollectSites(UsSiteList *list, UsImage *img, UsImageKind kind);

/*
 * Sorts by image and then by RVA, so that the same image always produces the
 * same order. The order sites are found in depends on the section table, and
 * comparing two dumps is the whole point of having them
 */
void usSiteListSort(UsSiteList *list);

#endif
