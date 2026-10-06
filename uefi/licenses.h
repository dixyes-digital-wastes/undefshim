/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The licences this binary carries
 *
 * A driver is distributed as a binary and nothing else, so the notices that
 * have to go with it have nowhere to go but inside it. Each text is embedded
 * as bytes rather than kept beside the build, and the report is the way to
 * read them back out of a machine
 *
 * The texts are the files themselves, so they cannot drift from what the tree
 * says: a licence copied into a C string is a second copy, and the second copy
 * is the one that goes stale
 */

#ifndef US_LICENSES_H
#define US_LICENSES_H

/*
 * Writes every embedded licence to the console, one heading and one block of
 * text each. Whether it is called is the configuration's business, from
 * [log] showLicenses; a line of it is an ordinary line of the log
 */
void usLicensesReport(void);

#endif
