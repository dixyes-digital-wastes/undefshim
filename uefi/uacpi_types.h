/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The types uACPI is built on
 *
 * It expects either the standard headers to be there or this file to be, and
 * it is this file because a freestanding target has no stdarg and no
 * stdbool: what it wants is the four widths, a char and a boolean, and those
 * are the same everywhere
 */

#ifndef US_UACPI_TYPES_H
#define US_UACPI_TYPES_H

#include <uefi.h>

typedef uint8_t uacpi_u8;
typedef uint16_t uacpi_u16;
typedef uint32_t uacpi_u32;
typedef uint64_t uacpi_u64;

typedef int8_t uacpi_i8;
typedef int16_t uacpi_i16;
typedef int32_t uacpi_i32;
typedef int64_t uacpi_i64;

typedef uint8_t uacpi_bool;
#define UACPI_TRUE 1
#define UACPI_FALSE 0

#define UACPI_NULL NULL

typedef uintptr_t uacpi_uintptr;
typedef uacpi_uintptr uacpi_virt_addr;
typedef size_t uacpi_size;

typedef va_list uacpi_va_list;
#define uacpi_va_start va_start
#define uacpi_va_end va_end
#define uacpi_va_arg va_arg

typedef char uacpi_char;

#define uacpi_offsetof offsetof

/* A 64 bit number has no standard way to be formatted, so uACPI states the
 * one it will use rather than looking it up in inttypes */
#define UACPI_PRIu64 "llu"
#define UACPI_PRIx64 "llx"
#define UACPI_PRIX64 "llX"
#define UACPI_FMT64(val) ((unsigned long long)(val))

#endif
