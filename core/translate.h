/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 */

#ifndef US_TRANSLATE_H
#define US_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Query the EL1&0 translation of an address without dereferencing it,
 * preserving PAR_EL1. This is the answer the kernel will get for the address,
 * at any exception level
 */
bool usTranslateAddress(uint64_t va, bool write, uint64_t *pa);

/*
 * Query the translation the current exception level actually uses for its own
 * loads and stores
 *
 * At EL1 this is the same question as usTranslateAddress. At the boot, which
 * runs at EL2 with VHE off, it is a different regime: one that maps physical
 * memory to itself, where the EL1&0 tables have already been rebuilt for the
 * kernel and no longer answer for the loader's addresses
 */
bool usTranslateOwnAddress(uint64_t va, bool write, uint64_t *pa);

#endif
