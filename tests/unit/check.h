/*
 * How a check reports
 *
 * A run of these is read as one report, so every one of them says the same
 * things the same way: which check is talking, whether it passed, and -- when
 * it did not -- what it wanted and what it got
 *
 *   [PASS] test_thunk: 602 checks, 0 failures
 *   [FAIL] test_thunk: want 3 got 4
 *   [PASS] early: conversion, deferred ASLR publication, write checks passed
 *
 * The name is the test's own, defined before this is included, and it is what
 * a line is grepped by: fifteen of these print one after another and a line
 * without a name on it cannot be traced back to the one that failed
 *
 * Colour follows https://no-color.org, and is off unless it is asked for by a
 * terminal: NO_COLOR set to anything at all turns it off, and so does TERM
 * being dumb or the output not being a terminal. A log file and a pipe see no
 * escapes, which is what keeps the checks that grep this output working
 */

#ifndef US_CHECK_H
#define US_CHECK_H

#include <stdarg.h>

/*
 * The headers that are only needed to ask about the environment, and only
 * when it can be asked: the firmware headers declare names like getenv and
 * strcmp against their own types and carry their own FILE, so a test that
 * includes them cannot include these as well. Those two report plainly and
 * the rest colour their lines; a check is not the place to be clever about
 * which, and none of them is read by anything that cares
 */
#ifdef _UEFI_H_
#define US_CHECK_PLAIN 1
#endif

#ifndef US_CHECK_PLAIN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#endif

#ifndef US_CHECK_NAME
#error "define US_CHECK_NAME before including check.h"
#endif

/*
 * Where the report goes
 *
 * Standard output, unless the program's own output is what a caller reads.
 * planprobe is the one like that: its plan goes to standard output and is
 * compared byte for byte against the plan the target produced, so its report
 * would be part of what is compared. Define US_CHECK_OUT before including
 * this to send the report somewhere else
 *
 * A test that includes the firmware headers reaches the report the other way.
 * There stdout is a macro over the firmware table -- (FILE*)ST->... -- so
 * naming it would make the test refer to a symbol it does not define, and the
 * stream it wants is the one behind the firmware's own printf anyway
 */
#ifdef US_CHECK_PLAIN
#define usCheckOut(...) printf(__VA_ARGS__)
#else
#ifndef US_CHECK_OUT
#define US_CHECK_OUT stdout
#endif
#define usCheckOut(...) fprintf(US_CHECK_OUT, __VA_ARGS__)
#endif

#define US_CHECK_GREEN "\033[32m"
#define US_CHECK_RED "\033[31m"
#define US_CHECK_YELLOW "\033[33m"
#define US_CHECK_BOLD "\033[1m"
#define US_CHECK_RESET "\033[0m"

#ifdef US_CHECK_PLAIN
static inline int usCheckColour(void) {
    return 0;
}
#else
static inline int usCheckColour(void) {
    static int decided = -1;

    if (decided < 0) {
        const char *no = getenv("NO_COLOR");
        const char *term = getenv("TERM");

        decided = (no == NULL || no[0] == '\0')
                  && (term == NULL || strcmp(term, "dumb") != 0)
                  && isatty(fileno(US_CHECK_OUT));
    }
    return decided;
}
#endif

/* An escape, or nothing at all when colour is off, so a line can be written
 * once rather than twice */
static inline const char *usCheckEscape(const char *escape) {
    return usCheckColour() ? escape : "";
}

static inline const char *usCheckMark(int failed) {
    return failed ? "[FAIL]" : "[PASS]";
}

static inline const char *usCheckMarkColour(int failed) {
    return usCheckEscape(failed ? US_CHECK_RED : US_CHECK_GREEN);
}

/* One failing assertion, named the way every other line is */
static inline void usCheckFail(const char *fmt, ...) {
    va_list args;

    usCheckOut("%s[FAIL]%s " US_CHECK_NAME ": ",
               usCheckEscape(US_CHECK_RED), usCheckEscape(US_CHECK_RESET));
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* Context for a failure that is worth saying either way */
static inline void usCheckNote(const char *fmt, ...) {
    va_list args;

    printf("      " US_CHECK_NAME ": ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* The last line of a test that counts: what it did, and whether it passed.
 * The return is the exit status the caller wants */
static inline int usCheckSummary(long checks, long failures) {
    usCheckOut("%s%s%s " US_CHECK_NAME ": %s%ld%s checks, %ld failure%s\n",
            usCheckMarkColour(failures != 0), usCheckMark(failures != 0),
               usCheckEscape(US_CHECK_RESET), usCheckEscape(US_CHECK_BOLD),
               checks, usCheckEscape(US_CHECK_RESET), failures,
               failures == 1 ? "" : "s");
    return failures != 0;
}

/* The last line of a test that asserts instead of counting: reaching it means
 * nothing aborted, and the words say what was put through */
static inline int usCheckPassed(const char *what) {
    usCheckOut("%s[PASS]%s " US_CHECK_NAME ": %s\n",
               usCheckEscape(US_CHECK_GREEN), usCheckEscape(US_CHECK_RESET), what);
    return 0;
}

#endif
