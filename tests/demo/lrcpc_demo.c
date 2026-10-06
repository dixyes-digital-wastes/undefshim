/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * LDAPR in a Windows ARM64 process: every width, aligned and not, from one
 * thread and from several
 *
 * Without FEAT_LRCPC each ldapr* is an undefined instruction. On a machine
 * without the feature this only reaches the end if the shim carried every
 * trap out, which is the point of the program
 */

#include "win_common.h"

#define ARENA_BYTES 64
#define THREAD_COUNT 4
#define THREAD_ROUNDS 512

/* Sixteen, so that every width below finds both the offsets it is aligned for
 * and the offsets it is not. The unaligned ones are not a curiosity: real code
 * has them, and the shim carries them out by a different sequence */
static _Alignas(16) volatile u8 arena[ARENA_BYTES];

static u32 loads;
static u32 failures;

/* Reset between groups, so a group that swept nothing reports zero rather
 * than the previous group's count */
static void startGroup(void) {
    loads = 0;
}

static u32 ldaprW(const volatile void *p) {
    u32 v;
    __asm__ __volatile__("ldapr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static u64 ldaprX(const volatile void *p) {
    u64 v;
    __asm__ __volatile__("ldapr %x0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static u8 ldaprB(const volatile void *p) {
    u8 v;
    __asm__ __volatile__("ldaprb %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static u16 ldaprH(const volatile void *p) {
    u16 v;
    __asm__ __volatile__("ldaprh %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static u64 loadWidth(u32 width, u32 off) {
    switch (width) {
    case 1:
        return ldaprB(arena + off);
    case 2:
        return ldaprH(arena + off);
    case 4:
        return ldaprW(arena + off);
    default:
        return ldaprX(arena + off);
    }
}

/* The same bytes read one at a time, which is a plain load wherever the
 * wider one would be an undefined instruction */
static u64 wantWidth(u32 width, u32 off) {
    u64 want = 0;
    for (u32 i = 0; i < width; i++) {
        want |= (u64)arena[off + i] << (8 * i);
    }
    return want;
}

static void noteWidth(const char *what, u32 off, u64 got, u64 want) {
    conWriteZ("    ");
    conWriteZ(what);
    conWriteZ(" off ");
    conHex8((u8)off);
    conWriteZ(" got ");
    conHex64(got);
    conWriteZ(" want ");
    conHex64(want);
    conWriteZ("\n");
}

/* Sweep one width over the arena, taking offsets of the wanted alignment and
 * leaving the rest */
static u32 sweep(const char *what, u32 width, bool aligned) {
    u32 bad = 0;
    for (u32 off = 0; off + width <= ARENA_BYTES; off++) {
        if (((off % width) == 0) != aligned) {
            continue;
        }
        loads++;
        u64 got = loadWidth(width, off);
        u64 want = wantWidth(width, off);
        if (got != want) {
            bad++;
            noteWidth(what, off, got, want);
        }
    }
    failures += bad;
    return bad;
}

/* processors is zero for the single-threaded groups, and says how many the
 * threaded one was seen on when it is not */
static void report(const char *name, u32 bad, u32 count, u32 processors) {
    conWriteZ(bad ? "  FAIL " : "  PASS ");
    conWriteZ(name);
    conWriteZ(": ");
    conHex32(count);
    conWriteZ(" loads");
    if (processors != 0) {
        conWriteZ(" on ");
        conHex32(processors);
        conWriteZ(" processors");
    }
    conWriteZ("\n");
}

typedef struct ThreadCtx_t {
    volatile u32 cpu;
    volatile u32 bad;
    volatile u32 loads;
} ThreadCtx;

static u32 __stdcall threadMain(void *parameter) {
    ThreadCtx *ctx = parameter;
    u32 bad = 0;
    u32 count = 0;
    ctx->cpu = GetCurrentProcessorNumber();

    for (u32 round = 0; round < THREAD_ROUNDS; round++) {
        for (u32 off = 0; off + 8 <= ARENA_BYTES; off += 8) {
            if (loadWidth(8, off) != wantWidth(8, off)) {
                bad++;
            }
            count++;
        }

        /* The misaligned word, at a different offset every round */
        u32 off = 1 + (round % 3);
        if (loadWidth(4, off) != wantWidth(4, off)) {
            bad++;
        }
        count++;
    }

    ctx->loads = count;
    ctx->bad = bad;
    return 0;
}

/* Hold every processor at once, so the shim has to keep its per-processor
 * state straight while they all trap */
static u32 runThreads(u32 *cpus, u32 *count) {
    ThreadCtx ctx[THREAD_COUNT];
    HANDLE threads[THREAD_COUNT];
    u32 started = 0;
    u32 bad = 0;

    for (u32 i = 0; i < THREAD_COUNT; i++) {
        ctx[i] = (ThreadCtx){ .cpu = 0, .bad = 0, .loads = 0 };
        threads[i] = CreateThread(NULL, 0, threadMain, &ctx[i], 0, NULL);
        started += threads[i] != NULL;
    }

    for (u32 i = 0; i < THREAD_COUNT; i++) {
        if (threads[i] != NULL) {
            WaitForSingleObject(threads[i], INFINITE);
        }
    }

    /* A thread that never started does no loads, and a group that checked
     * nothing is not a group that passed */
    if (started != THREAD_COUNT) {
        conWriteZ("    only ");
        conHex32(started);
        conWriteZ(" of ");
        conHex32((u32)THREAD_COUNT);
        conWriteZ(" threads started\n");
        bad++;
    }

    *cpus = 0;
    *count = 0;
    for (u32 i = 0; i < THREAD_COUNT; i++) {
        bad += ctx[i].bad;
        *count += ctx[i].loads;
        u32 seen = 0;
        for (u32 j = 0; j < i; j++) {
            if (ctx[j].cpu == ctx[i].cpu) {
                seen = 1;
                break;
            }
        }
        *cpus += !seen;
    }

    if (bad != 0) {
        conWriteZ("    ");
        conHex32(bad);
        conWriteZ(" wrong results across ");
        conHex32((u32)THREAD_COUNT);
        conWriteZ(" threads\n");
    }
    return bad;
}

/* The value that says the arena really is the bytes the demos think: every
 * byte distinct, so a wrong offset or a wrong width lands on a different byte */
static void fillArena(void) {
    for (u32 i = 0; i < ARENA_BYTES; i++) {
        arena[i] = (u8)(0x10 + i);
    }
}

#define PAGE_BYTES 4096u

/* ldapr w0, [x0]: the four byte form, no register named beyond the two the
 * encoding always has */
#define LDAPR_W0_X0 0xB8BFC000u

/*
 * A site in the last four bytes of a mapped page, with the page after it gone
 *
 * Reading a site's instruction is how the shim decides what it is looking at,
 * and four byte aligned is all an instruction has to be. A read of eight bytes
 * from one four bytes from the end of a page reaches into the page that is not
 * there; the fault that comes back names that page rather than the site, and
 * a handler that compares the two does not recognise its own read. On ARM64 a
 * fault inside a handler is fatal to the machine, so this is the difference
 * between a wrong answer here and a machine that never prints again
 *
 * The thread it runs on always ends with an access violation, whether or not
 * the shim is there: the load reads four aligned bytes and is fine, but the
 * instruction after the site is in the page that is gone, so nothing can run
 * past it. That is not the question being asked - the question is whether the
 * machine is still here to report it
 */
static u32 __stdcall edgeThread(void *parameter) {
    u8 *pages = VirtualAlloc(NULL, 2 * PAGE_BYTES, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    volatile u32 word = 0x11223344u;
    u32 (*fn)(const volatile u32 *);

    (void)parameter;
    if (pages == NULL) {
        return 0;
    }
    /* Written through the data mapping and flushed, because nothing makes the
     * fetch see it otherwise */
    *(volatile u32 *)(pages + PAGE_BYTES - 4) = LDAPR_W0_X0;
    FlushInstructionCache(CURRENT_PROCESS, pages, 2 * PAGE_BYTES);

    /* Decommitted rather than released, so this page goes and the reservation
     * it came from stays */
    VirtualFree(pages + PAGE_BYTES, PAGE_BYTES, MEM_DECOMMIT);

    fn = (u32(*)(const volatile u32 *))(void *)(pages + PAGE_BYTES - 4);
    (void)fn(&word);
    return 0;
}

/* The one group that cannot be reported like the others, because the way it
 * would end in a wrong result is the machine going away rather than a number
 * coming back wrong. Reaching the line after the wait is the whole test */
static void reportEdge(void) {
    HANDLE thread = CreateThread(NULL, 0, edgeThread, NULL, 0, NULL);
    DWORD code = 0;

    if (thread == NULL) {
        conWriteZ("  FAIL page edge: no thread\n");
        failures++;
        return;
    }
    WaitForSingleObject(thread, INFINITE);
    if (!GetExitCodeThread(thread, &code)
        || code != EXCEPTION_ACCESS_VIOLATION) {
        conWriteZ("  FAIL page edge: the thread ended with ");
        conHex32(code);
        conWriteZ(" rather than an access violation\n");
        failures++;
        return;
    }
    conWriteZ("  PASS page edge: a site four bytes from the end of a page\n");
}

int demoMain(void) {
    conWriteZ("ldapr: the four widths, aligned, unaligned and threaded\n");
    fillArena();

    startGroup();
    u32 bad = sweep("b", 1, true);
    bad += sweep("h", 2, true);
    bad += sweep("w", 4, true);
    bad += sweep("x", 8, true);
    report("aligned", bad, loads, 0);

    startGroup();
    bad = sweep("h", 2, false);
    bad += sweep("w", 4, false);
    bad += sweep("x", 8, false);
    report("unaligned", bad, loads, 0);

    u32 cpus = 0;
    u32 count = 0;
    bad = runThreads(&cpus, &count);
    report("threads", bad, count, cpus);

    reportEdge();

    if (failures != 0) {
        conWriteZ("FAIL: ldapr result mismatch\n");
        return 1;
    }
    conWriteZ("PASS: every ldapr result matches\n");
    return 0;
}
