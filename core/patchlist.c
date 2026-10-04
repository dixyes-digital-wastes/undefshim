#include <stddef.h>

#include "core/patchlist.h"

typedef enum {
    TokenEnd = 0,
    TokenWord,      /* an identifier or a name, e.g. peFile, ntoskrnl */
    TokenNumber,    /* 0x followed by hexadecimal digits */
    TokenHexRun,    /* a run of hexadecimal digits, the width fields */
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *at;
    uint32_t length;
    uint32_t value;    /* TokenNumber */
} Token;

typedef struct {
    const char *text;
    uint32_t length;
    uint32_t at;
} Scanner;

static bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool isHex(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isAlnum(char c) {
    return isAlpha(c) || isDigit(c);
}

static int hexValue(char c) {
    if (isDigit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

static bool sameWord(const Token *token, const char *word) {
    uint32_t i = 0;

    for (; word[i] != '\0'; i++) {
        if (i >= token->length || token->at[i] != word[i]) {
            return false;
        }
    }
    return i == token->length;
}

/*
 * Comments run to the end of the line and, like whitespace, may stand between
 * any two tokens: nothing below looks at line structure.
 */
static Token next(Scanner *scan) {
    Token token = { TokenEnd, NULL, 0, 0 };

    for (;;) {
        while (scan->at < scan->length && isSpace(scan->text[scan->at])) {
            scan->at++;
        }
        if (scan->at >= scan->length) {
            return token;
        }
        if (scan->text[scan->at] == '#') {
            while (scan->at < scan->length && scan->text[scan->at] != '\n') {
                scan->at++;
            }
            continue;
        }
        break;
    }

    token.at = scan->text + scan->at;
    if (token.at[0] == '0' && scan->at + 1 < scan->length
        && (token.at[1] == 'x' || token.at[1] == 'X')) {
        uint32_t i = 2;

        token.kind = TokenNumber;
        token.value = 0;
        while (scan->at + i < scan->length && isHex(token.at[i]) && i < 10U) {
            token.value = (token.value << 4) | (uint32_t)hexValue(token.at[i]);
            i++;
        }
        if (i == 2U) {
            /* "0x" with nothing after it: refuse the whole file. */
            token.kind = TokenWord;
        }
        scan->at += i;
        token.length = i;
        return token;
    }

    uint32_t run = 0;
    while (scan->at + run < scan->length && isAlnum(token.at[run])) {
        run++;
    }
    if (run == 0) {
        /* A character the grammar has no use for. */
        token.kind = TokenWord;
        token.length = 1;
        scan->at++;
        return token;
    }
    token.length = run;
    scan->at += run;
    token.kind = TokenWord;
    if (run >= 2U) {
        bool hex = true;

        for (uint32_t i = 0; i < run; i++) {
            if (!isHex(token.at[i])) {
                hex = false;
                break;
            }
        }
        if (hex) {
            token.kind = TokenHexRun;
        }
    }
    return token;
}

/* How many bytes a run of hexadecimal digits describes, or 0 if it cannot. */
static uint32_t runBytes(const Token *token) {
    if (token->kind != TokenHexRun || (token->length % 2U) != 0
        || token->length == 0 || token->length > US_PATCH_MAX_WIDTH * 2U) {
        return 0;
    }
    return token->length / 2U;
}

static void readBytes(const Token *token, uint8_t *out, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        out[i] = (uint8_t)((hexValue(token->at[i * 2]) << 4)
                           | hexValue(token->at[i * 2 + 1]));
    }
}

static bool overlaps(const UsPatchSite *sites, uint32_t count, uint32_t rva,
                     uint32_t width) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t start = sites[i].rva;
        uint32_t end = start + sites[i].width;

        if (rva < end && start < rva + width) {
            return true;
        }
    }
    return false;
}

UsPatchStatus usPatchParse(const char *text, uint32_t length, UsPatchSite *sites,
                           uint32_t capacity, UsPatchFile *out) {
    Scanner scan = { text, length, 0 };
    Token token;
    bool haveVersion = false;
    bool haveHash = false;

    out->target = NULL;
    out->targetLength = 0;
    out->sites = 0;
    out->skipped = 0;
    out->errorAt = 0;

    /* The version has to be the first thing in the file. */
    token = next(&scan);
    if (token.kind != TokenWord || !sameWord(&token, "USPATCHV1")) {
        out->errorAt = (uint32_t)(token.at != NULL ? token.at - text : 0);
        return UsPatchUnsupported;
    }
    haveVersion = true;

    for (;;) {
        token = next(&scan);
        if (token.kind == TokenEnd) {
            break;
        }
        if (token.kind == TokenWord) {
            if (sameWord(&token, "peFile")) {
                Token name = next(&scan);

                if (name.kind != TokenWord && name.kind != TokenHexRun) {
                    out->errorAt = (uint32_t)(name.at != NULL ? name.at - text : scan.at);
                    return UsPatchSyntax;
                }
                out->target = name.at;
                out->targetLength = name.length;
                continue;
            }
            if (sameWord(&token, "textSHA256Hash")) {
                Token digits = next(&scan);

                if (digits.kind != TokenHexRun || digits.length != 64U) {
                    out->errorAt = (uint32_t)(digits.at != NULL ? digits.at - text : scan.at);
                    return UsPatchNoHash;
                }
                readBytes(&digits, out->hash, 32U);
                haveHash = true;
                continue;
            }
            /* An instruction from a newer format, or a typo: refuse it. */
            out->errorAt = (uint32_t)(token.at - text);
            return UsPatchSyntax;
        }
        if (token.kind != TokenNumber) {
            out->errorAt = (uint32_t)(token.at - text);
            return UsPatchSyntax;
        }

        /*
         * A site: the address, then up to three field runs, ending where the
         * next address or instruction begins.
         */
        UsPatchSite site;
        Token field[3];
        uint32_t fields = 0;
        uint32_t width = 0;
        bool shaped = true;

        site.rva = token.value;
        while (fields < 3U) {
            Token peek = next(&scan);

            if (peek.kind != TokenHexRun) {
                /* Not a field: put it back for the outer loop. */
                scan.at = (uint32_t)(peek.at != NULL ? peek.at - text : scan.at);
                break;
            }
            field[fields++] = peek;
        }
        if (fields == 0U || fields > 3U) {
            shaped = false;
        }
        for (uint32_t i = 0; i < fields && shaped; i++) {
            uint32_t bytes = runBytes(&field[i]);

            if (bytes == 0U || (i > 0U && bytes != width)) {
                shaped = false;
            }
            width = bytes;
        }
        if (shaped && (site.rva % width) != 0U) {
            shaped = false;
        }
        if (shaped && overlaps(sites, out->sites, site.rva, width)) {
            shaped = false;
        }
        if (!shaped) {
            out->skipped++;
            continue;
        }
        if (out->sites >= capacity) {
            return UsPatchTooManySites;
        }

        site.width = (uint8_t)width;
        for (uint32_t i = 0; i < width; i++) {
            site.match[i] = 0xFFU;
            site.mask[i] = 0xFFU;
        }
        if (fields == 1U) {
            readBytes(&field[0], site.replace, width);
        } else if (fields == 2U) {
            readBytes(&field[0], site.match, width);
            readBytes(&field[1], site.replace, width);
        } else {
            readBytes(&field[0], site.match, width);
            readBytes(&field[1], site.replace, width);
            readBytes(&field[2], site.mask, width);
        }
        sites[out->sites++] = site;
    }

    if (!haveVersion) {
        return UsPatchUnsupported;
    }
    if (out->target == NULL) {
        return UsPatchNoTarget;
    }
    /* Without the hash there is no way to know the list is for this build,
     * and a patch meant for another one is worse than no patch at all. */
    if (!haveHash) {
        return UsPatchNoHash;
    }
    out->status = UsPatchOk;
    return UsPatchOk;
}

bool usPatchMatches(const UsPatchSite *site, const uint8_t *memory) {
    for (uint32_t i = 0; i < site->width; i++) {
        if ((memory[i] & site->mask[i]) != (site->match[i] & site->mask[i])) {
            return false;
        }
    }
    return true;
}

void usPatchWrite(const UsPatchSite *site, uint8_t *memory) {
    for (uint32_t i = 0; i < site->width; i++) {
        memory[i] = site->replace[i];
    }
}
