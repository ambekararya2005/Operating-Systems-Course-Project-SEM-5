#!/usr/bin/env bash
# OS concept: Threads (pthreads) running a live scheduler; mutex + condition variable — Unit 2/3
#
# Tests reliefd's rescue-team threads (src/reliefd/dispatch.c):
#   1. FCFS, 2 teams, flood scenario: only RESCUE requests are scheduled; each is dispatched
#      and completed exactly once, in arrival order, with no preemption and nothing abandoned
#   2. reliefctl report's scheduler section matches the event=complete lines
#   3. prio --preemptive on, 1 team: a CRITICAL rescue arriving while a ROUTINE one runs
#      preempts it (event=preempt reason=priority); the ROUTINE one resumes and finishes later
#   4. mlfq, 1 team, a steady CRITICAL stream: the waiting ROUTINE rescue is promoted
#      (event=promote) and still served
#   5. reliefctl sched / reliefstat show the policy, queue levels and every team
#   6. stopping with work queued abandons it cleanly; bad options are rejected
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for reliefd (and its camps), e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
CTL="$ROOT/scripts/reliefctl"

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
# All scheduled requests completed and every camp said BYE.
drained() { [[ $(count ' event=bye ') -ge $1 && $(count ' event=complete ') -ge $(count ' event=request .* sched=1') ]]; }
field() { sed -n "s/.* $2=\\([^ ]*\\).*/\\1/p" <<< "$1"; }   # field <line> <key>

# run_reliefd <args...>: start reliefd in the background on a fresh log.
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

for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done

# 1. FCFS on the flood scenario.
run_reliefd --camps "$ROOT/scenarios/camps.conf" --scenario "$ROOT/scenarios/flood_pune.csv" \
    --time-scale 5 --exit-when-done --policy fcfs --teams 2
wait_for "camps done and scheduler drained" drained 8
stop_reliefd
sched=$(count ' event=request .* sched=1')
rescues=$(awk -F, 'NR > 1 && $4 == "RESCUE"' "$ROOT/scenarios/flood_pune.csv" | wc -l)
[[ $sched -eq $rescues && $sched -gt 0 ]] || fail "expected $rescues RESCUE requests scheduled, got $sched"
[[ $(count ' event=request .* resource=RESCUE .* sched=0') -eq 0 ]] || fail "a RESCUE request was not scheduled"
[[ $(count ' event=request .* resource=\(FOOD_KIT\|MED_KIT\|HYGIENE_KIT\|TRUCK\|BOAT\) .* sched=1') -eq 0 ]] \
    || fail "a non-RESCUE request was scheduled"
[[ $(count ' event=dispatch ') -eq $sched && $(count ' event=complete ') -eq $sched ]] \
    || fail "each scheduled request should be dispatched and completed exactly once"
diff <(sed -n 's/.* event=request id=\([0-9]*\) .* sched=1$/\1/p' "$LOG") \
     <(sed -n 's/.* event=dispatch team=[0-9]* id=\([0-9]*\) .*/\1/p' "$LOG") >/dev/null \
    || fail "FCFS should dispatch requests in arrival order"
[[ $(count ' event=preempt ') -eq 0 ]] || fail "FCFS must never preempt"
grep -q "event=sched_stop policy=fcfs enqueued=$sched completed=$sched .* abandoned=0" "$LOG" \
    || fail "sched_stop totals wrong: $(grep -o 'event=sched_stop.*' "$LOG")"
awk '/ event=complete / { for (i = 1; i <= NF; i++) if ($i ~ /^(wait|turnaround|response)=/) { split($i, kv, "="); if (kv[2] < 0) bad = 1 } } END { exit bad }' "$LOG" \
    || fail "negative wait/turnaround/response"
pass "fcfs, 2 teams: $sched RESCUE requests (of 200) dispatched + completed once each, in arrival order, no preemption"

# 2. report's scheduler section agrees with the event=complete lines (the log holds only run 1).
rep=$("$CTL" report --last-run) || fail "reliefctl report"
grep -q '^Scheduler (RESCUE requests' <<< "$rep" || fail "report: no scheduler section"
for sev in CRITICAL URGENT; do
    want=$(awk -v sev="$sev" '/ event=complete / { for (i = 1; i <= NF; i++) if ($i == "severity=" sev) c = 1; if (c) { for (i = 1; i <= NF; i++) if ($i ~ /^wait=/) { split($i, kv, "="); w += kv[2]; n++ } } c = 0 } END { if (n) printf "%d %.1f", n, w / n }' "$LOG")
    if [[ -n "$want" ]]; then
        grep -qE "^  $sev +${want% *} +${want#* } " <<< "$rep" \
            || fail "report $sev row should be '$want': $(grep "^  $sev" <<< "$rep" | tail -1)"
    fi
done
pass "report: scheduler section present; per-severity averages match the event=complete lines"

# 3. Preemptive priority, 1 team: a CRITICAL rescue preempts a ROUTINE one.
cat > "$RELIEF_HOME/one.conf" <<'EOF'
1 Hadapsar
EOF
cat > "$RELIEF_HOME/preempt.csv" <<'EOF'
arrival_min,camp_id,severity,resource,quantity,service_time
0,1,ROUTINE,RESCUE,1,30
5,1,CRITICAL,RESCUE,1,5
EOF
run_reliefd --camps "$RELIEF_HOME/one.conf" --scenario "$RELIEF_HOME/preempt.csv" --time-scale 20 \
    --exit-when-done --policy prio --preemptive on --teams 1
wait_for "preemption scenario drained" drained 1
stop_reliefd
r=1000002; c=1000003    # request ids: camp 1, scenario lines 2 and 3
grep -q " event=preempt team=1 id=$r .* reason=priority" "$LOG" || fail "ROUTINE $r was not preempted for priority"
order=$(sed -n 's/.* event=\(dispatch\|preempt\|complete\) team=1 id=\([0-9]*\) .*/\1 \2/p' "$LOG" | tr '\n' ',')
[[ "$order" == "dispatch $r,preempt $r,dispatch $c,complete $c,dispatch $r,complete $r," ]] \
    || fail "unexpected sequence: $order"
grep -q " event=complete team=1 id=$c .* wait=0\\.[0-9] .*" "$LOG" || \
    grep -q " event=complete team=1 id=$c .* wait=1\\.[0-9] .*" "$LOG" || fail "CRITICAL should wait at most ~1 minute"
pass "prio --preemptive on: CRITICAL preempted the running ROUTINE rescue, which resumed and finished last"

# 4. MLFQ aging with 1 team under a steady CRITICAL stream.
{
    echo "arrival_min,camp_id,severity,resource,quantity,service_time"
    echo "0,1,CRITICAL,RESCUE,1,10"
    echo "1,1,ROUTINE,RESCUE,1,5"
    for k in $(seq 1 14); do echo "$((10 * k)),1,CRITICAL,RESCUE,1,10"; done
} > "$RELIEF_HOME/aging.csv"
run_reliefd --camps "$RELIEF_HOME/one.conf" --scenario "$RELIEF_HOME/aging.csv" --time-scale 10 \
    --exit-when-done --policy mlfq --teams 1 --age 60
wait_for "aging scenario drained" drained 1
stop_reliefd
grep -q ' event=promote id=1000003 .* from=Q2 to=Q1' "$LOG" || fail "the ROUTINE rescue was never promoted"
grep -q ' event=complete team=1 id=1000003 ' "$LOG" || fail "the promoted ROUTINE rescue was not served"
rwait=$(field "$(grep ' event=complete team=1 id=1000003 ' "$LOG")" wait)
pass "mlfq: waiting ROUTINE rescue promoted Q2 -> Q1 by aging and served (wait ${rwait} simulated min)"

# 5. reliefctl sched / reliefstat while running (fresh log: count only this run).
: > "$LOG"
RELIEF_POLICY=mlfq RELIEF_TEAMS=2 RELIEF_TIME_SCALE=20 "$CTL" start >/dev/null || fail "reliefctl start"
wait_for "first dispatch" at_least ' event=dispatch ' 1
sched_out=$("$CTL" sched) || fail "reliefctl sched"
grep -q '^scheduler: policy mlfq, 2 teams' <<< "$sched_out" || fail "sched: no policy line: $sched_out"
grep -qE '^  ready queue: Q0=[0-9]+ Q1=[0-9]+ Q2=[0-9]+$' <<< "$sched_out" || fail "sched: no MLFQ levels"
[[ $(grep -cE '^ +[12]  (idle|busy) ' <<< "$sched_out") -eq 2 ]] || fail "sched: expected 2 team rows"
"$CTL" sched --json | python3 -I -c 'import json,sys; s=json.load(sys.stdin); assert s["policy"]=="mlfq" and len(s["team_list"])==2 and len(s["queue"])==3' \
    || fail "sched --json invalid"
"$CTL" status | grep -q '^scheduler: policy mlfq' || fail "status should include the scheduler section"
pass "reliefctl sched: policy, queue per level (Q0-Q2) and both teams; --json valid; status includes it"

# 6a. Stop with work still queued: abandoned cleanly.
"$CTL" stop >/dev/null || fail "reliefctl stop"
grep -qE 'event=sched_stop policy=mlfq .* abandoned=[0-9]+' "$LOG" || fail "no sched_stop line"
[[ ! -e /dev/shm/reliefos_status && ! -e /dev/mqueue/reliefos_requests ]] || fail "IPC objects left behind"
pass "stop mid-run: teams joined, queued work logged as abandoned, IPC objects removed"

# 6b. Bad options.
rc=0; "${WRAP[@]}" "$BIN/reliefd" --policy nope --camps /dev/null >/dev/null 2>&1 || rc=$?
[[ $rc -eq 1 ]] || fail "unknown --policy should exit 1, got $rc"
for bad in "--teams 0" "--teams 17" "--preemptive maybe"; do
    rc=0; read -r -a a <<< "$bad"; "${WRAP[@]}" "$BIN/reliefd" "${a[@]}" >/dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "reliefd $bad should exit 2, got $rc"
done
[[ ! -e /dev/shm/reliefos_status && ! -e /dev/mqueue/reliefos_requests ]] || fail "failed start left IPC objects"
pass "unknown policy fails cleanly; --teams 0/17 and --preemptive maybe exit 2"

# Under `make tsan`, a data race makes reliefd print a report (and exit 66). Runs started via
# reliefctl are not exit-checked, so also look for any report in the shared stderr log.
if grep -q 'WARNING: ThreadSanitizer' "$OUT"; then
    grep -m1 -A20 'WARNING: ThreadSanitizer' "$OUT" >&2
    fail "ThreadSanitizer reported a race"
fi
pass "no ThreadSanitizer reports in any run's stderr"

echo "all dispatcher tests passed"
