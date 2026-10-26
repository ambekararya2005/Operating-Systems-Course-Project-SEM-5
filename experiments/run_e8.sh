#!/usr/bin/env bash
# OS concept: Disk scheduling as delivery routing — FCFS, SSTF, SCAN, C-SCAN, LIFO — experiment E8 — Unit 5
#
# E8: build/routesim plans a truck's route through 20 random delivery stops on a 0-199 km
# highway (random start, moving up), for 5 seeds, under each policy. One CSV row per policy
# and seed goes to experiments/results/e8.csv.
#
#   usage: run_e8.sh [-s "1 2 3 4 5"] [-n STOPS]
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Deterministic: the same seeds give a byte-identical CSV.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

seeds="1 2 3 4 5"
stops=20
while getopts ":s:n:h" opt; do
    case "$opt" in
        s) seeds=$OPTARG ;;
        n) stops=$OPTARG ;;
        h) sed -n 's/^#   //p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ -x "$BIN/routesim" ]] || { echo "$PROG: $BIN/routesim not built; run 'make'" >&2; exit 1; }

mkdir -p "$RESULTS"
OUT="$RESULTS/e8.csv"
rm -f "$OUT"
echo "E8: $stops random stops on km 0-199, seeds: $seeds"
for seed in $seeds; do
    for p in fcfs sstf scan cscan lifo; do
        "$BIN/routesim" --policy "$p" --seed "$seed" --count "$stops" --quiet --csv "$OUT" > /dev/null
    done
done

awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { k = $c["policy"]; if (!(k in n)) order[++m] = k
      n[k]++; t[k] += $c["total_km"]; w[k] += $c["worst_wait"]; a[k] += $c["avg_wait"]; j[k] += $c["jump_km"]; d[k] += $c["worst_detour"]
      if ($c["worst_detour"] > dmax[k]) dmax[k] = $c["worst_detour"] }
    END {
        printf "\n%-6s %10s %12s %10s %12s %18s\n", "policy", "total km", "of it jump", "avg wait", "worst wait", "worst detour (max)"
        for (i = 1; i <= m; i++) { k = order[i]
            printf "%-6s %10.1f %12.1f %10.1f %12.1f %12.1f (%d)\n", k, t[k] / n[k], j[k] / n[k], a[k] / n[k], w[k] / n[k], d[k] / n[k], dmax[k] }
        print "(means over seeds; wait = km driven before a stop is reached; detour = wait - direct km from the start)"
    }' "$OUT"
echo
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs)"
