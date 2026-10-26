#!/usr/bin/env bash
# OS concept: Disk scheduling (FCFS, SSTF, SCAN, C-SCAN, LIFO) as delivery routing — Unit 5
#
# 1. build/test_disk: the textbook queue 98,183,37,122,14,124,65,67 from 53 (FCFS 640, SSTF 236,
#    SCAN up 331, C-SCAN up 382 with the return jump, LIFO 609)
# 2. build/routesim: the same totals through --stops; the diagram has one row per leg; seeded
#    runs are deterministic; usage errors exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
routesim() { "${WRAP[@]}" "$BIN/routesim" "$@"; }
km() { routesim "$@" --quiet | sed -n 's/^total \([0-9]*\) km.*/\1/p'; }

"${WRAP[@]}" "$BIN/test_disk" || fail "test_disk"

Q=98,183,37,122,14,124,65,67
got="$(km --policy fcfs --stops $Q) $(km --policy sstf --stops $Q) $(km --policy scan --stops $Q) $(km --policy cscan --stops $Q) $(km --policy lifo --stops $Q)"
[[ "$got" == "640 236 331 382 609" ]] || fail "routesim totals: expected '640 236 331 382 609', got '$got'"
[[ $(km --policy scan --dir down --stops $Q) == 236 ]] || fail "SCAN down should be 236"
pass "routesim --stops: FCFS 640, SSTF 236, SCAN 331, C-SCAN 382, LIFO 609 (SCAN down 236)"

out=$(routesim --policy cscan --stops $Q)
legs=$(grep -cE '\+[0-9]+ = [0-9]+ km$' <<< "$out")
[[ $legs -eq 10 ]] || fail "C-SCAN diagram: expected 10 legs (8 stops, the end, the jump), got $legs"
grep -q '^jump 0 *~' <<< "$out" || fail "diagram should show the C-SCAN jump"
grep -q '^end 199 .*|' <<< "$out" || fail "diagram should show the end turn"
grep -q 'of which 199 km the empty return jump; 183 without it' <<< "$out" || fail "C-SCAN jump accounting"
pass "diagram: one row per leg, end turn '|' and C-SCAN jump '~'; jump km reported separately"

a=$(routesim --policy sstf --seed 4 --quiet); b=$(routesim --policy sstf --seed 4 --quiet)
[[ "$a" == "$b" ]] || fail "seeded runs differ"
[[ $(grep -c '' <<< "$a") -ge 3 ]] || fail "seeded output"
pass "seeded runs (20 random stops) are deterministic"

for bad in "" "--policy bogus --stops 1" "--policy fcfs" "--policy fcfs --stops 1,x" "--policy fcfs --stops 300" \
           "--policy fcfs --stops 1 --seed 2" "--policy scan --dir left --stops 1"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    routesim $bad > /dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "routesim $bad: expected exit 2, got $rc"
done
pass "usage errors exit 2"

echo "all route tests passed"
