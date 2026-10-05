/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Something that is not an LDAPR, to show the shim does not swallow it
 *
 * Without the shim this process dies with STATUS_ILLEGAL_INSTRUCTION; with it
 * it must die the same way. "returned" on the screen means the shim took a
 * trap that was never its business
 */

#include "win_common.h"

int demoMain(void) {
    conWriteZ("udf: about to execute udf #0x1234\n");
    __asm__ __volatile__("udf #0x1234" ::: "memory");
    conWriteZ("udf: returned (UNEXPECTED)\n");
    return 1;
}
