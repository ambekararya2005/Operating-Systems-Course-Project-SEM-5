#!/usr/bin/env bash
# OS concept: Deadlock prevention, avoidance, detection and recovery compared — experiment E3 — Unit 3
#
# E3: build/dlsim (deterministic, one-minute clock) runs every deadlock mode on
#   - 5 seeds of a 300-request vehicle-heavy task mix (experiments/gen_deadlock.py --rows 300,
#     8 camps, tasks start within 1200 minutes; default resources: 3 trucks, 2 boats,
#     40 MED and 60 FOOD crates), and
#   - scenarios/deadlock.csv (2 trucks, 2 boats: scenarios/deadlock_resources.conf).
# Modes: none, prevent (resource ordering), prevent-all (all-or-nothing), avoid (Banker's),
# detect (every 5 minutes, recovery by preemption).
#
#   usage: run_e3.sh [-s "1 2 3 4 5"] [-n ROWS]
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Outputs in $RESULTS: e3.csv (one row per mode and scenario), e3_scenario_s<seed>.csv, and
# e3_timeline_none.csv / e3_timeline_detect.csv (seed 1, one row per simulated minute).
# Deterministic: the same seeds give byte-identical files.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

seeds="1 2 3 4 5"
rows=300
while getopts ":s:n:h" opt; do
    case "$opt" in
        s) seeds=$OPTARG ;;
        n) rows=$OPTARG ;;
        h) sed -n 's/^#   //p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ -x "$BIN/dlsim" ]] || { echo "$PROG: $BIN/dlsim not built; run 'make'" >&2; exit 1; }

MODES=(
    "none|--deadlock none"
    "prevent|--deadlock prevent"
    "prevent-all|--deadlock prevent --all-or-nothing"
    "avoid|--deadlock avoid"
    "detect|--deadlock detect --detect-interval 5"
)
mkdir -p "$RESULTS"
OUT="$RESULTS/e3.csv"
rm -f "$OUT"

run() {     # run <scenario> <seed> <extra dlsim args...>
    local scen=$1 seed=$2 v label
    shift 2
    for v in "${MODES[@]}"; do
        label=${v%%|*}
        read -r -a opts <<< "${v#*|}"
        "$BIN/dlsim" "${opts[@]}" --scenario "$scen" --seed "$seed" --summary-csv "$OUT" --label "$label" "$@" \
            > /dev/null || { echo "$PROG: dlsim $label failed on $scen" >&2; exit 1; }
    done
}

echo "E3: deadlock modes, $rows-request task mix x seeds $seeds, plus scenarios/deadlock.csv"
for seed in $seeds; do
    scen="$RESULTS/e3_scenario_s$seed.csv"
    python3 -I "$ROOT/experiments/gen_deadlock.py" --rows "$rows" --camps 8 --seed "$seed" --duration 1200 -o "$scen"
    run "$scen" "$seed"
done
run "$ROOT/scenarios/deadlock.csv" 0 --resources "$ROOT/scenarios/deadlock_resources.conf"

first=${seeds%% *}
"$BIN/dlsim" --deadlock none --scenario "$RESULTS/e3_scenario_s$first.csv" --seed "$first" \
    --timeline "$RESULTS/e3_timeline_none.csv" > /dev/null
"$BIN/dlsim" --deadlock detect --detect-interval 5 --scenario "$RESULTS/e3_scenario_s$first.csv" --seed "$first" \
    --timeline "$RESULTS/e3_timeline_detect.csv" > /dev/null

awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    $c["scenario"] != "deadlock.csv" {
      k = $c["label"]; if (!(k in n)) order[++m] = k
      n[k]++; done[k] += $c["completed"]; req[k] += $c["requests"]; dl[k] += $c["deadlocks"]; fr[k] += $c["frozen"]
      thr[k] += $c["throughput_per_hour"]; avg[k] += $c["avg_delay"]; if ($c["max_delay"] > mx[k]) mx[k] = $c["max_delay"]
      un[k] += $c["unsafe_denials"]; pre[k] += $c["preemptions"]; ord[k] += $c["order_rejects"]
      ut[k] += $c["vehicle_utilisation"]; idl[k] += $c["vehicle_idle_held"] }
    $c["scenario"] == "deadlock.csv" { small[$c["label"]] = $c["completed"] "/" $c["requests"] (($c["frozen"] == 1) ? " frozen" : "") }
    END {
        printf "\n%-12s %12s %10s %7s %10s %10s %9s %8s %9s %8s %7s %7s  %s\n", "mode", "completed", "deadlocks",
               "frozen", "req/hour", "avg delay", "max delay", "unsafe", "preempts", "order", "busy%", "idle%",
               "deadlock.csv"
        for (i = 1; i <= m; i++) { k = order[i]
            printf "%-12s %8.1f/%-3d %10.1f %5d/%-1d %10.2f %10.1f %9d %8.1f %9.1f %8.1f %7.1f %7.1f  %s\n", k,
                   done[k] / n[k], req[k] / n[k], dl[k] / n[k], fr[k], n[k], thr[k] / n[k], avg[k] / n[k], mx[k],
                   un[k] / n[k], pre[k] / n[k], ord[k] / n[k], 100 * ut[k] / n[k], 100 * idl[k] / n[k], small[k] }
        print "(means over seeds; max delay = worst over seeds; busy/idle = vehicle time on complete tasks / held by waiting tasks)"
    }' "$OUT"
echo
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs) and the timelines"
