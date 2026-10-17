#!/usr/bin/env bash
# OS concept: A read-only shared-memory observer (seqlock copy) and process states via /proc — Units 2/3
#
# Tests build/reliefui (the ncurses dashboard) in --batch mode (frames drawn to stdout, no keyboard):
#   1. without reliefd: "ReliefOS not running - press s to start"
#   2. with reliefd and 8 camps running: the top bar (pid, policy, deadlock mode), the camps
#      with their process state, the scheduler levels, the Banker's resources and safe state,
#      the dock and the event log are all drawn; reliefctl ui runs the same binary
#   3. usage errors exit 2; without a terminal and without --batch it refuses (exit 2)
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind (errors make reliefui exit non-zero)
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
CTL="$ROOT/scripts/reliefctl"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN" RELIEF_TIME_SCALE=50 RELIEF_POLICY=mlfq
cleanup() {
    "$CTL" stop >/dev/null 2>&1 || true
    rm -rf "$RELIEF_HOME"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
wait_for() {
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
# One or more frames as plain text: cursor moves (ncurses skips runs of blanks with them) become
# a space, other escape sequences and the line-drawing switches are dropped.
frames() {
    "${WRAP[@]}" "$BIN/reliefui" "$@" |
        sed 's/\x1b\[[0-9;]*[HG]/ /g; s/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b[()][0-9A-B]//g; s/\x1b[=>]//g'
}
camps_running() { [[ $("${WRAP[@]}" "$BIN/reliefstat" 2>/dev/null | grep -c ' RUNNING ') -ge 8 ]]; }

for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    [[ -e "$f" ]] && { echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2; exit 1; }
done

# 1. Not running.
out=$(frames --batch 1) || fail "reliefui --batch 1 failed without reliefd"
grep -aq 'ReliefOS not running - press s to start' <<< "$out" || fail "no 'not running' screen: $out"
pass "without reliefd: 'ReliefOS not running - press s to start'"

# 2. Running.
"$CTL" start >/dev/null || fail "reliefctl start"
wait_for "8 camps RUNNING" camps_running
dpid=$(cat "$RELIEF_HOME/reliefd.pid")
wait_for "an event in the log" grep -q ' event=request ' "$RELIEF_HOME/logs/reliefd.log"
out=$(frames --batch 2) || fail "reliefui --batch 2 failed (exit status)"
for want in "reliefd $dpid running" "sched mlfq" "deadlock avoid" "Camps (processes)" "Hadapsar" "Wakad" \
            "RUNNING" "Scheduler (rescue teams = CPUs)" "Q0" "team 1" "Resources (Banker's) and loading dock" \
            "TRUCK" "FOOD_KIT" "/8" "Events (reliefd.log)" "request"; do
    grep -aqF "$want" <<< "$out" || fail "frame lacks '$want'"
done
grep -aqE 'state +(SAFE|UNSAFE)' <<< "$out" || fail "frame lacks the safe/unsafe state"
grep -aqE 'Hadapsar +[0-9]+ +[RSD] ' <<< "$out" || fail "camp row lacks a /proc state letter"
pass "running: top bar, 8 camps with /proc states, scheduler, Banker's resources + safe state, dock, events"
out=$("$CTL" ui --batch 1 | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g') || fail "reliefctl ui --batch 1"
grep -aqF "Camps (processes)" <<< "$out" || fail "reliefctl ui did not draw the dashboard"
pass "reliefctl ui runs the dashboard"
"$CTL" stop >/dev/null || fail "reliefctl stop"

# 3. Usage.
for bad in "--batch 0" "--bogus" "extra"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    "${WRAP[@]}" "$BIN/reliefui" $bad >/dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "reliefui $bad should exit 2, got $rc"
done
rc=0; "${WRAP[@]}" "$BIN/reliefui" < /dev/null > /dev/null 2>&1 || rc=$?
[[ $rc -eq 2 ]] || fail "reliefui without a terminal should exit 2, got $rc"
pass "usage errors exit 2; without a terminal (and no --batch) it refuses"

echo "all ui tests passed"
