#!/usr/bin/env bash
# OS concept: Producer-consumer and readers-writers — Unit 3
#
# 1. build/test_sync: the dock's bounded buffer (capacity never exceeded, every package
#    consumed exactly once, a full dock blocks the producer) and both rwsync kinds
# 2. build/rwlab, both kinds: no torn read; without a wrapper that serialises threads, the
#    readers-preference semaphore starves the writer (far longer max wait, fewer writes during
#    the run) while the writer-preference rwlock does not; usage errors exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind (test_sync then runs --quick)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<< "$2"; }
VALGRIND=0
[[ "${RELIEF_WRAP:-}" == *valgrind* ]] && VALGRIND=1

# 1. Unit tests.
quick=()
(( VALGRIND )) && quick=(--quick)
"${WRAP[@]}" "$BIN/test_sync" "${quick[@]+"${quick[@]}"}" || fail "test_sync"

# 2. rwlab.
secs=0.5
(( VALGRIND )) && secs=0.2
sem=$("${WRAP[@]}" "$BIN/rwlab" --rw semaphore --seconds "$secs") || fail "rwlab semaphore: $sem"
rwl=$("${WRAP[@]}" "$BIN/rwlab" --rw rwlock --seconds "$secs") || fail "rwlab rwlock: $rwl"
for out in "$sem" "$rwl"; do
    [[ $(field torn_reads "$out") == 0 && $(field writes "$out") -ge 1 ]] || fail "rwlab: $out"
done
pass "rwlab: both kinds, no torn read, the writer got in at least once"
if (( VALGRIND )); then
    pass "rwlab starvation comparison skipped under valgrind (it serialises threads)"
else
    smax=$(field writer_wait_max_ms "$sem"); rmax=$(field writer_wait_max_ms "$rwl")
    sw=$(field writes_during_run "$sem"); rw=$(field writes_during_run "$rwl")
    awk -v s="$smax" -v r="$rmax" -v sw="$sw" -v rw="$rw" 'BEGIN { exit !(s > 10 * r && sw < rw) }' \
        || fail "expected writer starvation under semaphore only: semaphore max wait $smax ms, $sw writes; rwlock $rmax ms, $rw writes"
    pass "writer starvation: semaphore (readers preference) max writer wait $smax ms, $sw writes during the run; rwlock (writer preference) $rmax ms, $rw writes"
fi
for bad in "" "--rw bogus" "--rw rwlock --readers 0" "--rw rwlock --seconds x"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    "${WRAP[@]}" "$BIN/rwlab" $bad > /dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "rwlab $bad: expected exit 2, got $rc"
done
pass "rwlab usage errors exit 2"

echo "all sync tests passed"
