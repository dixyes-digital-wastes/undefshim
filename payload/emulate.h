/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Carrying out the RCpc load an exception was taken on
 *
 * The entry point the dispatch calls is declared here; everything else this
 * file does with the frame, the load tables and the pool stays in it
 */

#ifndef US_PAYLOAD_EMULATE_H
#define US_PAYLOAD_EMULATE_H

#include "payload/payload.h"

/*
 * Performs the instruction the frame was trapped on
 *
 * Returns false when it is not one of ours, in which case nothing has been
 * changed and the caller has to answer for the exception the same way it
 * would have without us
 */
bool usPayloadEmulate(UsFrame *frame, int cpu);

#endif
