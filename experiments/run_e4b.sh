#!/usr/bin/env bash
# OS concept: The readers-writers problem — writer starvation, readers preference (semaphores)
#             vs writer preference (pthread rwlock) — experiment E4b — Unit 3
#
# E4b: build/rwlab with 8 readers and 1 writer, R runs of each readers-writers kind (runs
# interleaved), then a per-kind summary of the writer's waits and the reads/writes done.
#
#   usage: run_e4b.sh [-r RUNS] [-s SECONDS]
#     -r  runs per kind (default 5)
#     -s  seconds per run (default 2)
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Output: $RESULTS/e4b.csv, one row per kind and run. Real measurements: they vary between runs.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

runs=5
secs=2
while getopts ":r:s:h" opt; do
    case "$opt" in
        r) runs=$OPTARG ;;
        s) secs=$OPTARG ;;
        h) sed -n 's/^#   //p; s/^#     /  /p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ $runs =~ ^[1-9][0-9]*$ ]] || { echo "$PROG: -r must be a positive integer" >&2; exit 2; }
[[ -x "$BIN/rwlab" ]] || { echo "$PROG: $BIN/rwlab not built; run 'make'" >&2; exit 1; }

mkdir -p "$RESULTS"
OUT="$RESULTS/e4b.csv"
rm -f "$OUT"
echo "E4b: $(nproc) CPUs, kernel $(uname -r), rwlab 8 readers (100 us reads) + 1 writer (20 us writes, every 2 ms), $secs s x $runs runs"
for r in $(seq "$runs"); do
    for k in semaphore rwlock; do
        "$BIN/rwlab" --rw "$k" --seconds "$secs" --csv "$OUT" --run "$r" > /dev/null
    done
done

awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
    { k = $c["rw"]; if (!(k in n)) order[++m] = k
      n[k]++; wr[k] += $c["writes_during_run"]; rd[k] += $c["reads"]; avg[k] += $c["writer_wait_avg_ms"]
      if ($c["writer_wait_max_ms"] > wmax[k]) wmax[k] = $c["writer_wait_max_ms"]
      if ($c["reader_wait_max_ms"] > rmax[k]) rmax[k] = $c["reader_wait_max_ms"]
      torn[k] += $c["torn_reads"] }
    END {
        printf "\n%-10s %16s %18s %18s %14s %18s %6s\n", "kind", "writes/run (mean)", "writer wait avg ms",
               "writer wait max ms", "reads/run", "reader wait max ms", "torn"
        for (i = 1; i <= m; i++) { k = order[i]
            printf "%-10s %16.1f %18.3f %18.3f %14.0f %18.3f %6d\n", k, wr[k] / n[k], avg[k] / n[k], wmax[k],
                   rd[k] / n[k], rmax[k], torn[k] }
    }' "$OUT"
echo
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs)"
