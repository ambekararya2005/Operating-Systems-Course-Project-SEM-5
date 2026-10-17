#!/usr/bin/env bash
# OS concept: CPU scheduling — Phase 2 gate M2 consistency check across every policy — Unit 2
#
# Runs build/schedsim for every policy variant on a small generated scenario (40 requests,
# 400 simulated minutes, 3 teams: about 44% load) and asserts, per variant:
#   - every request completes exactly once (same id set as the scenario, no duplicates)
#   - no request finishes before arrival + burst, or starts before it arrives
#   - the report agrees with the per-request CSV
#   - two runs with the same seed are byte-identical (report and CSV)
# and that mlfq reports 0 starved requests (wait > 120 min).
#
# The zero-starvation assertion is only valid at LIGHT load, which is why this scenario is
# lightly loaded. Under sustained overload MLFQ does starve requests (see E1 in
# docs/EXPERIMENTS.md, and 8-9 starved at 80% load on a similar 60-request scenario).
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind
# Exit status: 0 if every check passed, 1 otherwise.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
SCENARIO="$TMP/phase2.csv"
SEED=7
FAILED=0

pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; FAILED=1; }
schedsim() { "${WRAP[@]}" "$BIN/schedsim" "$@"; }

python3 -I "$ROOT/experiments/gen_scenario.py" --rows 40 --duration 400 --seed 3 -o "$SCENARIO"
# Expected request ids, exactly as schedsim derives them: camp_id * 1000000 + CSV line number.
awk -F, 'NR > 1 { print $2 * 1000000 + NR }' "$SCENARIO" | sort -n > "$TMP/expected_ids"
rows=$(wc -l < "$TMP/expected_ids")

# label|options
VARIANTS=(
    "fcfs|--policy fcfs"
    "sjf|--policy sjf"
    "srtf|--policy srtf"
    "rr|--policy rr --quantum 10"
    "prio|--policy prio --preemptive off"
    "prio-preempt|--policy prio --preemptive on"
    "mlfq|--policy mlfq --age 60"
)

for v in "${VARIANTS[@]}"; do
    label=${v%%|*}
    read -r -a opts <<< "${v#*|}"
    for run in 1 2; do
        schedsim "${opts[@]}" --scenario "$SCENARIO" --teams 3 --seed "$SEED" \
            --csv "$TMP/$label.$run.csv" > "$TMP/$label.$run.out" 2>"$TMP/$label.$run.err" \
            || { fail "$label: schedsim exited non-zero"; continue 2; }
        sed -i '/^per-request results written/d' "$TMP/$label.$run.out"
    done
    csv="$TMP/$label.1.csv"

    # Exactly once: the completed ids are the scenario's ids, each once.
    if diff -q <(awk -F, 'NR > 1 { print $1 }' "$csv" | sort -n) "$TMP/expected_ids" >/dev/null; then
        pass "$label: all $rows requests completed exactly once"
    else
        fail "$label: completed ids differ from the scenario (missing or duplicated requests)"
    fi

    # Timing sanity: start >= arrival, finish >= arrival + burst, wait = turnaround - burst.
    bad=$(awk -F, 'NR > 1 && ($6 < $4 || $7 < $4 + $5 || $8 != $9 - $5) { n++ } END { print n + 0 }' "$csv")
    if [[ $bad -eq 0 ]]; then
        pass "$label: no request finished before arrival + burst (or started before arriving)"
    else
        fail "$label: $bad requests violate start >= arrival / finish >= arrival + burst"
    fi

    # The report agrees with the CSV.
    avg=$(awk -F, 'NR > 1 { w += $8; n++ } END { printf "%.1f", w / n }' "$csv")
    if grep -qE "^  all +$rows +$avg " "$TMP/$label.1.out"; then
        pass "$label: reported average wait matches the CSV ($avg)"
    else
        fail "$label: reported average wait does not match the CSV ($avg)"
    fi

    # Deterministic for a given seed.
    if cmp -s "$TMP/$label.1.out" "$TMP/$label.2.out" && cmp -s "$csv" "$TMP/$label.2.csv" \
       && cmp -s "$TMP/$label.1.err" "$TMP/$label.2.err"; then
        pass "$label: two runs with seed $SEED are identical (report, CSV, event log)"
    else
        fail "$label: runs with the same seed differ"
    fi
done

# MLFQ starvation at light load.
starved=$(sed -n 's/^  starved (wait > 120) \([0-9]*\)$/\1/p' "$TMP/mlfq.1.out")
csv_starved=$(awk -F, 'NR > 1 && $8 > 120 { n++ } END { print n + 0 }' "$TMP/mlfq.1.csv")
if [[ "$starved" == 0 && "$csv_starved" == 0 ]]; then
    pass "mlfq: 0 starved requests (wait > 120 min) at ~44% load (report and CSV agree)"
else
    fail "mlfq: expected 0 starved, report says '${starved:-?}', CSV has $csv_starved"
fi

if [[ $FAILED -ne 0 ]]; then
    echo "PHASE 2 CHECK: FAILED" >&2
    exit 1
fi
echo "PHASE 2 CHECK: PASSED (${#VARIANTS[@]} policy variants consistent and deterministic)"
