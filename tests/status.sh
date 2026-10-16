#!/usr/bin/env bash
# OS concept: IPC via POSIX shared memory; process-shared robust mutex + seqlock — Unit 2/3
#
# Tests the live status table (/reliefos_status) and its readers:
#   1. reliefd creates the segment; reliefstat and `reliefctl status` show 8 RUNNING camps
#   2. the table updates live: total_requests grows and every camp heartbeats each second
#   3. counters are exact: when the scenario is done, total_requests == 200 == sum of the
#      camps' requests_sent, and each camp's count matches its scenario rows
#   4. reliefd-owned fields follow the camp table: kill -9 a camp -> new pid, restarts=1
#   5. a writer that dies holding the lock is recovered (robust mutex, EOWNERDEAD) and
#      reliefstat can read the table again
#   6. shutdown unlinks /dev/shm/reliefos_status; a stale segment from a SIGKILLed reliefd
#      is reported as stale and replaced on the next start
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for every binary, e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
SCENARIO="$ROOT/scenarios/flood_pune.csv"
CTL="$ROOT/scripts/reliefctl"
SHM=/dev/shm/reliefos_status

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN" RELIEF_TIME_SCALE=20
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
PIDS="$RELIEF_HOME/camps.pids"

cleanup() {
    "$CTL" stop >/dev/null 2>&1 || true
    if [[ -n "${KEEP_HOME:-}" ]]; then
        echo "kept $RELIEF_HOME" >&2
    else
        rm -rf "$RELIEF_HOME"
    fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "--- reliefstat ---" >&2
    reliefstat >&2 || true
    echo "--- reliefd stderr (tail) ---" >&2
    tail -n 30 "$OUT" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }
reliefstat() { "${WRAP[@]}" "$BIN/reliefstat" "$@"; }

wait_for() {    # wait_for <description> <command...>
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
# jq-free JSON queries: python3 (stdlib only) on reliefstat --json.
q() {   # q <python expression over s (the parsed JSON)>
    reliefstat --json | python3 -I -c "import json,sys; s=json.load(sys.stdin); print($1)"
}
running_camps() { [[ $(q 'sum(c["state"] == "RUNNING" for c in s["camps"])') -eq $1 ]]; }
total_is() { [[ $(q 's["total_requests"]') -eq $1 ]]; }
camp_field() { q "[c for c in s['camps'] if c['id'] == $1][0]['$2']"; }
restarted() { [[ $(camp_field "$1" restarts) -eq $2 && $(camp_field "$1" state) == RUNNING ]]; }

for f in /dev/mqueue/reliefos_requests "$SHM"; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done

# 1. Segment created; readers see 8 RUNNING camps.
"$CTL" start >/dev/null || fail "reliefctl start"
wait_for "8 RUNNING camps in the status table" running_camps 8
[[ "$(stat -c %a "$SHM")" == 644 ]] || fail "$SHM mode is $(stat -c %a "$SHM"), expected 644"
dpid=$(cat "$RELIEF_HOME/reliefd.pid")
[[ $(q 's["daemon_pid"]') -eq $dpid && $(q 's["running"]') == True ]] || fail "daemon fields wrong"
status=$("$CTL" status) || fail "reliefctl status"
grep -q "^running  pid=$dpid" <<< "$status" || fail "reliefctl status: no running line"
[[ $(grep -cE '^ +[0-9]+  [A-Za-z]+ +[0-9]+  RUNNING ' <<< "$status") -eq 8 ]] \
    || fail "reliefctl status: expected 8 RUNNING rows in the table"
pass "reliefd created $SHM (mode 644); reliefstat and reliefctl status show 8 RUNNING camps"

# 2. Live updates: requests grow, every camp (busy or idle) heartbeats.
mapfile -t s1 < <(q '"\n".join([str(s["total_requests"])] + [str(c["heartbeat_ms"]) for c in s["camps"]])')
sleep 2.2
mapfile -t s2 < <(q '"\n".join([str(s["total_requests"])] + [str(c["heartbeat_ms"]) for c in s["camps"]])')
[[ ${s2[0]} -gt ${s1[0]} ]] || fail "total_requests did not grow (${s1[0]} -> ${s2[0]})"
for i in 1 2 3 4 5 6 7 8; do
    [[ ${s2[$i]} -gt ${s1[$i]} ]] || fail "camp $i heartbeat did not advance"
done
max_age=$(q 'max(c["heartbeat_age_ms"] for c in s["camps"])')
[[ $max_age -lt 2500 ]] || fail "a heartbeat is ${max_age} ms old (camps beat every second)"
pass "live: total_requests ${s1[0]} -> ${s2[0]} in 2.2 s; all 8 heartbeats advanced (max age ${max_age} ms)"

# 3. Exact counters once the scenario has been replayed.
wait_for "all 200 requests" total_is 200
sleep 1.2   # one more heartbeat so every camp has published its final count
sum=$(q 'sum(c["requests_sent"] for c in s["camps"])')
[[ $sum -eq 200 ]] || fail "sum of requests_sent is $sum, expected 200"
for i in 1 2 3 4 5 6 7 8; do
    rows=$(awk -F, -v c="$i" 'NR > 1 && $2 == c' "$SCENARIO" | wc -l)
    sent=$(camp_field "$i" requests_sent)
    [[ $sent -eq $rows ]] || fail "camp $i requests_sent=$sent, scenario has $rows rows"
done
pass "total_requests = 200 = sum of camps' requests_sent; each camp matches its scenario rows"

# 4. reliefd-owned fields: restart after kill -9.
old=$(camp_field 4 pid)
kill -KILL "$old"
wait_for "camp 4 restarted" restarted 4 1
new=$(camp_field 4 pid)
[[ "$new" != "$old" ]] || fail "camp 4 pid unchanged after restart"
grep -q "^4 $new RUNNING " "$PIDS" || fail "camps.pids and status table disagree on camp 4"
pass "kill -9 camp 4: table shows new pid $new, restarts=1, RUNNING (matches camps.pids)"

# 5. A writer dies while holding the lock: the next writer recovers it.
"${WRAP[@]}" "$BIN/shm_die_locked" >/dev/null || fail "shm_die_locked helper failed"
reliefstat >/dev/null || fail "reliefstat could not read the table after a writer died holding the lock"
wait_for "lock recovery logged" grep -q 'event=status_lock_recovered .*reason=EOWNERDEAD' "$OUT"
before=$(q 's["total_requests"]')
sleep 1.2
reliefstat >/dev/null || fail "table unreadable after recovery"
pass "writer died holding the robust mutex: recovered via EOWNERDEAD (total_requests=$before), table readable"

# 6. Shutdown unlinks the segment.
"$CTL" stop >/dev/null || fail "reliefctl stop"
[[ ! -e "$SHM" ]] || fail "$SHM left behind after stop"
rc=0; reliefstat >/dev/null 2>&1 || rc=$?
[[ $rc -eq 1 ]] || fail "reliefstat should exit 1 when reliefd is stopped, got $rc"
[[ "$("$CTL" status --json)" == '{"running":false}' ]] || fail "status --json when stopped"
pass "reliefctl stop unlinked $SHM; reliefstat exits 1, status --json reports running=false"

# Stale segment: SIGKILL reliefd (camps die via PDEATHSIG), then start again.
"$CTL" start >/dev/null || fail "reliefctl start (2)"
wait_for "8 RUNNING camps (2)" running_camps 8
mapfile -t camp_pids < <(awk '$1 ~ /^[0-9]+$/ { print $2 }' "$PIDS")
kill -KILL "$(cat "$RELIEF_HOME/reliefd.pid")"
gone() { for p in "${camp_pids[@]}"; do kill -0 "$p" 2>/dev/null && return 1; done; return 0; }
wait_for "orphaned camps to exit (PDEATHSIG)" gone
[[ -e "$SHM" ]] || fail "segment should survive a SIGKILLed reliefd"
rc=0; stale=$(reliefstat 2>&1) || rc=$?
if [[ $rc -ne 1 ]] || ! grep -q 'NOT RUNNING (stale table)' <<< "$stale"; then
    fail "stale table not reported: $stale"
fi
"$CTL" start >/dev/null || fail "reliefctl start after crash"
wait_for "8 RUNNING camps after crash" running_camps 8
grep -q 'event=stale_shm_removed' "$LOG" || fail "stale segment not replaced"
"$CTL" stop >/dev/null
[[ ! -e "$SHM" ]] || fail "$SHM left behind"
pass "SIGKILLed reliefd: stale segment reported, replaced on next start, unlinked on stop"

echo "all status tests passed"
