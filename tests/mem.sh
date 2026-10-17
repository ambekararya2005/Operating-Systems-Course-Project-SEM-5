#!/usr/bin/env bash
# OS concept: Memory allocation — first/best/worst fit, buddy system, slab allocation — Unit 4
#
# 1. build/test_mem: the textbook first/best/worst-fit example, buddy split/merge on 1024 units,
#    slab caches
# 2. build/memsim, every allocator: deterministic for a seed (two runs identical); a trace
#    written with --write-trace and read back with --trace gives the same result; --show draws
#    a floor map of the right width; --csv writes one row per run; usage errors exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
memsim() { "${WRAP[@]}" "$BIN/memsim" "$@"; }
OPS=600
[[ "${RELIEF_WRAP:-}" == *valgrind* ]] && OPS=200

"${WRAP[@]}" "$BIN/test_mem" || fail "test_mem"

for a in first best worst buddy slab; do
    memsim --alloc "$a" --seed 7 --ops "$OPS" --csv "$TMP/rows.csv" > "$TMP/$a.1" || fail "memsim $a"
    memsim --alloc "$a" --seed 7 --ops "$OPS" --write-trace "$TMP/trace.csv" > "$TMP/$a.2" || fail "memsim $a (2)"
    cmp -s "$TMP/$a.1" "$TMP/$a.2" || fail "$a: two runs with seed 7 differ"
    memsim --alloc "$a" --trace "$TMP/trace.csv" > "$TMP/$a.3" || fail "memsim $a --trace"
    diff <(sed 1d "$TMP/$a.1") <(sed 1d "$TMP/$a.3") > /dev/null || fail "$a: the written trace replays differently"
    grep -q '^  allocation failures [0-9]* (' "$TMP/$a.1" || fail "$a: no failure line"
done
[[ $(wc -l < "$TMP/rows.csv") -eq 6 && $(head -1 "$TMP/rows.csv") == label,alloc,seed,* ]] || fail "CSV: $(cat "$TMP/rows.csv")"
pass "memsim: 5 allocators deterministic for a seed; --write-trace / --trace replay identically; CSV rows"

out=$(memsim --alloc buddy --seed 3 --ops 40 --show 20 --width 64)
maps=$(grep -c '^\[.*\]$' <<< "$out")
width=$(grep -m1 '^\[' <<< "$out" | tr -d '[]' | tr -d '\n' | wc -c)
[[ $maps -eq 2 && $width -eq 64 ]] || fail "--show 20 over 40 ops should draw 2 maps of 64 cells: $maps maps, width $width"
grep -m1 '^\[' <<< "$out" | grep -q '~' || fail "a buddy map should show internal waste as ~"
memsim --alloc slab --seed 3 --ops 40 --show 40 | grep -m1 '^\[' | grep -q '[FMH]' || fail "slab map lacks F/M/H"
pass "--show draws the floor map (64 cells; buddy waste '~'; slab kits F/M/H)"

for bad in "" "--alloc bogus" "--alloc first --ops 0" "--alloc buddy --size 1000"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    memsim $bad > /dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "memsim $bad: expected exit 2, got $rc"
done
printf 'alloc,1,FOOD,2\nexplode,3\n' > "$TMP/bad.csv"
rc=0; memsim --alloc first --trace "$TMP/bad.csv" > /dev/null 2>&1 || rc=$?
[[ $rc -eq 1 ]] || fail "a bad trace should exit 1, got $rc"
pass "usage errors exit 2 (buddy needs a power of two); a malformed trace exits 1"

echo "all mem tests passed"
