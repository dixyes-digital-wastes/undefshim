/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Checks for the name a stop is asked for by
 *
 * The name comes from a configuration file, which hands back a pointer into
 * its own document and a length rather than a terminated string, so the two
 * sides of the comparison do not have the same shape. That is the interesting
 * part and it is why this exists: an off-by-one here either stops the boot at
 * a milestone nobody asked for or never stops it at all, and neither looks
 * like a bug in this function
 *
 * The other half is that the comparison is whole rather than by prefix. Four
 * of the milestones begin with "M6.5", so a prefix would stop at whichever
 * came first -- which is a machine that stopped somewhere other than where it
 * was told to, and looks exactly like the right behaviour until it is read
 *
 * The names themselves are read out of the boot's own sources rather than
 * listed here. A list is a copy that goes stale the first time a milestone is
 * added, and the check it would carry -- "no two of the boot's names match
 * each other" -- would then be made against names the boot no longer prints,
 * which passes and means nothing
 */

#include <stdio.h>
#include <string.h>

#include "uefi/panic.h"

#define US_CHECK_NAME "test_panic"
#include "check.h"

static int failures;
static int checks;

/*
 * The boot's files that print a milestone, seen from this test's own directory
 *
 * All of them rather than one: the names have to be unique across the boot and
 * not within a file, and nothing here can walk a directory to find them, so the
 * list is the paths as they are. A file that starts printing a milestone and is
 * not here is the one way this check can go quiet, and the floor the caller
 * puts on the count does not catch it -- a file added elsewhere would only add
 * names
 */
static const char *const kSources[] = {
    "../../uefi/driver.c",
    "../../uefi/loadimage_hook.c",
    "../../uefi/patch.c",
    "../../uefi/payload_place.c",
    "../../uefi/work.c",
    "../../uefi/stage.c",
};
#define SOURCE_COUNT (sizeof(kSources) / sizeof(kSources[0]))

/* Every distinct name handed to usConsoleMilestone, up to these many */
#define MAX_NAMES 32
#define MAX_NAME 48

static char names[MAX_NAMES][MAX_NAME];
static int nameCount;

/* Whether a name is already in the set, so that a milestone printed from two
 * places counts once: what has to be unique is the set of names, not the set
 * of calls */
static int haveName(const char *name, size_t len) {
    for (int i = 0; i < nameCount; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], name, len) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * The names, taken out of the calls that print them
 *
 * Only the string literals on a line that calls the milestone printer, which
 * is what makes this a reading of the code rather than a reading of any string
 * that happens to be in the file. A call whose arguments are spelled out over
 * several lines would be missed, and the floor the caller puts on the count is
 * what catches that rather than a silent short list
 *
 * The count is the whole set so far and not what this file added, so callers
 * ask once at the end rather than adding up what each call returned
 */
static int readNames(const char *path) {
    FILE *f = fopen(path, "r");
    char line[512];

    if (f == NULL) {
        return nameCount;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        const char *p;

        /* Two shapes say a milestone now: a call naming a stage, and the
         * table in uefi/stage.c that spells it. Both are read, because a name
         * reaches a machine through either */
        if (strstr(line, "usStageMilestone(") == NULL
            && strstr(line, "[UsStage") == NULL) {
            continue;
        }
        for (p = line; (p = strchr(p, '"')) != NULL; p++) {
            const char *end = strchr(++p, '"');
            size_t len;

            if (end == NULL) {
                break;
            }
            len = (size_t)(end - p);
            if (len == 0 || len >= MAX_NAME || nameCount >= MAX_NAMES
                || haveName(p, len)) {
                continue;
            }
            memcpy(names[nameCount], p, len);
            names[nameCount][len] = '\0';
            nameCount++;
            p = end;
        }
    }
    fclose(f);
    return nameCount;
}

/* The configuration's shape: a pointer and a length, with the byte after the
 * value being whatever the document has there rather than a terminator */
static void matchesText(const char *name, const char *want, int wantLen,
                        int expected) {
    int got = usPanicNameMatches(want, wantLen, name);

    checks++;
    if (got != expected) {
        failures++;
        usCheckFail("%-46s want %d got %d\n", name, expected, got);
    }
}

/*
 * Every name the boot prints, against every other
 *
 * Two things have to hold for a stop to be askable for by name: each name
 * matches itself, and no name matches a different one. The second is what a
 * prefix comparison would break and it is not visible by reading a single
 * name -- the four that begin "M6.5" are the case, and there are four of them
 */
static void testAgainstTheBoot(void) {
    int found;

    for (size_t s = 0; s < SOURCE_COUNT; s++) {
        (void)readNames(kSources[s]);
    }
    found = nameCount;

    checks++;
    if (found < 8) {
        failures++;
        usCheckFail("the boot's sources listed %d milestones, too few to be "
                    "all of them\n", found);
        return;
    }
    for (int i = 0; i < found; i++) {
        int hits = 0;

        for (int j = 0; j < found; j++) {
            if (usPanicNameMatches(names[i], (int)strlen(names[i]), names[j])) {
                hits++;
            }
        }
        checks++;
        if (hits != 1) {
            failures++;
            usCheckFail("\"%s\" matched %d of the boot's names\n", names[i],
                        hits);
        }
    }
}

int main(void) {
    const char *want = "M6.5 armed";
    int len = (int)strlen(want);

    /* The ordinary case */
    matchesText("M6.5 armed", want, len, 1);

    /* A milestone that begins the same way is not the one named. This is the
     * reason the comparison is whole: three others share this prefix */
    matchesText("M6.5 arm failed", want, len, 0);
    matchesText("M6.5 armed skipped", want, len, 0);
    matchesText("M6.5 transfer done", want, len, 0);
    matchesText("M6", want, len, 0);

    /* The name has to end where the milestone does: a prefix of it is not
     * it, and neither is a longer name that begins with all of it */
    matchesText("M6.5 arme", want, len, 0);
    matchesText("M6.5 armedx", want, len, 0);

    /* Nothing asked for, and nothing to compare against */
    matchesText("M2 done", NULL, 0, 0);
    matchesText("M2 done", want, 0, 0);
    matchesText(NULL, want, len, 0);

    /* An empty milestone only matches a name with an empty length */
    matchesText("", want, 0, 1);

    testAgainstTheBoot();

    return usCheckSummary(checks, failures);
}
