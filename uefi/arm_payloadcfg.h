/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The payload's configuration block, written item by item as each becomes
 * known
 *
 * The descriptor base is known once the kernel's image has been located, the
 * user-mode decision follows from that base, and the driver hook's RVAs once
 * the call to divert has been found. Nothing here writes at the handover: the
 * values are RVAs for the payload to act on, and the payload is the side that
 * knows where the kernel ended up
 */

#ifndef US_UEFI_ARM_PAYLOADCFG_H
#define US_UEFI_ARM_PAYLOADCFG_H

#include <stdbool.h>
#include <stdint.h>

#include "core/scan.h"
#include "uefi/session.h"

/*
 * Records where the kernel keeps the base of its own descriptor mapping, and
 * answers the RVA a stub record should carry: the located one, the configured
 * fallback, or zero when neither the image nor the configuration named one
 */
uint32_t usArmConfigDescriptorBase(UsSession *session, const UsPteBaseSite *site);

/*
 * Records whether an instruction EL0 executed may be replaced where it stands
 *
 * A refusal here is not a fault: the exception path carries out the load
 * instead, which is correct and slower
 */
void usArmConfigEl0InPlace(UsSession *session, uint32_t descriptorBaseRVA);

/*
 * Records the kernel's driver load call, where its thunk goes and what the call
 * was going to before it was diverted
 */
void usArmConfigDriverHook(UsSession *session, const UsDriverLoadHook *hook);

#endif
