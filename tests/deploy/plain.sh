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
# A reader may stop before the end, and that is not a failure: the checks run
# `plain.sh LOG | grep -q PATTERN`, which has its answer the moment PATTERN is
# seen, and grep exits there. What is left of the log then has nowhere to go,
# the producer is killed by SIGPIPE, and pipefail reports the pipeline as
# failed - for having answered the question. It takes a log long enough that
# the reader is still being written to when it stops, which is why this went
# unnoticed until the driver carried enough text to write for longer than a
# pipe holds. So the one status that means the reader went away is not passed
# on, and every other one is
set -uo pipefail

# SGR sequences are the only escapes the log carries; anything else a terminal
# would read is left alone
rc=0
sed -e 's/\x1b\[[0-9;]*m//g' -e 's/\r$//' "$@" || rc=$?

# 141 is 128 + SIGPIPE
[ "$rc" -eq 0 ] || [ "$rc" -eq 141 ] || exit "$rc"
exit 0
