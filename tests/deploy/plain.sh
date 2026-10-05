#!/usr/bin/env bash
#
# The driver's log, as text.
#
# The driver colours a line by putting escape sequences around the tag and
# around the values in it, and a check that matches on a line has to see the
# line without them: the escapes sit between the tag and its text, so a
# pattern like "payload: at" is not in the raw bytes at all.
#
# With no arguments this filters standard input, so it can stand where a tr
# that only stripped carriage returns used to.
#
set -euo pipefail

# SGR sequences are the only escapes the log carries; anything else a
# terminal would read is left alone
sed -e 's/\x1b\[[0-9;]*m//g' -e 's/\r$//' "$@"
