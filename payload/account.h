/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What is written down about an exception, in the pool the host reads
 *
 * None of this decides anything: the entry, the hand-back and the halt are
 * answered elsewhere, and what is here is what those answers are later read
 * out of. Nothing here may fault either, which is why every one of them
 * starts by asking whether there is a pool at all
 */

#ifndef US_PAYLOAD_ACCOUNT_H
#define US_PAYLOAD_ACCOUNT_H

#include <stdint.h>

#include "common/layout.h"
#include "payload/payload.h"

/* What arrived, recorded before the handler does anything that can fault */
void usTrapAccountEnter(UsPoolTrapCPU *acc, UsFrame *frame, uint64_t cycle0);

/* What the entry cost, on the way out */
void usTrapAccountDone(UsPoolTrapCPU *acc, uint64_t cycle0);

/* The frame on the way out, at the same index the way in used */
void usRecordHandback(UsFrame *frame);

/* One record per trapping address, so that which sites are still costing an
 * exception can be read out afterwards */
void usStatsRecord(uint64_t va);

#endif
