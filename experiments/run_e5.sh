#!/usr/bin/env bash
# OS concept: Memory allocation strategies and fragmentation — experiment E5 (warehouse floor) — Unit 4
#
# E5: build/memsim runs all five allocators (first, best, worst fit, buddy, slab) on the same
# 2000-operation shipment trace of a 1024-unit warehouse floor, for 5 seeds. One CSV row per
# allocator and seed goes to experiments/results/e5.csv.
#
#   usage: run_e5.sh [-s "1 2 3 4 5"] [-n OPS]
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Deterministic: the same seeds give a byte-identical CSV.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

seeds="1 2 3 4 5"
ops=2000
while getopts ":s:n:h" opt; do
    case "$opt" in
        s) seeds=$OPTARG ;;
        n) ops=$OPTARG ;;
        h) sed -n 's/^#   //p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ -x "$BIN/memsim" ]] || { echo "$PROG: $BIN/memsim not built; run 'make'" >&2; exit 1; }

mkdir -p "$RESULTS"
OUT="$RESULTS/e5.csv"
rm -f "$OUT"
echo "E5: 1024-unit floor, $ops operations, seeds: $seeds"
for seed in $seeds; do
    for a in first best worst buddy slab; do
        "$BIN/memsim" --alloc "$a" --seed "$seed" --ops "$ops" --csv "$OUT" > /dev/null
    done
done

awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { k = $c["alloc"]; if (!(k in n)) order[++m] = k
      n[k]++; ext[k] += $c["ext_frag_avg"]; int_[k] += $c["int_frag_avg"]
      fr[k] += 100 * $c["failures"] / $c["allocs"]; fs[k] += $c["failures_with_space"]; f[k] += $c["failures"]
      occ[k] += $c["occupied_avg"]; req[k] += $c["requested_avg"] }
    END {
        printf "\n%-6s %9s %9s %10s %15s %9s %10s\n", "alloc", "ext frag", "int frag", "failures", "with free space",
               "occupied", "requested"
        for (i = 1; i <= m; i++) { k = order[i]
            printf "%-6s %8.1f%% %8.1f%% %9.1f%% %14.0f%% %8.1f%% %9.1f%%\n", k, 100 * ext[k] / n[k], 100 * int_[k] / n[k],
                   fr[k] / n[k], f[k] ? 100 * fs[k] / f[k] : 0, 100 * occ[k] / n[k], 100 * req[k] / n[k] }
        print "(means over seeds; failures = % of allocation requests; with free space = share of failures where the"
        print " free units in total would have been enough)"
    }' "$OUT"
echo
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs)"
