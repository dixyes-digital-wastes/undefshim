#!/usr/bin/env bash
#
# Writes a generated file only when its contents differ.
#
# A rule whose recipe always rewrites its target makes the target look new to
# make, and everything that depends on the target is then rebuilt for a change
# that did not happen. Here that is expensive in a way that is easy to miss:
# the payload's header - the blob's bytes and the offsets into it - is a
# prerequisite of every driver object, so relinking the payload, which happens
# whenever any file in it changes, would rewrite a header whose bytes are the
# same and the whole driver would be compiled again.
#
# Usage: write_if_different.sh TARGET [COMMAND [ARG...]]
#
# With no COMMAND, TARGET is what the caller has already written to, which is
# how a generator that takes its output as an argument is called. With one,
# COMMAND runs with its output redirected into TARGET, which is how a shell
# recipe is called. Either way the file is replaced only when it differs, so an
# unchanged target keeps its timestamp and the build stops there
set -uo pipefail

target="${1:-}"
shift || true

if [ -z "$target" ]; then
    echo "usage: $(basename "$0") TARGET [COMMAND [ARG...]]" >&2
    exit 2
fi

# The caller's own temporary, named after the target and left for it to write:
# a generator that takes its output as an argument cannot be given a file
# descriptor, so the two forms are one by the caller passing $@.new
tmp="${target}.new"

if [ "$#" -gt 0 ]; then
    if ! "$@" >"$tmp"; then
        rm -f "$tmp"
        exit 1
    fi
    if [ ! -f "$tmp" ]; then
        echo "$(basename "$0"): $* wrote no output" >&2
        exit 1
    fi
fi

if [ ! -f "$tmp" ]; then
    echo "$(basename "$0"): no $tmp to install" >&2
    exit 1
fi

if [ -f "$target" ] && cmp -s "$tmp" "$target"; then
    rm -f "$tmp"
    exit 0
fi
mv "$tmp" "$target"
echo "generated $target"
