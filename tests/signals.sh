#!/usr/bin/env bash
# OS concept: Signals as IPC — SIGUSR1 alert, SIGUSR2 status dump, SIGTERM graceful exit — Unit 2
#
# Tests signal-based camp control through reliefctl:
#   1. reliefd publishes camps.pids (8 RUNNING camps, all children of reliefd)
#   2. `reliefctl alert 3`: camp 3's next 5 requests arrive CRITICAL (alert=1, original
#      severity preserved), then traffic returns to normal; no other camp is affected
#   3. `reliefctl dump all`: every camp prints its counters
#   4. bad targets are rejected
#   5. SIGTERM while a send is blocked on a full queue: the camp finishes that send,
#      sends BYE and exits 0
#   BIN_DIR       directory holding reliefd and camp (default: build)
#   RELIEF_WRAP   optional command prefix for every binary, e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
SCENARIO="$ROOT/scenarios/flood_pune.csv"
CTL="$ROOT/scripts/reliefctl"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN" RELIEF_TIME_SCALE=20
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
PIDS="$RELIEF_HOME/camps.pids"
DPID=""
CPID=""

cleanup() {
    if [[ -n "$DPID" ]]; then kill -CONT "$DPID" 2>/dev/null || true; fi
    if [[ -n "$CPID" ]]; then kill -TERM "$CPID" 2>/dev/null || true; fi
    "$CTL" stop >/dev/null 2>&1 || true
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
at_least() { [[ $(count "$1") -ge $2 ]]; }     # at_least <pattern> <n>
pidfile_written() { [[ -s "$RELIEF_HOME/reliefd.pid" ]]; }
running_in_pids() { [[ $(grep -c " RUNNING " "$PIDS" 2>/dev/null) -eq $1 ]]; }
camp3_count_at_least() { [[ $(camp3_requests | wc -l) -ge $1 ]]; }
# Camp 3's request lines in arrival order, as "id severity alert orig_severity".
camp3_requests() {
    awk '/ event=request / && / camp=3 / {
            for (i = 1; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
            print f["id"], f["severity"], f["alert"], f["orig_severity"]
         }' "$LOG"
}

if [[ -e /dev/mqueue/reliefos_requests ]]; then
    echo "SKIP: /dev/mqueue/reliefos_requests exists (is reliefd running?). Stop it first." >&2
    exit 1
fi

# 1. Start the system; reliefd publishes the camp table.
"$CTL" start >/dev/null || fail "reliefctl start"
wait_for "8 camps RUNNING" at_least ' event=camp_running ' 8
wait_for "camps.pids lists 8 RUNNING camps" running_in_pids 8
dpid=$(cat "$RELIEF_HOME/reliefd.pid")
while read -r id pid _; do
    [[ "$id" =~ ^[0-9]+$ ]] || continue
    [[ "$(ps -o ppid= -p "$pid" | tr -d ' ')" == "$dpid" ]] || fail "camps.pids: camp $id pid $pid is not a child of reliefd"
done < "$PIDS"
pass "camps.pids lists 8 RUNNING camps, all children of reliefd (pid $dpid)"

# 2. Evacuation alert for camp 3.
wait_for "camp 3's first request" at_least ' event=request id=[0-9]* camp=3 ' 1
before=$(camp3_requests | wc -l)
"$CTL" alert 3 | grep -q 'camp 3 Shivajinagar' || fail "reliefctl alert 3"
# An alert applies to requests sent after the camp's main loop sees it, so one request
# already in flight when SIGUSR1 arrives may still go out normally (allowed: at most one).
wait_for "camp 3: in-flight + 5 escalated + 1 normal" camp3_count_at_least $((before + 7))
grep -q 'camp\[[0-9]*\] event=alert camp=3 signal=USR1 escalate_next=5' "$OUT" || fail "camp 3 did not log the alert"

mapfile -t after < <(camp3_requests | tail -n +"$((before + 1))")
read -r _ _ alert _ <<< "${after[0]}"
if [[ "$alert" == 0 ]]; then after=("${after[@]:1}"); fi   # skip the in-flight request
for i in 0 1 2 3 4; do
    read -r id sev alert orig <<< "${after[$i]}"
    [[ "$sev" == CRITICAL && "$alert" == 1 ]] || fail "request $id after alert: severity=$sev alert=$alert"
    line=$(( id - 3000000 ))
    csv_sev=$(awk -F, -v n="$line" 'NR == n { print $3 }' "$SCENARIO")
    [[ "$orig" == "$csv_sev" ]] || fail "request $id: orig_severity=$orig but scenario says $csv_sev"
done
read -r id sev alert orig <<< "${after[5]}"
[[ "$alert" == 0 && "$sev" == "$orig" ]] || fail "6th request after alert ($id) still escalated"
[[ $(count ' alert=1 ') -eq 5 ]] || fail "expected exactly 5 escalated requests in total"
pass "alert 3: next 5 camp-3 requests CRITICAL (alert=1, orig severity kept), 6th normal, other camps unaffected"

# 3. Status dump of every camp.
dump=$("$CTL" dump all) || fail "reliefctl dump all"
for id in 1 2 3 4 5 6 7 8; do
    grep -q "^camp=$id pid=[0-9]* sent=[0-9]* critical=" <<< "$dump" || fail "dump all: no status from camp $id"
done
grep -q '^camp=3 .* escalated=5 alerts=1 escalate_pending=0 ' <<< "$dump" || fail "camp 3 counters wrong: $(grep '^camp=3 ' <<< "$dump")"
pass "dump all: 8 camps reported counters; camp 3 shows escalated=5 alerts=1"

# 4. Bad targets.
rc=0; "$CTL" alert 99 >/dev/null 2>&1 || rc=$?
[[ $rc -ne 0 ]] || fail "alert 99 should fail"
rc=0; "$CTL" dump abc >/dev/null 2>&1 || rc=$?
[[ $rc -eq 2 ]] || fail "dump abc should exit 2, got $rc"
pass "unknown camp id and non-numeric target rejected"

"$CTL" stop >/dev/null || fail "reliefctl stop"
[[ $(count 'event=camp_exit .* status=0 ') -eq 8 ]] || fail "expected 8 clean camp exits"
[[ $(count ' event=bye ') -eq 8 ]] || fail "expected 8 BYE"
[[ ! -e "$PIDS" ]] || fail "camps.pids left behind"
pass "SIGTERM via reliefctl stop: 8 camps sent BYE and exited 0; camps.pids removed"

# 5. SIGTERM during a blocked send: freeze reliefd so camp 1's 10th request (after
#    HELLO + 9 requests fill the 10-slot queue) waits for space, then ask it to stop.
: > "$LOG"
"${WRAP[@]}" "$BIN/reliefd" --camps /dev/null >>"$OUT" 2>&1 &
DPID=$!
wait_for "reliefd pid file" pidfile_written
kill -STOP "$DPID"
"${WRAP[@]}" "$BIN/camp" --id 1 --scenario "$SCENARIO" --time-scale 0 2>"$RELIEF_HOME/camp1.log" &
CPID=$!
wait_for "camp 1 blocked on the full queue" grep -q 'queue full, retrying REQUEST' "$RELIEF_HOME/camp1.log"
kill -TERM "$CPID"
sleep 0.3
kill -CONT "$DPID"
rc=0; wait "$CPID" || rc=$?; CPID=""
[[ $rc -eq 0 ]] || fail "camp exited with status $rc after SIGTERM"
sent=$(grep -c 'sent REQUEST' "$RELIEF_HOME/camp1.log" || true)
[[ $sent -eq 10 ]] || fail "camp should have finished exactly its in-flight 10th request, sent $sent"
"${WRAP[@]}" "$BIN/camp" --shutdown 2>/dev/null
rc=0; wait "$DPID" || rc=$?; DPID=""
[[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"
grep -q 'event=daemon_stop hello=1 request=10 bye=1' "$LOG" || fail "in-flight request or BYE lost: $(grep daemon_stop "$LOG")"
pass "SIGTERM while blocked on a full queue: in-flight request delivered, BYE sent, exit 0"

echo "all signal tests passed"
