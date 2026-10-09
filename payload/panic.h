/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Stopping on purpose, and saying why
 *
 * A machine that cannot go on has three ways to end and only one of them is
 * worth having. It can carry on into the next fault, which is a stop whose
 * cause is somewhere else entirely; it can branch to nowhere, which is a stop
 * with nothing recorded; or it can stop here, having said what it knew. This
 * is the third one
 *
 * What it is for is the class of failure where the mechanism itself is the
 * thing that is broken: there is no guest to report those to, and by the time
 * they happen the console the boot used is not reachable
 *
 * The report is short on purpose. A panic is read off a photograph of a screen
 * or out of a memory dump, both of which are lossy, so what it says has to fit
 * and has to be the things that identify the failure rather than everything
 * that was true at the time
 */

#ifndef US_PANIC_H
#define US_PANIC_H

#include <stdint.h>

/*
 * What went wrong, as a number
 *
 * The number is what a note or a script matches on, so it is part of the
 * interface and not an implementation detail. Zero is "nothing went wrong",
 * which is what the record holds on a machine that is still running
 */
#define US_PANIC_NONE 0U
#define US_PANIC_INJECTED 0xF000U /* a stop the configuration asked for */

/*
 * The shape of the report: how wide its widest line can be, and how long it is
 *
 * The report is a table, so its width is a column of names, the mark between
 * them and their values, and a column of values; its length is the heading, a
 * line for each field, and the line that says it ended. Both are claims about
 * the text that is written where they are used, and they are here because the
 * face is sized from them and the face is not the side that writes it
 *
 * The longest value is a 64 bit address, which is what the field column is the
 * width of: eighteen characters of 0x and sixteen digits
 */
#define US_PANIC_FIELD_NAME 8U   /* a name is padded to this many characters */
#define US_PANIC_FIELD_VALUE 18U /* 0x and sixteen digits, which is the most */
#define US_PANIC_REPORT_COLUMNS (US_PANIC_FIELD_NAME + 1U + US_PANIC_FIELD_VALUE)
#define US_PANIC_REPORT_LINES 11U

/*
 * Stops the machine, having said what it knew
 *
 * Never returns, and that is the interface rather than a detail: every caller
 * is a path that has just decided it cannot continue, so there is no state to
 * give back and nothing to return to
 *
 * The code is one of the above, what is a short name for the site, and detail
 * is whatever the site has that identifies the failure. Nothing here may fail:
 * a report that faults on this path is a stop with nothing at all
 */
_Noreturn void usPanicHalt(uint64_t code, const char *what, uint64_t detail);

/*
 * The injected stops, for checking that the reporting works
 *
 * A machine that stops where it is meant to is what says the reporting path is
 * alive, and there is no other way to ask: the report only exists on a path
 * that is only reached when something has already gone wrong. So the
 * configuration can name a point and these count towards it
 *
 * Both do nothing when the configuration asked for nothing, which is what a
 * count of zero means. They are called from the places they count, so what is
 * counted is what actually happened rather than what was supposed to
 */
void usPanicCountTrap(void);
void usPanicCountRewrite(void);

#endif
