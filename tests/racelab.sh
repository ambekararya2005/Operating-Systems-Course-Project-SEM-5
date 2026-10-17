#!/usr/bin/env bash
# OS concept: The critical-section problem — Peterson, mutex, spinlock, semaphore — Unit 3
#
# Tests build/racelab:
#   1. peterson, mutex, spinlock, semaphore: 2 threads, final count exactly 2N (no lost update)
#   2. one thread (no contention): every mode, including none, gives exactly N
#   3. none: loses updates (tried up to 5 times; skipped on a 1-CPU machine), and
#      peterson-nofence runs and never counts more than 2N. Both race on purpose, so they are
#      skipped under ThreadSanitizer (BIN_DIR ending in tsan)
#   4. --csv writes a header once and appends one row per run; usage errors exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind (fewer iterations: it serialises threads)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
racelab() { "${WRAP[@]}" "$BIN/racelab" "$@"; }
field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<< "$2"; }

N=200000
[[ "${RELIEF_WRAP:-}" == *valgrind* ]] && N=2000
TSAN=0
[[ "$BIN" == */tsan ]] && TSAN=1

# 1. Correct modes.
for m in peterson mutex spinlock semaphore; do
    out=$(racelab --mode "$m" --iters "$N") || fail "$m: racelab failed"
    [[ $(field final "$out") -eq $((2 * N)) && $(field error "$out") -eq 0 ]] \
        || fail "$m: expected final $((2 * N)), error 0: $out"
    pass "$m: 2 threads x $N increments, final count $((2 * N)), no lost update"
done

# 2. One thread: nothing to race with.
for m in none peterson peterson-nofence mutex spinlock semaphore; do
    [[ $TSAN -eq 1 && ( $m == none || $m == peterson-nofence ) ]] && continue
    out=$(racelab --mode "$m" --iters "$N" --threads 1) || fail "$m --threads 1 failed"
    [[ $(field final "$out") -eq $N ]] || fail "$m --threads 1: expected $N: $out"
done
pass "one thread: every mode counts exactly N"

# 3. The deliberate races.
if [[ $TSAN -eq 1 ]]; then
    pass "none / peterson-nofence skipped under ThreadSanitizer (deliberate data races)"
else
    out=$(racelab --mode peterson-nofence --iters "$N") || fail "peterson-nofence failed"
    [[ $(field final "$out") -le $((2 * N)) ]] || fail "peterson-nofence counted more than 2N: $out"
    pass "peterson-nofence: runs, lost $(field error "$out") of $((2 * N)) this time"
    if [[ $(nproc) -lt 2 || -n "${RELIEF_WRAP:-}" ]]; then
        pass "none: lost-update check skipped (one CPU, or threads serialised by a wrapper)"
    else
        lost=0
        for _ in 1 2 3 4 5; do
            out=$(racelab --mode none --iters "$N") || fail "none failed"
            lost=$(field error "$out")
            [[ $lost -gt 0 ]] && break
        done
        [[ $lost -gt 0 ]] || fail "none: no lost update in 5 runs on $(nproc) CPUs"
        pass "none: race condition, lost $lost of $((2 * N)) increments"
    fi
fi

# 4. CSV and usage.
racelab --mode mutex --iters 1000 --csv "$TMP/r.csv" --run 1 > /dev/null
racelab --mode spinlock --iters 1000 --csv "$TMP/r.csv" --run 2 > /dev/null
[[ $(wc -l < "$TMP/r.csv") -eq 3 && $(head -1 "$TMP/r.csv") == mode,run,* ]] || fail "CSV: $(cat "$TMP/r.csv")"
[[ $(sed -n 3p "$TMP/r.csv") == spinlock,2,2,1000,2000,2000,0,* ]] || fail "CSV row: $(sed -n 3p "$TMP/r.csv")"
for bad in "--mode bogus" "--mode mutex --iters 0" "--mode mutex --threads 3" ""; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    racelab $bad > /dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "racelab $bad: expected exit 2, got $rc"
done
pass "--csv header once + one row per run; usage errors exit 2"

echo "all racelab tests passed"
