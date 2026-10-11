/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The probe bank, and telling the answer to a probe from a fault
 *
 * The four calls the rest of the payload makes are part of the external
 * contract and payload.h declares them; what is here as well is the question
 * the nested entry asks, which is whether the fault it is holding belongs to
 * an access a handler asked about
 */

#ifndef US_PAYLOAD_PROBE_H
#define US_PAYLOAD_PROBE_H

#include <stdint.h>

/*
 * Whether a fault on this address is the answer to an access the handler
 * asked about, and to take that answer: the arm is disarmed and says the
 * access did not happen
 *
 * The caller reads this instead of the fault and carries on at the
 * instruction after the one that faulted, which is the whole recovery
 */
bool usPayloadProbeAnswered(int cpu, uint64_t far);

#endif
