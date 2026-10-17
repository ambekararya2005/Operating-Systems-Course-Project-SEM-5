#!/usr/bin/env bash
# OS concept: Deadlock characterization, prevention, avoidance, detection and recovery — Unit 3
#
# scenarios/deadlock.csv (camps 1 and 2 each hold a truck and a boat, then ask for the other's;
# 2 trucks and 2 boats in scenarios/deadlock_resources.conf) under every --deadlock mode:
#   1. none: a real deadlock. Neither camp finishes within the timeout (both still waiting),
#      and reliefctl waitfor shows the cycle camp1 -> camp2 -> camp1 (text and DOT)
#   2. prevent: completes; out-of-order requests were rejected and retried in order
#   3. prevent --all-or-nothing: completes; each task was granted as one bundle
#   4. avoid: completes; some request waited as unsafe
#   5. detect: completes; the log has a deadlock_detected with the cycle and a recovery
#   6. scenarios/deadlock_mix.csv (random tasks, default resources) completes under
#      prevent, detect and avoid
#   7. bad --detect-interval / --all-or-nothing without prevent exit 2
# In every completed run each grant is released (or revoked by recovery) and nothing is left.
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for reliefd (and its camps), e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
CTL="$ROOT/scripts/reliefctl"
SCEN="$ROOT/scenarios/deadlock.csv"
RES="$ROOT/scenarios/deadlock_resources.conf"
MIX="$ROOT/scenarios/deadlock_mix.csv"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN"
mkdir -p "$RELIEF_HOME/logs"
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
DPID=""

cleanup() {
    if [[ -n "$DPID" ]] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    rm -rf "$RELIEF_HOME"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "--- reliefd stderr (tail) ---" >&2
    tail -n 30 "$OUT" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }
count() { grep -c -- "$1" "$LOG" 2>/dev/null || true; }
wait_for() {    # wait_for <description> <command...>
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
at_least() { [[ $(count "$1") -ge $2 ]]; }
run_reliefd() {
    : > "$LOG"
    "${WRAP[@]}" "$BIN/reliefd" "$@" >>"$OUT" 2>&1 &
    DPID=$!
}
stop_reliefd() {
    local rc=0
    kill -TERM "$DPID"
    wait "$DPID" || rc=$?
    DPID=""
    [[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"
}
# Every grant was handed back: releases + revokes = grants, nothing abandoned.
balanced() {
    local g r v
    g=$(count ' event=grant '); r=$(count ' event=release '); v=$(count ' event=recovery ')
    [[ $((r + v)) -eq $g && $(count ' event=banker_abandoned ') -eq 0 && $(count ' event=stray_release ') -eq 0 ]] \
        || fail "$1: grants $g, releases $r, revoked $v, abandoned $(count ' event=banker_abandoned ')"
}

for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done
printf '1 Hadapsar\n2 Kothrud\n' > "$RELIEF_HOME/two.conf"
TWO=(--camps "$RELIEF_HOME/two.conf" --scenario "$SCEN" --resources "$RES" --time-scale 10 --exit-when-done)

# 1. none: deadlock.
run_reliefd "${TWO[@]}" --deadlock none
wait_for "both camps waiting" at_least ' event=wait_unavailable ' 2
sleep 2     # the run takes about 0.7 s when it can finish: give it far longer
[[ $(count ' event=bye ') -eq 0 ]] || fail "none: a camp finished, expected a deadlock"
wf=$("$CTL" waitfor) || fail "reliefctl waitfor"
dot=$("$CTL" waitfor --dot) || fail "reliefctl waitfor --dot"
if ! { grep -q '^  camp1 -> camp2  \[TRUCK\]' <<< "$wf" && grep -q '^  camp2 -> camp1  \[BOAT\]' <<< "$wf" &&
        grep -qE '^cycle: camp(1|2) -> camp(2|1) -> camp(1|2) ' <<< "$wf"; }; then
    fail "waitfor: $wf"
fi
if ! { grep -q '^digraph waitfor {' <<< "$dot" && grep -q 'camp1 -> camp2 \[label="TRUCK"' <<< "$dot"; }; then
    fail "waitfor --dot: $dot"
fi
bank=$("${WRAP[@]}" "$BIN/reliefstat" --banker) || fail "reliefstat --banker"
grep -q 'the current state is UNSAFE' <<< "$bank" || fail "the deadlocked state should be unsafe: $bank"
stop_reliefd
pass "none: deadlock (no camp finished in 2 s); waitfor shows camp1 -> camp2 [TRUCK], camp2 -> camp1 [BOAT] and the cycle"

# 2.-5. The modes that must finish.
finish() {      # finish <label> <reliefd options...>
    local label=$1; shift
    run_reliefd "${TWO[@]}" "$@"
    wait_for "$label: both camps done" at_least ' event=bye ' 2
    stop_reliefd
    balanced "$label"
}
finish prevent --deadlock prevent
[[ $(count ' event=reject_order ') -ge 1 ]] || fail "prevent: expected out-of-order rejections"
pass "prevent: completed; $(count ' event=reject_order ') out-of-order requests rejected and retried in order"

finish all-or-nothing --deadlock prevent --all-or-nothing
[[ $(count ' event=grant .* resource=BUNDLE ') -eq 2 && $(count ' event=grant ') -eq 2 ]] \
    || fail "all-or-nothing: expected 2 bundle grants"
pass "prevent --all-or-nothing: completed; each task granted as one bundle"

finish avoid --deadlock avoid
[[ $(count ' event=deny_unsafe ') -ge 1 ]] || fail "avoid: expected an unsafe request to wait"
pass "avoid: completed; $(count ' event=deny_unsafe ') request(s) waited as unsafe"

finish detect --deadlock detect --detect-interval 0.2
grep -qE ' event=deadlock_detected cycle=camp(1->camp2->camp1|2->camp1->camp2) ' "$LOG" \
    || fail "detect: no deadlock_detected with the cycle"
grep -q ' event=recovery victim=camp2 severity=ROUTINE .* resource=TRUCK .* requeued=1 ' "$LOG" \
    || fail "detect: recovery should preempt camp 2's (ROUTINE) truck: $(grep -o 'event=recovery.*' "$LOG")"
pass "detect: completed; $(grep -o 'cycle=[^ ]*' "$LOG" | head -1) detected, camp 2's truck preempted and requeued"

# 6. The random mix (8 camps, default resources).
for mode in "prevent" "detect --detect-interval 0.2" "avoid"; do
    read -r -a opts <<< "--deadlock $mode"
    run_reliefd --camps "$ROOT/scenarios/camps.conf" --scenario "$MIX" --time-scale 5 --exit-when-done "${opts[@]}"
    wait_for "mix ($mode): every camp done" at_least ' event=bye ' 8
    stop_reliefd
    balanced "mix $mode"
done
pass "deadlock_mix.csv completes under prevent, detect and avoid"

# 7. Bad options.
for bad in "--deadlock detect --detect-interval 0" "--deadlock avoid --all-or-nothing"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    "${WRAP[@]}" "$BIN/reliefd" $bad --camps /dev/null >/dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "reliefd $bad should exit 2, got $rc"
done
pass "bad --detect-interval and --all-or-nothing without prevent exit 2"

if ls /dev/mqueue/reliefos_camp_* >/dev/null 2>&1; then
    fail "camp reply queues left behind: $(ls /dev/mqueue/reliefos_camp_*)"
fi
if grep -q 'WARNING: ThreadSanitizer' "$OUT"; then
    grep -m1 -A20 'WARNING: ThreadSanitizer' "$OUT" >&2
    fail "ThreadSanitizer reported a race"
fi
pass "no reply queues left behind; no ThreadSanitizer reports"

echo "all deadlock tests passed"
