#!/usr/bin/env bash
#
# How a check reports, the same way tests/unit/check.h does for the host ones.
#
#   [PASS] driver_boot: the driver brings itself up
#   [FAIL] payload_case: the placement report is not parseable
#   [SKIP] boot_hook: WIN_DISK is not set
#       driver_boot: context for a failure that is worth printing anyway
#
# The name is what a line is grepped by: nine of these run one after another
# and a line without a name on it cannot be traced back to the one that failed.
# A check sets it before sourcing this, and a script it calls inherits it, so
# a run of scripts reports under the name of the check that started them.
#
# Colour follows https://no-color.org: NO_COLOR, set to anything at all, turns
# it off, and so does TERM being dumb or the output not being a terminal. A log
# file and a pipe see no escapes.
#
# Sourced, not run:
#     CHECK="driver_boot"
#     . "$(dirname "$0")/check.sh"
#
# A check with something of its own to say about a failure wraps checkFail in
# a function of its own -- printing the log that says why first -- so that the
# call sites stay a plain "|| fail \"what went wrong\""
#
: "${CHECK:=run}"
# Exported so that a script the check calls reports under the check's name
# rather than its own: a run of scripts is one check, and a line that says
# screen_case when the check was config_cases cannot be traced back
export CHECK

# What a check that skipped exits with. It is the usual code for "nothing was
# asked of me", and it is not zero because a caller that runs several checks
# has to be able to tell a skipped one from a passed one -- and not 1, because
# it is not a failure
: "${US_SKIP:=77}"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != dumb ]; then
    US_GREEN=$'\033[32m'
    US_RED=$'\033[31m'
    US_YELLOW=$'\033[33m'
    US_BOLD=$'\033[1m'
    US_RESET=$'\033[0m'
else
    US_GREEN=''; US_RED=''; US_YELLOW=''; US_BOLD=''; US_RESET=''
fi

# The verdict, and the check is over: anything that would have been said after
# it is context, and context goes through note, before it
checkPass() {
    printf '%s[PASS]%s %s: %s\n' "$US_GREEN" "$US_RESET" "$CHECK" "$1"
    exit 0
}

# One case of a set of them went right, and there are more to come
checkGood() {
    printf '%s[PASS]%s %s: %s\n' "$US_GREEN" "$US_RESET" "$CHECK" "$1"
}

# Ends the check, because every failure here is one the run cannot continue
# past: the caller is a chain of scripts and a status nobody looks at is a
# check that quietly passed. The reason it is worth saying comes before it,
# through note, since nothing after this line runs
checkFail() {
    printf '%s[FAIL]%s %s: %s\n' "$US_RED" "$US_RESET" "$CHECK" "$1" >&2
    exit 1
}

# The other way round: one case of a set of them went wrong. A check that is a
# set says so for each, because stopping at the first would hide the rest, and
# ends with checkFail once it has counted them
checkSoft() {
    printf '%s[FAIL]%s %s: %s\n' "$US_RED" "$US_RESET" "$CHECK" "$1" >&2
}

checkSkip() {
    printf '%s[SKIP]%s %s: %s\n' "$US_YELLOW" "$US_RESET" "$CHECK" "$1"
    exit "$US_SKIP"
}

checkNote() {
    printf '      %s: %s\n' "$CHECK" "$1"
}

# Also usable as a program, for a caller with one line to say and no script of
# its own -- a Makefile that skipped a check, say:
#
#     tests/deploy/check.sh skip peprobe "no CORPUS given"
#
# The code it exits with is the same one a check that skipped exits with.
#
# Run rather than sourced is what decides which of the two this is, and it has
# to be asked that way: a sourced script sees the arguments of the script that
# sourced it, and a check takes arguments of its own -- so counting them here
# would have read "screen_case.sh <image> <port> <text>" as a report
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    if [ "$#" -lt 3 ]; then
        printf 'usage: check.sh <pass|fail|skip|note> <name> <message>\n' >&2
        exit 2
    fi
    CHECK="$2"
    case "$1" in
    pass) checkPass "$3" ;;
    fail) checkFail "$3" ;;
    skip) checkSkip "$3" ;;
    note) checkNote "$3" ;;
    *) printf 'not a status: %s\n' "$1" >&2; exit 2 ;;
    esac
fi
