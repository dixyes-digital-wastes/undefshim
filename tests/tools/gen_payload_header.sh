#!/usr/bin/env bash
#
# The header the driver reaches the payload through.
#
# The payload is a blob the driver embeds as bytes, so the two sides are not
# linked and the driver cannot name anything in it. What they agree on instead
# is this file: how many bytes there are, and where each entry point and record
# sits inside them. Both come out of the linked ELF rather than from the link
# script, so the payload's layout stays free to change.
#
# It is written through write_if_different.sh, because a header that is
# rewritten for nothing is a header that makes every driver object look out of
# date.
#
# Usage: gen_payload_header.sh PAYLOAD_BIN PAYLOAD_ELF OUT
#
# SYMBOLS in the environment is TAG=SYMBOL pairs, separated by spaces, and each
# one is emitted as US_PAYLOAD_<TAG>_OFFSET. A symbol that is not in the blob,
# or that lands at offset zero, is an error: an offset of zero is what a symbol
# the linker dropped looks like, and it is a plausible answer rather than an
# obvious one
set -uo pipefail

bin="${1:-}"
elf="${2:-}"
out="${3:-}"

if [ -z "$bin" ] || [ -z "$elf" ] || [ -z "$out" ]; then
    echo "usage: $(basename "$0") PAYLOAD_BIN PAYLOAD_ELF OUT" >&2
    exit 2
fi

bytes="$(stat -c %s "$bin")" || exit 1

{
    printf '/* Generated from %s. */\n' "$elf"
    printf '#define US_PAYLOAD_BYTES %s\n' "$bytes"
    for pair in ${SYMBOLS:-}; do
        name="${pair#*=}"
        tag="${pair%%=*}"
        off="$(llvm-nm "$elf" | awk -v w="$name" \
            '$3==w {v=strtonum("0x"$1)} $3=="usPayloadStart" {s=strtonum("0x"$1)} END{print v-s}')"
        if [ -z "$off" ]; then
            echo "ERROR: $name is not in the payload" >&2
            exit 1
        fi
        if [ "$off" = "0" ]; then
            echo "ERROR: $name is at offset zero, which means it was dropped" >&2
            exit 1
        fi
        printf '#define US_PAYLOAD_%s_OFFSET %s\n' "$tag" "$off"
    done
    printf '\nstatic const unsigned char kPayloadBlob[US_PAYLOAD_BYTES] = {\n'
    printf '#embed "payload.bin"\n'
    printf '};\n'
} >"$out" || exit 1

echo "payload header: $(grep -c '^#define' "$out") constants" >&2
