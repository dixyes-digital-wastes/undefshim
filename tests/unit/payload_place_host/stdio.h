/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * toml.h includes stdio unconditionally. Use the driver's FILE declarations,
 * but leave assert.h and the other host headers untouched for this test
 */
#ifndef US_PLACEMENT_HOST_STDIO_H
#define US_PLACEMENT_HOST_STDIO_H
#include <uefi.h>
#endif
