#!/usr/bin/env bash
# OS concept: Process synchronisation and deadlocks — Phase 3 gate M3 — Unit 3
#
# Gate M3 (zero deadlocks in E3 under avoidance), asserted end to end:
#   1. critical section (build/racelab): mutex, semaphore and peterson lose no update; none does
#      (lost updates; skipped under ThreadSanitizer, which would flag the deliberate race, and
#      under a wrapper that serialises threads, like valgrind)
#   2. loading dock invariant (build/test_sync): occupancy never above the capacity, every
#      package consumed exactly once
#   3. Banker's algorithm (build/test_banker): the textbook example, <P1, P3, P4, P0, P2>
#   4. E3 (build/dlsim, 5 seeds of the 300-request mix + deadlock.csv, run_e3.sh into a scratch
#      directory): avoid and prevent (both variants) reach 0 deadlocks and complete every
#      request; detect completes every request although deadlocks happen, i.e. it recovered from
#      every one; none deadlocks and freezes on deadlock.csv (expected)
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind
# Exit status: 0 if every check passed, 1 otherwise.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
FAILED=0

pass() { echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; FAILED=1; }
field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<< "$2"; }
run() { local b=$1; shift; "${WRAP[@]}" "$BIN/$b" "$@"; }

# 1. Critical section.
N=200000
[[ "${RELIEF_WRAP:-}" == *valgrind* ]] && N=2000
for m in mutex semaphore peterson; do
    out=$(run racelab --mode "$m" --iters "$N") || { fail "racelab $m failed"; continue; }
    if [[ $(field error "$out") == 0 ]]; then
        pass "racelab $m: 0 lost updates (final $(field final "$out"))"
    else
        fail "racelab $m: lost $(field error "$out") updates"
    fi
done
if [[ "$BIN" == */tsan || -n "${RELIEF_WRAP:-}" || $(nproc) -lt 2 ]]; then
    pass "racelab none: skipped (ThreadSanitizer, a wrapper, or one CPU)"
else
    lost=0
    for _ in 1 2 3 4 5; do
        out=$(run racelab --mode none --iters "$N") || break
        lost=$(field error "$out")
        [[ $lost -gt 0 ]] && break
    done
    if [[ $lost -gt 0 ]]; then pass "racelab none: race condition, lost $lost of $((2 * N)) updates"
    else fail "racelab none: no lost update in 5 runs"; fi
fi

# 2. Loading dock invariant.
quick=()
[[ "${RELIEF_WRAP:-}" == *valgrind* ]] && quick=(--quick)
if out=$(run test_sync "${quick[@]+"${quick[@]}"}") && grep -q '^ok   dock cap 8: .* each package consumed once' <<< "$out" \
   && grep -q '^ok   dock cap 1: ' <<< "$out"; then
    pass "dock: capacity never exceeded, every package consumed exactly once (test_sync)"
else
    fail "dock invariant (test_sync): $out"
fi

# 3. Banker's algorithm, textbook example.
if out=$(run test_banker) && grep -q 'textbook state is safe: <P1, P3, P4, P0, P2>' <<< "$out" \
   && grep -q 'P1 requests (1,0,2): granted' <<< "$out" && grep -q 'P4 requests (3,3,0): denied' <<< "$out"; then
    pass "Banker's textbook example: safe <P1, P3, P4, P0, P2>, P1 (1,0,2) granted, P4 (3,3,0) denied"
else
    fail "test_banker: $out"
fi

# 4. E3 (deterministic simulator).
# Under make tsan the wrapper is `setarch ... -R` (no ASLR): run the script through it so dlsim
# inherits it. A valgrind wrapper is not applied to a shell script (dlsim is pure computation).
pre=()
[[ "${RELIEF_WRAP:-}" == setarch* ]] && pre=("${WRAP[@]}")
if ! BIN_DIR="${BIN#"$ROOT"/}" RESULTS="$TMP" "${pre[@]+"${pre[@]}"}" "$ROOT/experiments/run_e3.sh" > "$TMP/e3.out" 2>&1; then
    fail "run_e3.sh failed: $(tail -3 "$TMP/e3.out")"
else
    if awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
        { l = $c["label"]; s = $c["scenario"]; dl = $c["deadlocks"]; ok = $c["completed"] == $c["requests"]
          if (l == "avoid" || l == "prevent" || l == "prevent-all") {
              if (dl != 0 || !ok) { printf "%s on %s: deadlocks %s, completed %s/%s\n", l, s, dl, $c["completed"], $c["requests"]; bad = 1 }
              zero[l]++ }
          if (l == "detect") {
              if (!ok || $c["frozen"] != 0) { printf "detect on %s: completed %s/%s frozen %s\n", s, $c["completed"], $c["requests"], $c["frozen"]; bad = 1 }
              if (dl > 0 && $c["preemptions"] == 0) { printf "detect on %s: deadlocks without recovery\n", s; bad = 1 }
              det += dl; rec += $c["preemptions"] }
          if (l == "none" && s == "deadlock.csv" && ($c["frozen"] != 1 || dl < 1)) { print "none should deadlock on deadlock.csv"; bad = 1 }
          if (l == "none" && s == "deadlock.csv") nonedl = 1 }
        END { if (!nonedl) { print "no none run on deadlock.csv"; bad = 1 }
              printf "%d %d %d %d %d\n", zero["avoid"], zero["prevent"], zero["prevent-all"], det, rec
              exit bad }' "$TMP/e3.csv" > "$TMP/check" 2>&1; then
        read -r za zp zpa det rec < <(tail -1 "$TMP/check")
        pass "E3: avoid ($za runs), prevent ($zp) and prevent-all ($zpa) reach 0 deadlocks and complete every request"
        pass "E3: detect completed every run: recovered from all $det deadlocks ($rec preemptions)"
        pass "E3: none deadlocks and freezes on deadlock.csv (expected)"
    else
        fail "E3: $(head -5 "$TMP/check")"
    fi
fi

if [[ $FAILED -ne 0 ]]; then
    echo "PHASE 3 GATE M3: FAILED" >&2
    exit 1
fi
echo "PHASE 3 GATE M3: PASSED (zero deadlocks in E3 under avoidance and prevention)"
