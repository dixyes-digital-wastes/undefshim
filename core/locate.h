/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The locators: the places a boot has to find before it can act
 *
 * They are declared apart from the scan primitives they are built on because
 * the two answer different questions. A scan reports where a byte string
 * occurs, and says nothing about whether finding it means anything. A locator
 * decides whether an image is one this project can act on, and its refusals
 * carry as much weight as its answers: a shape that matches twice is not a
 * place to patch, and a caller is told so rather than handed the first one
 *
 * What each locator accepts as a shape, and what the counts it reports mean,
 * are written with the type it answers with in core/scan.h. That header
 * includes this one, so a caller that wants a locator keeps including
 * core/scan.h and does not have to know which of the two a declaration is in
 */

#ifndef US_LOCATE_H
#define US_LOCATE_H

#include "core/scan.h"

/*
 * Locates the transfer leaf. Reports the count so the caller can refuse a
 * result that is not unique
 */
UsLeafSite usLocateTransferLeaf(UsImage *img);

UsHandoffSite usLocateTTBRHandoff(UsImage *img);

UsSpareSlot usLocateSpareSlot(UsImage *img, uint32_t minBytes);

UsDriverLoadHook usLocateDriverLoadHook(UsImage *img);

UsPteBaseSite usLocatePteBase(UsImage *img);

UsVBARTables usFindVBARTables(UsImage *img);

UsLDAPRCounts usCountLDAPR(UsImage *img);

/*
 * Whether a slot holds a branch, and if so where it goes
 *
 * A slot holding a branch can be taken over whatever the branch does, because
 * the stub that replaces it keeps the branch and only diverts the exceptions
 * this project exists for. A slot holding anything else cannot: those bytes
 * are the handler, and there is nothing to keep
 */
bool usVectorSlotBranch(UsImage *img, uint32_t tableRVA, UsVectorSlot slot,
                        int32_t *displacement);

#endif
