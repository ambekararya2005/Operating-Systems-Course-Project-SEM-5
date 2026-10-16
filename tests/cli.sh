#!/usr/bin/env bash
# OS concept: Shell programming (Unit 1) and process states / PCB via /proc (Unit 2)
#
# Tests the Phase 1 CLI:
#   1. reliefctl and stockcheck.sh pass shellcheck; usage errors exit 2, runtime errors exit 1
#   2. `simulate flood` runs a generated scenario to completion and its report matches the CSV
#   3. `ps` reads PCB fields from /proc (PPID, state letters incl. T after SIGSTOP), `tree`
#      is rooted at reliefd
#   4. `report --since` / `--last-run` windows
#   5. stockcheck.sh warns above its threshold
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for reliefd/reliefstat (applied by reliefctl)
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
TIMEOUT=${TEST_TIMEOUT:-10}
CTL="$ROOT/scripts/reliefctl"
STOCK="$ROOT/scripts/stockcheck.sh"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN" RELIEF_TIME_SCALE=20
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
    tail -n 20 "$OUT" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }
wait_for() {    # wait_for <description> <command...>
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
expect_rc() {   # expect_rc <rc> <description> <command...>
    local want=$1 desc=$2 rc=0
    shift 2
    "$@" >/dev/null 2>&1 || rc=$?
    [[ $rc -eq $want ]] || fail "$desc: expected exit $want, got $rc"
}
running_in_pids() { [[ $(grep -c " RUNNING " "$PIDS" 2>/dev/null) -eq $1 ]]; }

for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done

# 1. Static checks and exit codes.
if command -v shellcheck >/dev/null; then
    shellcheck "$CTL" "$STOCK" || fail "shellcheck reported problems"
    pass "shellcheck: reliefctl and stockcheck.sh clean"
fi
expect_rc 2 "no command" "$CTL"
expect_rc 2 "unknown command" "$CTL" bogus
expect_rc 2 "report --since abc" "$CTL" report --since abc
expect_rc 2 "report unknown option" "$CTL" report --bogus
expect_rc 2 "simulate unknown scenario" "$CTL" simulate rain
expect_rc 2 "simulate --camps 0" "$CTL" simulate flood --camps 0
expect_rc 2 "simulate --camps x" "$CTL" simulate flood --camps=x
expect_rc 2 "alert without camp" "$CTL" alert
expect_rc 1 "ps without reliefd" "$CTL" ps
expect_rc 1 "tree without reliefd" "$CTL" tree
expect_rc 0 "help" "$CTL" help
pass "usage errors exit 2, 'reliefd not running' exits 1, help exits 0"

# 2. simulate: generated scenario, run to completion, report matches the CSV.
sim=$("$CTL" simulate flood --camps=4 --seed 7 --time-scale 5) || fail "simulate exited non-zero"
csv="$RELIEF_HOME/sim/flood_c4_s7_r200.csv"
[[ -r "$csv" ]] || fail "simulate did not write $csv"
python3 -I "$ROOT/experiments/gen_scenario.py" --camps 4 --seed 7 | cmp -s - "$csv" || fail "scenario not reproducible"
grep -q '^all 4 camps sent BYE$' <<< "$sim" || fail "simulate did not wait for 4 BYE"
grep -q '^  requests: 200 ' <<< "$sim" || fail "report total is not 200"
for c in 1 2 3 4; do
    rows=$(awk -F, -v c="$c" 'NR > 1 && $2 == c' "$csv" | wc -l)
    grep -qE "^ +$c  [A-Za-z]+ +$rows +" <<< "$sim" || fail "report: camp $c should have $rows requests"
done
for sev in "CRITICAL 30" "URGENT 70" "ROUTINE 100"; do
    grep -qE "^  ${sev% *} +${sev#* } " <<< "$sim" || fail "report: expected $sev"
done
best=$(awk -F, 'NR > 1 { c[$2]++ } END { for (k in c) if (c[k] > m || (c[k] == m && k < b)) { m = c[k]; b = k }; print b, m }' "$csv")
grep -qE "^Busiest camp: ${best% *} [A-Za-z]+ \(${best#* } requests" <<< "$sim" || fail "busiest camp should be '$best'"
"$CTL" status | grep -q '^stopped' || fail "reliefd still running after simulate"
pass "simulate flood --camps=4 --seed 7: 4 BYE, report = CSV (per camp, 30/70/100, busiest ${best% *}), reliefd stopped"

# 3. ps and tree on the running system.
"$CTL" start >/dev/null || fail "reliefctl start"
wait_for "8 RUNNING camps" running_in_pids 8
dpid=$(cat "$RELIEF_HOME/reliefd.pid")
psout=$("$CTL" ps) || fail "reliefctl ps"
grep -q "task_struct" <<< "$psout" || fail "ps header should mention task_struct"
[[ $(grep -cE '^ +[0-9]+ +[0-9]+  (reliefd|camp )' <<< "$psout") -eq 9 ]] || fail "ps should list reliefd + 8 camps"
[[ $(awk -v d="$dpid" '$3 == "camp" && $2 == d' <<< "$psout" | wc -l) -eq 8 ]] || fail "every camp's PPID should be $dpid"
awk '$3 == "reliefd" || $3 == "camp" { if ($0 !~ / [RSD] \((running|sleeping|disk)/) bad = 1 } END { exit bad }' <<< "$psout" \
    || fail "unexpected process state in: $psout"
threads=$(awk '/^Threads:/ { print $2 }' "/proc/$dpid/status")
awk -v d="$dpid" -v t="$threads" '$1 == d && $3 == "reliefd" { found = 1; if ($0 !~ (") +" t " ")) exit 1 } END { exit !found }' <<< "$psout" \
    || fail "ps THREADS for reliefd should be $threads"
c2=$(awk '$1 == 2 { print $2 }' "$PIDS")
kill -STOP "$c2"
grep -qE "^ +$c2 .* T \(stopped\)" <<< "$("$CTL" ps)" || { kill -CONT "$c2"; fail "SIGSTOPped camp should show T (stopped)"; }
kill -CONT "$c2"
pass "ps: 9 processes with PCB fields from /proc, camps' PPID = reliefd, SIGSTOP shows T (stopped)"

tree=$("$CTL" tree) || fail "reliefctl tree"
first=$(sed -n '2p' <<< "$tree" | awk '{ print $1 }')
[[ "$first" == "$dpid" ]] || fail "tree should be rooted at reliefd ($dpid), got $first"
[[ $(grep -c '\\_ ' <<< "$tree") -eq 8 ]] || fail "tree should show 8 children"
pass "tree: rooted at reliefd $dpid with 8 children"

# 4. report windows.
wait_for "some requests in the log" grep -q ' event=request ' "$RELIEF_HOME/logs/reliefd.log"
"$CTL" stop >/dev/null
sleep 1.1   # timestamps have 1 s resolution in --since comparisons
grep -q 'no requests in this window' <<< "$("$CTL" report --since 0)" || fail "report --since 0 should be empty"
all=$("$CTL" report | sed -n 's/^  requests: \([0-9]*\) .*/\1/p')
recent=$("$CTL" report --since=60 | sed -n 's/^  requests: \([0-9]*\) .*/\1/p')
last=$("$CTL" report --last-run | sed -n 's/^  requests: \([0-9]*\) .*/\1/p')
[[ $all -gt 200 && $recent -eq $all && $last -lt $all && $last -gt 0 ]] \
    || fail "report windows: all=$all since60=$recent last-run=$last"
pass "report: --since 0 empty, --since=60 = whole log ($all), --last-run only the last run ($last)"

# 5. stockcheck.sh
expect_rc 0 "stockcheck below threshold" "$STOCK" -t 100000 -w 60
expect_rc 1 "stockcheck above threshold" "$STOCK" -t 0 -w 60
grep -q 'WARN stockcheck: [0-9]* requests in the last 60 min (threshold 0)' "$RELIEF_HOME/logs/stockcheck.log" \
    || fail "stockcheck did not log its warning"
expect_rc 2 "stockcheck bad option" "$STOCK" -x
pass "stockcheck.sh: OK below threshold, WARN logged + exit 1 above it"

echo "all cli tests passed"
