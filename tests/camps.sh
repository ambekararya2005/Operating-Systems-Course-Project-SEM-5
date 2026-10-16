#!/usr/bin/env bash
# OS concept: Process creation and termination — fork/exec, SIGCHLD + waitpid reaping, restarts — Unit 2
#
# Tests reliefd as the parent of all camp processes:
#   8 camps spawned, kill -9 restarts a camp (max 3 times, then CRASHED),
#   no zombies, and shutdown escalates SIGTERM -> SIGKILL for a stuck (SIGSTOPped) camp.
#   BIN_DIR       directory holding reliefd and camp (default: build)
#   RELIEF_WRAP   optional command prefix for reliefd (camps inherit it via exec), e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
#   KEEP_HOME     if set, keep the temporary RELIEF_HOME (logs) for debugging
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME
mkdir -p "$RELIEF_HOME/logs"
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
PIDFILE="$RELIEF_HOME/reliefd.pid"
DPID=""

cleanup() {
    if [[ -n "$DPID" ]] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    if [[ -n "${KEEP_HOME:-}" ]]; then
        echo "kept $RELIEF_HOME" >&2
    else
        rm -rf "$RELIEF_HOME"
    fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "--- structured log ---" >&2
    cat "$LOG" >&2 2>/dev/null || true
    echo "--- reliefd stderr ---" >&2
    tail -n 40 "$OUT" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }

count() { grep -c -- "$1" "$LOG" || true; }
children() { ps -o pid= --ppid "$DPID" | wc -l; }
# shellcheck disable=SC2009  # need the state column of reliefd's children
zombies() { ps -o stat= --ppid "$DPID" | grep -c '^Z' || true; }
# Latest pid reliefd spawned for camp $1 (taken from the structured log).
camp_pid() { grep "event=camp_spawn camp=$1 " "$LOG" | tail -n 1 | sed 's/.* pid=\([0-9]*\) .*/\1/'; }

wait_for() {    # wait_for <description> <command...>
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
running_events() { [[ $(count "event=camp_running camp=$1 ") -ge $2 ]]; }
total_running() { [[ $(count 'event=camp_running ') -ge $1 ]]; }
logged() { grep -q -- "$1" "$LOG"; }
children_eq() { [[ $(children) -eq $1 ]]; }
requests_at_least() { [[ $(count " event=request .*$1") -ge $2 ]]; }   # <pattern> <n>

if [[ -e /dev/mqueue/reliefos_requests ]]; then
    echo "SKIP: /dev/mqueue/reliefos_requests exists (is reliefd running?). Stop it first." >&2
    exit 1
fi

# 1. reliefd fork/execs one camp per line of camps.conf.
# Expected counts come from the scenario itself (--time-scale 0: no pacing, fast test).
SCENARIO="$ROOT/scenarios/flood_pune.csv"
TOTAL_ROWS=$(awk 'NR > 1' "$SCENARIO" | wc -l)
CAMP3_ROWS=$(awk -F, 'NR > 1 && $2 == 3' "$SCENARIO" | wc -l)
"${WRAP[@]}" "$BIN/reliefd" --camps "$ROOT/scenarios/camps.conf" \
    --scenario "$SCENARIO" --seed 1 --time-scale 0 >>"$OUT" 2>&1 &
DPID=$!
wait_for "8 camps RUNNING" total_running 8
[[ $(children) -eq 8 ]] || fail "expected 8 children of reliefd, got $(children)"
pass "reliefd (pid $DPID) has 8 camp children"
wait_for "$TOTAL_ROWS scenario requests" requests_at_least "" "$TOTAL_ROWS"
pass "camps replayed all $TOTAL_ROWS scenario rows"

# 2. kill -9 camp 3: reaped, logged as signaled, restarted; repeat until it is CRASHED.
for n in 1 2 3; do
    old=$(camp_pid 3)
    kill -KILL "$old"
    wait_for "camp 3 restart $n" logged "event=camp_restart camp=3 name=Shivajinagar restart=$n "
    wait_for "restarted camp 3 RUNNING" running_events 3 $((n + 1))
    new=$(camp_pid 3)
    [[ "$new" != "$old" ]] || fail "camp 3 kept pid $old after restart"
    # Let the new camp finish replaying its 4 rows so the message totals are exact.
    wait_for "camp 3 pid $new replays $CAMP3_ROWS rows" \
        requests_at_least "camp=3 camp_pid=$new " "$CAMP3_ROWS"
    [[ $(children) -eq 8 ]] || fail "expected 8 children after restart $n, got $(children)"
    [[ $(zombies) -eq 0 ]] || fail "zombie children after restart $n"
    pass "kill -9 camp 3 (pid $old) -> restart $n/3 as pid $new, 8 children, no zombies"
done
logged "event=camp_signaled camp=3 name=Shivajinagar pid=$old sig=9 signame=KILL" \
    || fail "signal death not logged with WTERMSIG"

old=$(camp_pid 3)
kill -KILL "$old"
wait_for "camp 3 CRASHED" logged "event=camp_crashed camp=3 "
wait_for "7 children" children_eq 7
[[ $(zombies) -eq 0 ]] || fail "zombie after final crash"
pass "4th kill -9 -> camp 3 CRASHED (no more restarts), 7 children"

# 3. Shutdown: a SIGSTOPped camp ignores SIGTERM until reliefd escalates to SIGKILL.
stuck=$(camp_pid 5)
kill -STOP "$stuck"
mapfile -t spawned < <(grep 'event=camp_spawn ' "$LOG" | sed 's/.* pid=\([0-9]*\) .*/\1/')
kill -TERM "$DPID"
rc=0; wait "$DPID" || rc=$?; DPID=""
[[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"

logged "event=camp_killed camp=5 name=Sangamwadi pid=$stuck signal=KILL" \
    || fail "stuck camp 5 was not SIGKILLed"
[[ $(count 'event=camp_exit .* status=0 ') -eq 6 ]] || fail "expected 6 clean camp exits (status=0)"
logged 'event=camp_final camp=3 name=Shivajinagar state=CRASHED restarts=3' || fail "camp 3 not CRASHED"
# 8 HELLO + 3 from restarts; camp 3 replayed its rows 3 extra times; BYE from the 6 clean exits.
logged "event=daemon_stop hello=11 request=$((TOTAL_ROWS + 3 * CAMP3_ROWS)) bye=6" \
    || fail "wrong message totals"
pass "SIGTERM -> 6 camps exited 0 (BYE sent), stuck camp 5 SIGKILLed after grace period"

for pid in "${spawned[@]}"; do
    if ps -o stat= -p "$pid" >/dev/null 2>&1; then
        fail "camp pid $pid still exists ($(ps -o stat=,args= -p "$pid"))"
    fi
done
pass "all ${#spawned[@]} camp processes ever spawned are gone (no zombies, no orphans)"

[[ ! -e "$PIDFILE" ]] || fail "pid file left behind"
[[ ! -e /dev/mqueue/reliefos_requests ]] || fail "queue left behind"
pass "pid file and queue removed"

echo "all camp supervision tests passed"
