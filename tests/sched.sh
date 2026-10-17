#!/usr/bin/env bash
# OS concept: CPU scheduling and scheduling criteria — unit tests + simulator checks — Unit 2
#
#   1. build/test_sched: textbook answers for FCFS (17), SJF (7.75), SRTF (6.5), RR q=4 (5.66),
#      priority (preemptive and not), MLFQ demotion, starvation under prio vs aging in MLFQ
#   2. schedsim on the flood scenario: every request completes, metrics agree with the
#      per-request CSV, FCFS starts requests in arrival order, output is deterministic
#   3. every policy on the flood scenario: complete and self-consistent, non-preemptive
#      policies never split a request, and the expected orderings hold (SJF beats FCFS on
#      average wait, priority cuts CRITICAL waits, RR switches more, MLFQ logs promotions)
#   4. --gantt draws one 60-minute row per team; usage errors exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for every binary, e.g. valgrind
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
SCENARIO="$ROOT/scenarios/flood_pune.csv"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
schedsim() { "${WRAP[@]}" "$BIN/schedsim" "$@"; }
expect_rc() {   # expect_rc <rc> <description> <command...>
    local want=$1 desc=$2 rc=0
    shift 2
    "$@" >/dev/null 2>&1 || rc=$?
    [[ $rc -eq $want ]] || fail "$desc: expected exit $want, got $rc"
}

# 1. Unit tests.
"${WRAP[@]}" "$BIN/test_sched" > "$TMP/unit.out" 2>&1 || { cat "$TMP/unit.out" >&2; fail "test_sched"; }
pass "$(tail -n 1 "$TMP/unit.out") (textbook FCFS 17, SJF 7.75, SRTF 6.5, RR 5.66, prio, MLFQ aging)"

# 2. Flood scenario on 3 teams.
out=$(schedsim --policy fcfs --scenario "$SCENARIO" --teams 3 --csv "$TMP/jobs.csv") || fail "schedsim exited non-zero"
rows=$(awk 'NR > 1' "$SCENARIO" | wc -l)
[[ $(awk 'NR > 1' "$TMP/jobs.csv" | wc -l) -eq $rows ]] || fail "csv should have $rows rows"
awk -F, 'NR > 1 && ($6 < $4 || $7 != $6 + $5 || $8 != $9 - $5 || $10 != $6 - $4) { bad++ } END { exit bad > 0 }' "$TMP/jobs.csv" \
    || fail "per-request csv inconsistent (start >= arrival, finish = start + burst for FCFS, wait = turnaround - burst)"
csv_avg=$(awk -F, 'NR > 1 { w += $8; n++ } END { printf "%.1f", w / n }' "$TMP/jobs.csv")
grep -qE "^  all +$rows +$csv_avg " <<< "$out" || fail "reported average wait does not match the csv ($csv_avg)"
csv_max=$(awk -F, 'NR > 1 && $8 > m { m = $8 } END { print m + 0 }' "$TMP/jobs.csv")
grep -q "worst-case wait     $csv_max min" <<< "$out" || fail "worst-case wait should be $csv_max"
starved=$(awk -F, 'NR > 1 && $8 > 120 { n++ } END { print n + 0 }' "$TMP/jobs.csv")
grep -q "starved (wait > 120) $starved\$" <<< "$out" || fail "starved count should be $starved"
pass "schedsim fcfs, 3 teams: $rows requests complete; avg wait $csv_avg, max $csv_max, starved $starved agree with the csv"

# FCFS: requests start in arrival order (ties in scenario order = id's line number).
awk -F, 'NR > 1 { print $4, $1 % 1000000, $6 }' "$TMP/jobs.csv" | sort -n -k1,1 -k2,2 |
    awk '$3 < prev { bad = 1 } { prev = $3 } END { exit bad }' || fail "FCFS started a later arrival before an earlier one"
pass "FCFS starts requests in arrival order"

# Deterministic: identical output on a second run, with and without --seed.
r1=$(schedsim --policy fcfs --scenario "$SCENARIO" --teams 3)
r2=$(schedsim --policy fcfs --scenario "$SCENARIO" --teams 3)
[[ "$r1" == "$r2" ]] || fail "second run differs"
[[ "$r1" == "$(sed '/^per-request results written/d; ${/^$/d}' <<< "$out")" ]] || fail "--csv changed the report"
s1=$(schedsim --policy fcfs --scenario "$SCENARIO" --teams 3 --seed 7)
s2=$(schedsim --policy fcfs --scenario "$SCENARIO" --teams 3 --seed 7)
[[ "$s1" == "$s2" ]] || fail "--seed 7 runs differ"
pass "deterministic: repeated runs (with and without --seed) give identical reports"

# 3. Every policy on the flood scenario.
# metric <report> <row> <column>: e.g. metric "$r" CRITICAL 3 = CRITICAL average wait
metric() { awk -v row="$2" -v col="$3" '$1 == row { print $col; exit }' <<< "$1"; }
line_value() { sed -n "s/^  $2 *\([0-9.]*\).*/\1/p" <<< "$1"; }
declare -A REP
for spec in fcfs sjf srtf rr prio:off prio:on mlfq; do
    name=${spec%%:*}
    args=(--policy "$name" --scenario "$SCENARIO" --teams 3 --csv "$TMP/$spec.csv")
    [[ "$spec" == *:* ]] && args+=(--preemptive "${spec#*:}")
    REP[$spec]=$(schedsim "${args[@]}" 2> "$TMP/$spec.err") || fail "schedsim $spec exited non-zero"
    [[ $(awk 'NR > 1' "$TMP/$spec.csv" | wc -l) -eq $rows ]] || fail "$spec: not all $rows requests completed"
    awk -F, 'NR > 1 && ($6 < $4 || $7 < $6 + $5 || $8 != $9 - $5 || $10 != $6 - $4) { bad++ } END { exit bad > 0 }' \
        "$TMP/$spec.csv" || fail "$spec: per-request csv inconsistent"
    avg=$(awk -F, 'NR > 1 { w += $8; n++ } END { printf "%.1f", w / n }' "$TMP/$spec.csv")
    [[ "$(metric "${REP[$spec]}" all 3)" == "$avg" ]] || fail "$spec: reported average wait differs from the csv ($avg)"
    case "$spec" in
        fcfs|sjf|prio:off)      # non-preemptive: a request runs in one piece
            awk -F, 'NR > 1 && $7 != $6 + $5 { bad++ } END { exit bad > 0 }' "$TMP/$spec.csv" \
                || fail "$spec is non-preemptive but split a request" ;;
    esac
done
pass "all 7 policy variants complete $rows requests; reports match their csv; non-preemptive ones never split a request"

w() { metric "${REP[$1]}" all 3; }
cw() { metric "${REP[$1]}" CRITICAL 3; }
cs() { line_value "${REP[$1]}" 'context switches'; }
lt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a < b) }'; }
le() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a <= b) }'; }
lt "$(w sjf)" "$(w fcfs)" || fail "SJF should beat FCFS on average wait ($(w sjf) vs $(w fcfs))"
le "$(w srtf)" "$(w sjf)" || fail "SRTF should not lose to SJF on average wait ($(w srtf) vs $(w sjf))"
lt "$(cw prio:off)" "$(cw fcfs)" || fail "priority should cut CRITICAL waits ($(cw prio:off) vs $(cw fcfs))"
le "$(cw prio:on)" "$(cw prio:off)" || fail "preemptive priority should not slow CRITICAL ($(cw prio:on) vs $(cw prio:off))"
lt "$(cs fcfs)" "$(cs rr)" || fail "RR should context-switch more than FCFS ($(cs rr) vs $(cs fcfs))"
pass "avg wait: SRTF $(w srtf) <= SJF $(w sjf) < FCFS $(w fcfs); CRITICAL wait: prio-on $(cw prio:on) <= prio $(cw prio:off) < FCFS $(cw fcfs); switches: RR $(cs rr) > FCFS $(cs fcfs)"

promos=$(line_value "${REP[mlfq]}" 'promotions (aging)')
logged=$(grep -c ' promote request ' "$TMP/mlfq.err" || true)
[[ -n "$promos" && $promos -gt 0 && $logged -eq $promos ]] \
    || fail "mlfq: report says ${promos:-?} promotions, stderr logged $logged"
pass "mlfq logged every promotion on stderr ($logged, matching the report)"

[[ "$(schedsim --policy mlfq --scenario "$SCENARIO" --teams 3 2>&1)" == \
   "$(schedsim --policy mlfq --scenario "$SCENARIO" --teams 3 2>&1)" ]] || fail "mlfq runs differ"
pass "mlfq output (report and promotion log) is deterministic"

# 4. Gantt chart.
gantt=$(schedsim --policy fcfs --scenario "$SCENARIO" --teams 3 --gantt)
[[ $(grep -cE '^team [0-9]+ +[CUR.cur]{60}$' <<< "$gantt") -eq 3 ]] || fail "--gantt should draw 3 rows of 60 minutes"
pass "--gantt draws one 60-minute row per team"

expect_rc 2 "missing --policy" schedsim --scenario "$SCENARIO"
expect_rc 2 "unknown policy" schedsim --policy nope
expect_rc 2 "--teams 0" schedsim --policy fcfs --teams 0
expect_rc 2 "--preemptive maybe" schedsim --policy prio --preemptive maybe
expect_rc 2 "--age 0" schedsim --policy mlfq --age 0
expect_rc 1 "missing scenario file" schedsim --policy fcfs --scenario "$TMP/none.csv"
expect_rc 0 "--help" schedsim --help
pass "usage errors exit 2, a missing scenario exits 1"

echo "all scheduler tests passed"
