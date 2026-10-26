#!/usr/bin/env bash
# OS concept: Demand paging and page replacement — FIFO, LRU, Optimal, expiry-first — Unit 4
#
# 1. build/test_page: the classic reference string (FIFO 15, LRU 12, Optimal 9 with 3 frames),
#    Belady's anomaly (FIFO 9 faults with 3 frames, 10 with 4), expiry-first
# 2. build/pagesim: the same answers through --refs; --verbose prints the textbook table
#    (reference row, one row per frame, fault row Y/N); a generated trace is deterministic and
#    replays identically from --write-trace / --trace; usage errors exit 2
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
pagesim() { "${WRAP[@]}" "$BIN/pagesim" "$@"; }
faults() { pagesim "$@" | sed -n 's/.*references: \([0-9]*\) faults.*/\1/p'; }

"${WRAP[@]}" "$BIN/test_page" || fail "test_page"

CLASSIC=7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1
BELADY=1,2,3,4,1,2,5,1,2,3,4,5
got="$(faults --policy fifo --frames 3 --refs $CLASSIC) $(faults --policy lru --frames 3 --refs $CLASSIC) $(faults --policy optimal --frames 3 --refs $CLASSIC)"
[[ "$got" == "15 12 9" ]] || fail "classic string via pagesim: expected '15 12 9', got '$got'"
got="$(faults --policy fifo --frames 3 --refs $BELADY) $(faults --policy fifo --frames 4 --refs $BELADY)"
[[ "$got" == "9 10" ]] || fail "Belady via pagesim: expected '9 10', got '$got'"
pass "pagesim --refs: FIFO 15, LRU 12, Optimal 9; Belady FIFO 9 -> 10 faults with 3 -> 4 frames"

out=$(pagesim --policy fifo --frames 3 --refs $CLASSIC --verbose)
[[ $(grep -c '^frame [123] ' <<< "$out") -eq 3 ]] || fail "--verbose: one row per frame"
[[ $(grep '^fault' <<< "$out" | tr -cd 'Y' | wc -c) -eq 15 ]] || fail "--verbose: 15 Y in the fault row"
grep -q '^reference   7  0  1  2  0  3' <<< "$out" || fail "--verbose: the reference row"
pass "--verbose prints the textbook table (reference row, 3 frame rows, fault row with 15 Y)"

for p in fifo lru optimal expiry; do
    a=$(pagesim --policy "$p" --frames 4 --seed 9 --length 200 --write-trace "$TMP/t.csv")
    b=$(pagesim --policy "$p" --frames 4 --seed 9 --length 200)
    c=$(pagesim --policy "$p" --frames 4 --trace "$TMP/t.csv")
    [[ "$a" == "$b" && "$a" == "$c" ]] || fail "$p: generated trace not deterministic or not replayed identically"
done
pass "generated perishables traces are deterministic and replay identically from a written trace"

for bad in "" "--policy bogus --refs 1,2" "--policy lru --frames 0 --refs 1" "--policy lru --refs 1,x" \
           "--policy lru --refs 1 --seed 2"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    pagesim $bad > /dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "pagesim $bad: expected exit 2, got $rc"
done
pass "usage errors exit 2"

echo "all page tests passed"
