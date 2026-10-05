/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The string and memory routines, see us_mem.h
 */

#include "payload/us_mem.h"

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n-- != 0) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;

    /* Overlapping and moving up means copying backwards, or the source is
     * overwritten before it is read */
    if (d > s && d < s + n) {
        d += n;
        s += n;
        while (n-- != 0) {
            *--d = *--s;
        }
        return dst;
    }
    return memcpy(dst, src, n);
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;

    while (n-- != 0) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a;
    const uint8_t *y = b;

    while (n-- != 0) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;

    while (*p != '\0') {
        p++;
    }
    return (size_t)(p - s);
}
