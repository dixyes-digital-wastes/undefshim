/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The licences this binary carries, see licenses.h
 */

#include <uefi.h>

#include "uefi/console.h"
#include "uefi/licenses.h"

/*
 * The longest line in any of them is 79 characters, so this is the buffer a
 * line is assembled in. A line that did not fit would be split rather than
 * dropped, because a licence with a paragraph missing is not the licence
 */
#define US_LICENSE_LINE_BYTES 128

/*
 * Every text as it is in the tree, embedded at compile time
 *
 * The trailing zero is what makes each one a string: #embed brings the bytes
 * of a file and nothing else, and nothing else in a licence file could stand
 * in for a terminator
 */
static const char kAgpl[] = {
#embed "../LICENSE"
    , 0
};

static const char kUACPI[] = {
#embed "../third_party/uACPI/LICENSE"
    , 0
};

static const char kTLSF[] = {
#embed "../third_party/tlsf/LICENSE"
    , 0
};

static const char kPosixUEFI[] = {
#embed "../third_party/posix-uefi/LICENSE"
    , 0
};

static const char kTomlC[] = {
#embed "../third_party/toml-c/LICENSE"
    , 0
};

static const char kAtariSTFont[] = {
#embed "../third_party/atarist-font/LICENSE"
    , 0
};

/*
 * musl is not built into this driver. Its formatted output is what the UEFI
 * library's is derived from, and a derivation carries the notice of what it
 * came from
 */
static const char kMusl[] = {
#embed "../third_party/musl/COPYRIGHT"
    , 0
};

typedef struct UsLicense_t {
    const char *what;
    const char *text;
} UsLicense;

static const UsLicense kLicenses[] = {
    {
        .what = "undefshim: AGPL-3.0-or-later",
        .text = kAgpl,
    },
    {
        .what = "uACPI",
        .text = kUACPI,
    },
    {
        .what = "TLSF",
        .text = kTLSF,
    },
    {
        .what = "posix-uefi",
        .text = kPosixUEFI,
    },
    {
        .what = "toml-c",
        .text = kTomlC,
    },
    {
        .what = "atarist-font",
        .text = kAtariSTFont,
    },
    {
        .what = "musl, through posix-uefi",
        .text = kMusl,
    },
};

#define US_LICENSES (sizeof(kLicenses) / sizeof(kLicenses[0]))

void usLicensesReport(void) {
    for (uint32_t i = 0; i < US_LICENSES; i++) {
        const char *at = kLicenses[i].text;
        char line[US_LICENSE_LINE_BYTES];
        uint32_t used = 0;

        usLogI("license", "%s\n", kLicenses[i].what);

        for (;;) {
            char c = *at;

            if (c == '\n' || c == '\0' || used == sizeof(line) - 1U) {
                /* The zero at the end of the text is not a blank line, and
                 * an empty line in the middle of one is */
                if (used != 0 || c == '\n') {
                    line[used] = '\0';
                    usLogI("license", "%s\n", line);
                }
                used = 0;
                if (c == '\0') {
                    break;
                }
                if (c == '\n') {
                    at++;
                }
                /* Otherwise the line was longer than the buffer and what is
                 * left of it is the next one out, from the same byte */
                continue;
            }
            line[used++] = c;
            at++;
        }
    }
}
