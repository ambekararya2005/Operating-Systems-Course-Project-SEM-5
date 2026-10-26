#!/usr/bin/env bash
# OS concept: Write-ahead journal with fsync, crash recovery — Unit 5
#
# reliefd runs the flood scenario (8 camps, Banker's claims, grants and releases). At
# CRASH_ROUNDS random moments (deterministic for CRASH_SEED) it is killed with kill -9 and
# started again on the same $RELIEF_HOME. After each restart tests/helpers/journal_check.py
# (its own reader of the journal format) compares three states:
#   acked      what the crashed reliefd acknowledged (log lines are written after the fsync)
#   journal    snapshot.bin + journal.log as left by the crash
#   recovered  what the restarted reliefd rebuilt (event=recovered alloc=)
#   1. journal on: recovered == journal, journal at the last acknowledged seq == acked, so
#      0 units lost in every round; a checkpoint (snapshot.bin by rename) happened; a torn
#      record (round CRASH_ROUNDS-1) and a bad-CRC record (last round) appended to the journal
#      after the crash are skipped (skipped_bytes=20 / 40) and nothing else is lost
#   2. reliefd --no-journal: the same crashes lose every allocation that was held
# Then a graceful stop: the journal is checkpointed and empty.
#   BIN_DIR        directory holding the binaries (default: build)
#   RELIEF_WRAP    optional command prefix for reliefd, e.g. valgrind
#   CRASH_ROUNDS   kills per mode (default 10); CRASH_SEED (default 1)
#   CRASH_CSV      if set, one row per round is appended there (experiment E9)
#   TEST_TIMEOUT   seconds to wait for each condition (default 20)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-20}
ROUNDS=${CRASH_ROUNDS:-10}
SEED=${CRASH_SEED:-1}
CHECK="$ROOT/tests/helpers/journal_check.py"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-crash.XXXXXX")
DPID=""

cleanup() {
    if [[ -n "$DPID" ]] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    camps_gone 2>/dev/null || true
    # A kill -9 leaves the queue and the status table behind; they are ours (checked below).
    rm -f /dev/mqueue/reliefos_requests /dev/shm/reliefos_status
    rm -rf "$WORK"
}

fail() {
    echo "FAIL: $*" >&2
    echo "--- reliefd stderr (tail) ---" >&2
    tail -n 20 "$RELIEF_HOME/logs/reliefd.out" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }
count() { grep -c -- "$1" "$RELIEF_HOME/logs/reliefd.log" 2>/dev/null || true; }
wait_for() {    # wait_for <description> <command...>
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
at_least() { [[ $(count "$1") -ge $2 ]]; }

for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done
trap cleanup EXIT

# The camps die with reliefd (PR_SET_PDEATHSIG): wait until every camp in camps.pids is gone.
camps_gone() {
    local pid
    [[ -f "$RELIEF_HOME/camps.pids" ]] || return 0
    while read -r _ pid _; do
        [[ $pid =~ ^[1-9][0-9]*$ ]] && kill -0 "$pid" 2>/dev/null && return 1
    done < "$RELIEF_HOME/camps.pids"
    return 0
}

start_reliefd() {   # start_reliefd <extra args...>
    local before
    before=$(count 'event=recovered')
    "${WRAP[@]}" "$BIN/reliefd" --time-scale 10 "$@" >> "$RELIEF_HOME/logs/reliefd.out" 2>&1 &
    DPID=$!
    wait_for "reliefd recovered" at_least 'event=recovered' $((before + 1))
}

# Kill moments, seconds after the restart: 0.3-3.0, deterministic for the seed.
mapfile -t DELAYS < <(awk -v s="$SEED" -v n="$ROUNDS" 'BEGIN { srand(s); for (i = 0; i < n; i++) printf "%.2f\n", 0.3 + 2.7 * rand() }')

run_mode() {        # run_mode journal|none
    local mode=$1 extra=() total_lost=0 total_acked=0 r out
    [[ $mode == none ]] && extra=(--no-journal)
    RELIEF_HOME="$WORK/$mode"
    export RELIEF_HOME
    mkdir -p "$RELIEF_HOME/logs"
    start_reliefd "${extra[@]}"
    for ((r = 1; r <= ROUNDS; r++)); do
        sleep "${DELAYS[r - 1]}"
        kill -KILL "$DPID"
        wait "$DPID" 2>/dev/null || true
        DPID=""
        wait_for "camps to die with reliefd" camps_gone
        local tail_bytes=0
        if [[ $mode == journal && $r -eq $((ROUNDS - 1)) ]]; then
            head -c 20 /dev/urandom >> "$RELIEF_HOME/journal.log"    # a torn last write
            tail_bytes=20
        elif [[ $mode == journal && $r -eq $ROUNDS ]]; then
            head -c 40 /dev/zero >> "$RELIEF_HOME/journal.log"       # a whole record, bad CRC
            tail_bytes=40
        fi
        cp "$RELIEF_HOME/journal.log" "$WORK/journal.copy" 2>/dev/null || : > "$WORK/journal.copy"
        cp "$RELIEF_HOME/snapshot.bin" "$WORK/snapshot.copy" 2>/dev/null || : > "$WORK/snapshot.copy"
        start_reliefd "${extra[@]}"
        out=$(python3 -I "$CHECK" --mode "$mode" --log "$RELIEF_HOME/logs/reliefd.log" \
              --journal "$WORK/journal.copy" --snapshot "$WORK/snapshot.copy") || fail "$mode round $r: $out"
        if (( tail_bytes )); then
            grep 'event=recovered' "$RELIEF_HOME/logs/reliefd.log" | tail -n 1 | grep -q "skipped_bytes=$tail_bytes " \
                || fail "round $r: the ${tail_bytes}-byte bad tail was not skipped"
        fi
        echo "     $mode round $r: kill -9 after ${DELAYS[r - 1]}s: $out"
        local acked lost
        acked=$(sed -n 's/.*acked_units=\([0-9]*\).*/\1/p' <<< "$out")
        lost=$(sed -n 's/.*lost_units=\([0-9]*\).*/\1/p' <<< "$out")
        total_acked=$((total_acked + acked))
        total_lost=$((total_lost + lost))
        if [[ -n "${CRASH_CSV:-}" ]]; then
            [[ -s "$CRASH_CSV" ]] || echo "mode,seed,round,delay_s,acked_seq,acked_units,recovered_units,lost_units,ok" > "$CRASH_CSV"
            awk -v m="$mode" -v s="$SEED" -v r="$r" -v d="${DELAYS[r - 1]}" '{
                for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
                print m "," s "," r "," d "," v["acked_seq"] "," v["acked_units"] "," v["recovered_units"] "," v["lost_units"] "," v["ok"]
            }' <<< "$out" >> "$CRASH_CSV"
        fi
    done
    LOST=$total_lost
    ACKED=$total_acked
}

run_mode journal
[[ $ACKED -gt 0 ]] || fail "no allocation was held at any of the $ROUNDS kills (raise the delays)"
[[ $LOST -eq 0 ]] || fail "journal on: $LOST units lost"
at_least 'event=checkpoint seq=[1-9]' 1 || fail "no checkpoint (snapshot.bin by rename) after records"
pass "journal: $ROUNDS kill -9 crashes, $ACKED held units at the crashes, 0 lost; recovered == journal == acknowledged"
pass "journal: torn (20 bytes) and bad-CRC (40 bytes) last records skipped; checkpoints by rename happened"

# Graceful stop: the camps release everything, the journal is checkpointed (empty).
kill -TERM "$DPID"
rc=0
wait "$DPID" || rc=$?
DPID=""
[[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"
grep 'event=journal_stop' "$RELIEF_HOME/logs/reliefd.log" | tail -n 1 | grep -q 'camps=0 grants=0' \
    || fail "after a graceful stop the journal should hold no claims or grants"
[[ ! -s "$RELIEF_HOME/journal.log" ]] || fail "journal.log should be empty after the final checkpoint"
pass "graceful stop: everything released, final checkpoint, journal.log empty"

run_mode none
[[ $LOST -gt 0 && $LOST -eq $ACKED ]] || fail "--no-journal: expected all $ACKED held units lost, got $LOST"
pass "--no-journal: the same crashes lose all $LOST held units"
kill -TERM "$DPID"
wait "$DPID" || fail "reliefd --no-journal did not stop cleanly"
DPID=""

echo "all crash tests passed"
