#!/usr/bin/env bash
# OS concept: Write-ahead journal with fsync, crash recovery — experiment E9 — Unit 5
#
# E9: what the journal buys and what it costs.
#   1. Cost: reliefd runs the flood scenario to the end (8 camps, --time-scale 5,
#      --exit-when-done) with and without the journal, -r times each. Every grant line carries
#      commit_us (Banker's decision -> journal write + fsync -> camp told). One row per run to
#      experiments/results/e9_cost.csv.
#   2. Crashes: tests/crash_test.sh (kill -9 at 10 random moments, restart, compare) with and
#      without the journal; one row per crash to experiments/results/e9_crash.csv.
#
#   usage: run_e9.sh [-r RUNS] [-s SEED]
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Real time and a real disk: the kill moments are deterministic for the seed, the fsync times
# and therefore how far each run got before a kill are not.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

runs=3
seed=1
while getopts ":r:s:h" opt; do
    case "$opt" in
        r) runs=$OPTARG ;;
        s) seed=$OPTARG ;;
        h) sed -n 's/^#   //p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ -x "$BIN/reliefd" ]] || { echo "$PROG: $BIN/reliefd not built; run 'make'" >&2; exit 1; }
for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    [[ ! -e "$f" ]] || { echo "$PROG: $f exists (is reliefd running?). Stop it first." >&2; exit 1; }
done

mkdir -p "$RESULTS"
COST="$RESULTS/e9_cost.csv"
CRASH="$RESULTS/e9_crash.csv"
rm -f "$COST" "$CRASH"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-e9.XXXXXX")
DPID=""
trap '[[ -n "$DPID" ]] && kill -TERM "$DPID" 2>/dev/null; rm -rf "$WORK"' EXIT

echo "E9 part 1: time per grant, flood scenario to the end, $runs runs per mode"
echo "mode,run,grants,mean_us,p50_us,p99_us,max_us,fsync_avg_us,run_s" > "$COST"
for ((r = 1; r <= runs; r++)); do
    for mode in journal none; do
        export RELIEF_HOME="$WORK/$mode$r"
        mkdir -p "$RELIEF_HOME/logs"
        LOG="$RELIEF_HOME/logs/reliefd.log"
        extra=()
        [[ $mode == none ]] && extra=(--no-journal)
        t0=$(date +%s.%N)
        "$BIN/reliefd" --time-scale 5 --exit-when-done --seed "$seed" "${extra[@]}" \
            >> "$RELIEF_HOME/logs/reliefd.out" 2>&1 &
        DPID=$!
        for _ in $(seq 600); do         # every camp done (BYE), at most 60 s
            [[ $(grep -c ' event=bye ' "$LOG" 2>/dev/null || true) -ge 8 ]] && break
            sleep 0.1
        done
        t1=$(date +%s.%N)
        kill -TERM "$DPID"
        wait "$DPID" || { echo "$PROG: reliefd ($mode) failed" >&2; exit 1; }
        DPID=""
        fs=$(sed -n 's/.* event=journal_stop .*fsync_avg_us=\([0-9.]*\).*/\1/p' "$LOG")
        sed -n 's/.* event=grant .*commit_us=\([0-9.]*\).*/\1/p' "$LOG" | sort -n |
            awk -v m="$mode" -v r="$r" -v fs="${fs:-0}" -v s="$(awk -v a="$t0" -v b="$t1" 'BEGIN { print b - a }')" '
                { v[NR] = $1; sum += $1 }
                END { p50 = v[int((NR + 1) / 2)]; p99 = v[int(NR * 0.99 + 0.999)]
                      printf "%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f\n", m, r, NR, sum / NR, p50, p99, v[NR], fs, s }' >> "$COST"
    done
done
awk -F, 'NR > 1 { n[$1]++; g[$1] += $3; mu[$1] += $4; p50[$1] += $5; p99[$1] += $6; fs[$1] += $8; s[$1] += $9 }
    END { printf "\n%-8s %7s %10s %9s %9s %13s %8s\n", "mode", "grants", "mean_us", "p50_us", "p99_us", "fsync_avg_us", "run_s"
          for (m in n) printf "%-8s %7.0f %10.1f %9.1f %9.1f %13.1f %8.2f\n", m, g[m] / n[m], mu[m] / n[m], p50[m] / n[m],
                                p99[m] / n[m], fs[m] / n[m], s[m] / n[m]
          print "(means over runs; commit_us = Banker'"'"'s decision -> journal write + fsync -> camp told)" }' "$COST"

echo
echo "E9 part 2: kill -9 at 10 random moments, with and without the journal (tests/crash_test.sh)"
CRASH_CSV="$CRASH" CRASH_SEED="$seed" BIN_DIR="${BIN_DIR:-build}" "$ROOT/tests/crash_test.sh" | grep '^ok'
awk -F, 'NR > 1 { n[$1]++; a[$1] += $6; l[$1] += $8 }
    END { printf "\n%-8s %7s %18s %11s\n", "mode", "crashes", "units held (acked)", "units lost"
          for (m in n) printf "%-8s %7d %18d %11d\n", m, n[m], a[m], l[m] }' "$CRASH"
echo
echo "wrote $COST and $CRASH"
