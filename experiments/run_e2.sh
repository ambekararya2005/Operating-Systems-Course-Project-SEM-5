#!/usr/bin/env bash
# OS concept: Linux scheduling (CFS/EEVDF + nice, SCHED_FIFO, SCHED_RR) vs simulated
#             textbook policies — experiment E2 — Unit 2
#
# E2: run one rescue scenario as real CPU-bound threads under each Linux scheduling mode
# (build/linuxsched, pinned to one CPU), and the same scenario through our simulated prio
# and mlfq with 1 team (build/schedsim --teams 1), so both sides have one CPU.
#
#   usage: run_e2.sh [-k SCALE_MS] [-m "cfs-flat cfs-nice fifo rr"]
#     -k  real ms of CPU per simulated minute (default 4: about 7 s of CPU in total)
#     -m  Linux modes to run (default all four). fifo and rr need CAP_SYS_NICE:
#           sudo setcap cap_sys_nice+ep build/linuxsched     (lost on every rebuild)
#         Without it they are skipped with a message; the other results are still written.
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Outputs in $RESULTS: e2_scenario.csv, e2_<mode>.csv (one row per thread), e2_sim.csv
# (schedsim summary rows). The scenario and the simulated rows are deterministic; the Linux
# runs are real measurements and vary a little from run to run.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

scale=4
modes="cfs-flat cfs-nice fifo rr"
while getopts ":k:m:h" opt; do
    case "$opt" in
        k) scale=$OPTARG ;;
        m) modes=$OPTARG ;;
        h) sed -n 's/^#   //p; s/^#     /  /p; s/^#         /    /p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
for b in linuxsched schedsim; do
    [[ -x "$BIN/$b" ]] || { echo "$PROG: $BIN/$b not built; run 'make'" >&2; exit 1; }
done

mkdir -p "$RESULTS"
SCENARIO="$RESULTS/e2_scenario.csv"
# 120 requests, rush hour 60-120: about 1,800 simulated minutes of work in 2,000 minutes,
# i.e. ~90% load on ONE CPU, with a backlog after the rush.
python3 -I "$ROOT/experiments/gen_scenario.py" --rows 120 --seed 1 --duration 2000 --rush 60-120 \
    -o "$SCENARIO"
echo "E2: kernel $(uname -r), scenario $SCENARIO, $scale ms per simulated minute"

skipped=()
for m in $modes; do
    rm -f "$RESULTS/e2_$m.csv"
    rc=0
    "$BIN/linuxsched" --mode "$m" --scenario "$SCENARIO" --scale-ms "$scale" \
        --out "$RESULTS/e2_$m.csv" || rc=$?
    case $rc in
        0) ;;
        3) skipped+=("$m") ;;       # no CAP_SYS_NICE: message already printed
        *) echo "$PROG: linuxsched --mode $m failed (exit $rc)" >&2; exit 1 ;;
    esac
    echo
done

# The simulated counterparts on the same scenario and the same single CPU.
SIM="$RESULTS/e2_sim.csv"
rm -f "$SIM"
for v in "sim-prio|--policy prio --preemptive off" "sim-prio-preempt|--policy prio --preemptive on" \
         "sim-mlfq|--policy mlfq --age 60"; do
    read -r -a opts <<< "${v#*|}"
    "$BIN/schedsim" "${opts[@]}" --scenario "$SCENARIO" --teams 1 --seed 1 \
        --summary-csv "$SIM" --label "${v%%|*}" > /dev/null 2>&1
done

# Side-by-side per-severity averages (simulated minutes).
echo "per-severity averages (simulated minutes): response = arrival -> first run, wait = time not running"
printf '%-18s %10s %10s %10s   %10s %10s %10s\n' variant "resp CRIT" "resp URG" "resp ROUT" \
    "wait CRIT" "wait URG" "wait ROUT"
for m in $modes; do
    f="$RESULTS/e2_$m.csv"
    [[ -r "$f" ]] || continue
    awk -F, -v m="linux-$m" 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
        { s = $c["severity"]; n[s]++; r[s] += $c["response_min"]; w[s] += $c["wait_min"] }
        END { printf "%-18s %10.1f %10.1f %10.1f   %10.1f %10.1f %10.1f\n", m,
              r["CRITICAL"] / n["CRITICAL"], r["URGENT"] / n["URGENT"], r["ROUTINE"] / n["ROUTINE"],
              w["CRITICAL"] / n["CRITICAL"], w["URGENT"] / n["URGENT"], w["ROUTINE"] / n["ROUTINE"] }' "$f"
done
awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { printf "%-18s %10.1f %10.1f %10.1f   %10.1f %10.1f %10.1f\n", $c["label"],
      $c["critical_avg_response"], $c["urgent_avg_response"], $c["routine_avg_response"],
      $c["critical_avg_wait"], $c["urgent_avg_wait"], $c["routine_avg_wait"] }' "$SIM"

if (( ${#skipped[@]} )); then
    echo
    echo "skipped (need CAP_SYS_NICE): ${skipped[*]}"
    echo "  grant it with:  sudo setcap cap_sys_nice+ep $BIN/linuxsched   then rerun"
fi
