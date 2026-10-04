/*
 * The string and memory routines the compiler is allowed to emit calls to
 *
 * These are written by hand rather than taken from a libc because the payload
 * is linked with no libraries at all and must not depend on one being there.
 * Byte at a time, and no attempt at speed: they run on an exception path,
 * where being predictable matters more than being fast
 */

#ifndef US_MEM_H
#define US_MEM_H

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
