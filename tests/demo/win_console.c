/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Freestanding Windows ARM64 console output and the PE entry point
 */

#include "win_common.h"

static u32 strLen(const char *s) {
    u32 n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

void conWrite(const char *s, u32 len) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &written, 0);
}

void conWriteZ(const char *s) {
    conWrite(s, strLen(s));
}

static void conHex(u64 v, u32 digits) {
    char buf[16];
    u32 i = 0;
    do {
        u32 n = (u32)(v & 0xF);
        buf[i++] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
        v >>= 4;
    } while (v != 0);
    while (i < digits) {
        buf[i++] = '0';
    }
    while (i != 0) {
        conWrite(&buf[--i], 1);
    }
}

void conHex64(u64 v) {
    conHex(v, 16);
}

void conHex32(u32 v) {
    conHex(v, 8);
}

void conHex16(u16 v) {
    conHex(v, 4);
}

void conHex8(u8 v) {
    conHex(v, 2);
}

/* The console subsystem translates bare "\n", so the demos emit it as they
 * would to a normal C stdout */
void mainCRTStartup(void) {
    ExitProcess((u32)demoMain());
}
