#!/usr/bin/env bash
# OS concept: Page replacement — faults vs number of frames, FIFO / LRU / Optimal / expiry-first — experiment E6 — Unit 4
#
# E6: one perishables trace (build/pagesim --seed, 300 references over 12 items with shelf
# lives of 5-60, a moving working set of 4 items) replayed with 1-10 shelf slots (frames) under
# each policy. One CSV row per policy and frame count goes to experiments/results/e6.csv; the
# trace itself to experiments/results/e6_trace.csv.
#
#   usage: run_e6.sh [-s SEED] [-n LENGTH]
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Deterministic: the same seed gives byte-identical files.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

seed=1
length=300
while getopts ":s:n:h" opt; do
    case "$opt" in
        s) seed=$OPTARG ;;
        n) length=$OPTARG ;;
        h) sed -n 's/^#   //p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ -x "$BIN/pagesim" ]] || { echo "$PROG: $BIN/pagesim not built; run 'make'" >&2; exit 1; }

mkdir -p "$RESULTS"
OUT="$RESULTS/e6.csv"
TRACE="$RESULTS/e6_trace.csv"
rm -f "$OUT"
"$BIN/pagesim" --policy fifo --seed "$seed" --length "$length" --write-trace "$TRACE" > /dev/null
echo "E6: perishables trace $TRACE ($length references, seed $seed), frames 1-10"
for frames in 1 2 3 4 5 6 7 8 9 10; do
    for p in fifo lru optimal expiry; do
        "$BIN/pagesim" --policy "$p" --frames "$frames" --trace "$TRACE" --csv "$OUT" > /dev/null
    done
done

awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { f = $c["frames"]; p = $c["policy"]; faults[f, p] = $c["faults"]; exp_[f, p] = $c["expired"]
      if (!(p in seen)) { seen[p] = 1; order[++m] = p } }
    END {
        printf "\n%-7s", "frames"
        for (i = 1; i <= m; i++) printf " %16s", order[i] " faults/exp"
        printf "\n"
        for (f = 1; f <= 10; f++) {
            printf "%-7d", f
            for (i = 1; i <= m; i++) printf " %11d / %-3d", faults[f, order[i]], exp_[f, order[i]]
            printf "\n"
        }
        print "(faults = stock-outs; exp = units that expired on the shelf)"
    }' "$OUT"
echo
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs)"
