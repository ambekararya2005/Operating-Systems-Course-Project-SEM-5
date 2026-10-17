#!/usr/bin/env bash
# OS concept: Producer-consumer (bounded buffer, three semaphores) inside reliefd — Unit 3
#
# Tests reliefd's loading dock (src/reliefd/dock.c) on the flood scenario with a small dock
# (capacity 2, 2 producers, 2 trucks) so producers do block:
#   1. every kit request becomes one order, and is produced, consumed and delivered exactly
#      once; nothing else goes to the dock
#   2. the occupancy logged at every put/take is within 0..capacity and equals the replayed
#      put/take history (the log line is written inside the dock's critical section)
#   3. dock_stop totals agree; producers were blocked on the full dock; reliefstat shows the
#      dock and the inventory while running; reliefctl report has the dock section
#   4. --rw semaphore runs too, and stopping with orders pending abandons them cleanly
#   5. bad --dock/--producers/--trucks/--rw exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for reliefd (and its camps), e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
CTL="$ROOT/scripts/reliefctl"
SCEN="$ROOT/scenarios/flood_pune.csv"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN"
mkdir -p "$RELIEF_HOME/logs"
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
DPID=""

cleanup() {
    if [[ -n "$DPID" ]] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    rm -rf "$RELIEF_HOME"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "--- reliefd stderr (tail) ---" >&2
    tail -n 30 "$OUT" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }
count() { grep -c -- "$1" "$LOG" 2>/dev/null || true; }
wait_for() {    # wait_for <description> <command...>
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
delivered_all() { [[ $(count ' event=bye ') -ge 8 && $(count ' event=deliver ') -ge $(count ' event=request .* dock=1') ]]; }
ids() { sed -n "s/.* event=$1 .* id=\\([0-9]*\\) .*/\\1/p" "$LOG" | sort -n; }

run_reliefd() {
    : > "$LOG"
    "${WRAP[@]}" "$BIN/reliefd" "$@" >>"$OUT" 2>&1 &
    DPID=$!
}
stop_reliefd() {
    local rc=0
    kill -TERM "$DPID"
    wait "$DPID" || rc=$?
    DPID=""
    [[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"
}

for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done

CAP=2
run_reliefd --camps "$ROOT/scenarios/camps.conf" --scenario "$SCEN" --time-scale 5 --exit-when-done \
    --dock "$CAP" --producers 2 --trucks 2
wait_for "camps done and every kit delivered" delivered_all
stat=$("${WRAP[@]}" "$BIN/reliefstat") || fail "reliefstat while running"
stop_reliefd

# 1. Exactly once.
kits=$(awk -F, 'NR > 1 && $4 ~ /_KIT$/' "$SCEN" | wc -l)
[[ $(count ' event=request .* dock=1') -eq $kits && $kits -gt 0 ]] || fail "expected $kits kit requests sent to the dock"
[[ $(count ' event=request .* resource=\(RESCUE\|TRUCK\|BOAT\) .* dock=1') -eq 0 ]] || fail "a non-kit request went to the dock"
sed -n 's/.* event=request id=\([0-9]*\) .* dock=1$/\1/p' "$LOG" | sort -n > "$RELIEF_HOME/want"
for ev in produce consume deliver; do
    ids "$ev" > "$RELIEF_HOME/$ev"
    cmp -s "$RELIEF_HOME/want" "$RELIEF_HOME/$ev" \
        || fail "$ev ids differ from the kit requests (missing or duplicated): $(diff "$RELIEF_HOME/want" "$RELIEF_HOME/$ev" | head -5)"
done
pass "$kits kit requests: each produced, consumed and delivered exactly once"

# 2. Occupancy within bounds and consistent with the put/take history.
awk -v cap="$CAP" '/ event=(produce|consume) / {
        occ = -1
        for (i = 1; i <= NF; i++) if ($i ~ /^occupancy=/) { split($i, kv, "="); occ = kv[2] + 0 }
        n += ($0 ~ / event=produce /) ? 1 : -1
        if (occ < 0 || occ > cap) { print "out of range: " $0; bad = 1 }
        if (occ != n) { print "history says " n ": " $0; bad = 1 }
    } END { exit bad }' "$LOG" > "$RELIEF_HOME/occ" || fail "occupancy: $(head -3 "$RELIEF_HOME/occ")"
pass "occupancy at every put/take is within 0..$CAP and matches the replayed history"

# 3. Totals, blocking, status and report.
stop_line=$(grep ' event=dock_stop ' "$LOG") || fail "no dock_stop line"
for kv in "orders=$kits" "produced=$kits" "consumed=$kits" "delivered=$kits" "undelivered=0" "abandoned_orders=0" "max_occupancy=$CAP"; do
    [[ " $stop_line " == *" $kv "* ]] || fail "dock_stop should have $kv: $stop_line"
done
blocked=$(sed -n 's/.* blocked_puts=\([0-9]*\) .*/\1/p' <<< "$stop_line")
[[ $blocked -gt 0 ]] || fail "with a $CAP-slot dock some puts should block: $stop_line"
if ! { grep -q "^loading dock: [0-9]*/$CAP packages" <<< "$stat" && grep -q '^inventory (lock: rwlock)' <<< "$stat"; }; then
    fail "reliefstat should show the dock and the inventory: $stat"
fi
rep=$("$CTL" report --last-run) || fail "reliefctl report"
if ! { grep -q "^  produced $kits, consumed $kits, delivered $kits" <<< "$rep" &&
        grep -q "^  producers blocked on a full dock: $blocked of $kits puts" <<< "$rep"; }; then
    fail "report dock section: $(grep -A5 '^Loading dock' <<< "$rep")"
fi
pass "dock_stop totals agree, $blocked puts blocked on the full dock; reliefstat and report show the dock"

# 4. Readers-preference inventory lock; stop with orders still pending.
cat > "$RELIEF_HOME/slow.csv" <<'EOF'
arrival_min,camp_id,severity,resource,quantity,service_time
0,1,URGENT,FOOD_KIT,10,1
0,1,URGENT,MED_KIT,10,1
0,1,URGENT,HYGIENE_KIT,10,1
0,1,URGENT,FOOD_KIT,10,1
0,1,URGENT,MED_KIT,10,1
0,1,URGENT,HYGIENE_KIT,10,1
EOF
echo "1 Hadapsar" > "$RELIEF_HOME/one.conf"
run_reliefd --camps "$RELIEF_HOME/one.conf" --scenario "$RELIEF_HOME/slow.csv" --time-scale 1000 \
    --dock 1 --producers 1 --trucks 1 --rw semaphore
six_orders() { [[ $(count ' event=request .* dock=1') -ge 6 ]]; }
wait_for "6 kit requests received" six_orders
stop_reliefd
grep -q ' event=dock_start capacity=1 producers=1 trucks=1 rw=semaphore' "$LOG" || fail "dock_start with rw=semaphore"
stop_line=$(grep ' event=dock_stop ' "$LOG") || fail "no dock_stop line"
abandoned=$(sed -n 's/.* abandoned_orders=\([0-9]*\) .*/\1/p' <<< "$stop_line")
[[ $abandoned -ge 1 && " $stop_line " == *" orders=6 "* ]] || fail "stop with pending orders: $stop_line"
pass "--rw semaphore; stopping with work pending abandons $abandoned orders and exits 0"

# 5. Bad options.
for bad in "--dock 0" "--dock 65" "--producers 9" "--trucks 0" "--rw bogus"; do
    rc=0
    # shellcheck disable=SC2086  # word-splitting the option string is intended
    "${WRAP[@]}" "$BIN/reliefd" $bad --camps /dev/null > /dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "reliefd $bad should exit 2, got $rc"
done
pass "bad --dock/--producers/--trucks/--rw exit 2"

if grep -q 'WARNING: ThreadSanitizer' "$OUT"; then
    grep -m1 -A20 'WARNING: ThreadSanitizer' "$OUT" >&2
    fail "ThreadSanitizer reported a race"
fi
pass "no ThreadSanitizer reports in reliefd's stderr"

echo "all dock tests passed"
