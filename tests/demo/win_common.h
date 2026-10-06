/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Freestanding Windows ARM64 console helpers, for the demos
 *
 * They build with clang on Linux and link against nothing: kernel32 is
 * imported by name and the PE entry point is our own, so no C runtime and no
 * Windows SDK is involved
 */

#ifndef US_WIN_COMMON_H
#define US_WIN_COMMON_H

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef u32 DWORD;
typedef u64 size_t;
typedef void *HANDLE;

/* GetStdHandle pseudo-handle for the process console output */
#define STD_OUTPUT_HANDLE ((DWORD)-11)

/* WaitForSingleObject, no timeout */
#define INFINITE 0xFFFFFFFFu

#define NULL ((void *)0)

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) int __stdcall WriteFile(HANDLE hFile,
                                              const void *buffer,
                                              DWORD bytesToWrite,
                                              DWORD *bytesWritten,
                                              void *overlapped);
__declspec(dllimport) void __stdcall ExitProcess(u32 exitCode);

/*
 * Threads, for the demos that need more than one processor busy: the shim
 * keeps per-processor state, and a process that stays on one processor never
 * exercises it
 */
typedef u32(__stdcall *ThreadFn)(void *);
__declspec(dllimport) HANDLE __stdcall CreateThread(void *attributes,
                                                    size_t stackSize,
                                                    ThreadFn start,
                                                    void *parameter,
                                                    DWORD flags,
                                                    DWORD *threadId);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE handle,
                                                          DWORD milliseconds);
__declspec(dllimport) DWORD __stdcall GetCurrentProcessorNumber(void);

/*
 * Virtual memory, for the group that places an instruction at the very end of
 * a mapped page on purpose: it is only the site's own address that makes the
 * read of it interesting, and the page after it has to be gone for that
 */
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define PAGE_EXECUTE_READWRITE 0x40u

__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size,
                                                   DWORD type, DWORD protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size,
                                                DWORD type);
__declspec(dllimport) int __stdcall FlushInstructionCache(HANDLE process,
                                                          const void *address,
                                                          size_t size);

/* What a thread that ran into a fault it had no handler for ends with, which
 * is how the page edge group tells that ending from any other */
__declspec(dllimport) int __stdcall GetExitCodeThread(HANDLE thread,
                                                      DWORD *code);
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u

/* The pseudo-handle every process can pass to say "this one" */
#define CURRENT_PROCESS ((HANDLE)-1)

/* All of it is 8-bit text; there is no CRT and therefore no varargs */
void conWrite(const char *s, u32 len);
void conWriteZ(const char *s);
void conHex64(u64 v);
void conHex32(u32 v);
void conHex16(u16 v);
void conHex8(u8 v);

/* Provided by each demo; mainCRTStartup calls it and exits with its result */
int demoMain(void);

#endif
