/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The address change that moves everything we own
 *
 * The payload and the pool live in runtime services memory, because that is
 * the one class the OS keeps. Keeping it comes at a price that is easy to
 * miss: the OS is free to rebuild the address space and move that memory
 * somewhere else, and it says so exactly once, through
 * SetVirtualAddressMap. From that moment the old addresses are gone
 *
 * So the change has to be caught. UEFI raises one event for it, before the
 * switch takes effect, and everything pointing into runtime memory is handed
 * to ConvertPointer while it is still possible to translate. Missing this
 * does not fail here: it fails later, in whatever code next touches one of
 * those addresses, as a translation fault on an address nobody recognises
 *
 * This is the same problem for the pool and for the payload. Both are runtime
 * memory, both hold addresses of their own, and both have to be told
 *
 * Where the answers go, and why the code that produces them cannot live here:
 * see payload/vamap.h. The short version is that this file's own text is gone
 * by the time the notification runs, so only its setup can be here
 */

#ifndef US_VAMAP_H
#define US_VAMAP_H

#include <stdbool.h>

#include "uefi/session.h"

/*
 * Registers for the address change. Called once, while boot services are
 * still up; there is no way to register later, because the event fires during
 * the call that takes them away
 *
 * Returns false when the firmware refuses the event, which is not fatal on
 * its own: it means nothing we placed will survive the switch, so nothing
 * later may depend on it
 */
bool usVAMapArm(UsSession *session);

#endif
