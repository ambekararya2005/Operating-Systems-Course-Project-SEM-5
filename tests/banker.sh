#!/usr/bin/env bash
# OS concept: Deadlock avoidance — the Banker's algorithm inside reliefd — Unit 3
#
#   1. build/test_banker: the textbook example (safe sequence <P1, P3, P4, P0, P2>, P1 (1,0,2)
#      granted, P4 (3,3,0) denied, ...)
#   2. reliefd + 8 camps on the flood scenario (3 trucks, 2 boats, 40 MED and 60 FOOD crates):
#      every Banker's request is granted exactly once and released exactly once, nothing is
#      rejected or abandoned; replaying grant/release from the log, Available never leaves
#      0..Total and matches every logged available=, no camp ever holds more than its claim;
#      every grant names a safe sequence; some requests had to wait because they were unsafe
#   3. reliefctl banker (text and JSON) while running: Need = Max - Allocation and
#      sum(Allocation) + Available = Total, plus a safe sequence
#   4. a camp holding all 3 trucks is SIGKILLed: its trucks are released (process termination),
#      and the camp that was waiting for a truck is granted one, woken by the release
#   5. --deadlock none|prevent|detect are not implemented yet (exit 2), bad --deadlock or
#      --resources fail cleanly, reliefctl banker without reliefd exits 1
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
at_least() { [[ $(count "$1") -ge $2 ]]; }
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

# 1. Textbook example.
"${WRAP[@]}" "$BIN/test_banker" || fail "test_banker"

# 2. Flood scenario.
run_reliefd --camps "$ROOT/scenarios/camps.conf" --scenario "$SCEN" --time-scale 5 --exit-when-done
wait_for "8 camps active in the Banker's table" at_least ' event=claim ' 8
wait_for "some grants" at_least ' event=grant ' 5
bank=$("${WRAP[@]}" "$BIN/reliefstat" --banker) || fail "reliefstat --banker"
json=$("${WRAP[@]}" "$BIN/reliefstat" --banker --json) || fail "reliefstat --banker --json"
wait_for "every camp done (BYE)" at_least ' event=bye ' 8
stop_reliefd

reqs=$(count ' event=request .* banker=1 ')
want=$(awk -F, 'NR > 1 && $4 ~ /^(TRUCK|BOAT|MED_KIT|FOOD_KIT)$/' "$SCEN" | wc -l)
[[ $reqs -eq $want && $reqs -gt 0 ]] || fail "expected $want Banker's requests, logged $reqs"
for ev in grant release; do
    dup=$(sed -n "s/.* event=$ev .* id=\\([0-9]*\\) .*/\\1/p" "$LOG" | sort | uniq -d | head -1)
    [[ -z "$dup" && $(count " event=$ev ") -eq $reqs ]] \
        || fail "every Banker's request should get exactly one $ev ($(count " event=$ev ") of $reqs, duplicate: ${dup:-none})"
done
[[ $(count ' event=reject_over_claim ') -eq 0 && $(count ' event=banker_abandoned ') -eq 0 ]] \
    || fail "no request should be rejected or abandoned"
grep -q " event=banker_stop mode=avoid grants=$reqs .* releases=$reqs rejects=0 .* abandoned=0" "$LOG" \
    || fail "banker_stop totals: $(grep -o 'event=banker_stop.*' "$LOG")"
pass "flood: $reqs Banker's requests, each granted once and released once; none rejected or abandoned"

awk 'function vec(s, v,   a, n) { n = split(s, a, ","); for (i = 1; i <= n; i++) v[i] = a[i] + 0 }
    function kv(k,   i, p) { for (i = 1; i <= NF; i++) if (index($i, k "=") == 1) return substr($i, length(k) + 2); return "" }
    / event=banker_start / { vec(kv("total"), tot); for (j = 1; j <= 4; j++) av[j] = tot[j]; next }
    / event=claim / { vec(kv("max"), m); for (j = 1; j <= 4; j++) claim[kv("camp"), j] = m[j]; next }
    / event=(grant|release) / {
        r = kv("resource"); j = (r == "TRUCK") ? 1 : (r == "BOAT") ? 2 : (r == "MED_KIT") ? 3 : 4
        u = kv("units") + 0; c = kv("camp")
        if ($0 ~ / event=grant /) { av[j] -= u; held[c, j] += u; grants++
                                    if (kv("safe_seq") !~ /^P[0-9]/) { print "no safe sequence: " $0; bad = 1 } }
        else { av[j] += u; held[c, j] -= u }
        if (av[j] < 0 || av[j] > tot[j]) { print "available out of range: " $0; bad = 1 }
        if (held[c, j] > claim[c, j] || held[c, j] < 0) { print "holding beyond the claim: " $0; bad = 1 }
        vec(kv("available"), logged)
        for (k = 1; k <= 4; k++) if (logged[k] != av[k]) { print "logged available differs from the replay: " $0; bad = 1; break }
    }
    END { for (j = 1; j <= 4; j++) if (av[j] != tot[j]) { print "not everything came back: resource " j; bad = 1 }
          exit bad }' "$LOG" > "$RELIEF_HOME/replay" || fail "Banker's replay: $(head -3 "$RELIEF_HOME/replay")"
unsafe=$(count ' event=deny_unsafe ')
[[ $unsafe -gt 0 ]] || fail "with 3 trucks for 8 camps some requests should have been unsafe"
pass "replay: Available stays in 0..Total and matches every grant/release line; no camp exceeds its claim; every grant has a safe sequence; $unsafe requests waited as unsafe"

# 3. reliefctl banker / reliefstat --banker views (taken while running).
if ! { grep -q '^ *Allocation *Max *Need *Available' <<< "$bank" && grep -q '^Safe sequence: <P[0-9]' <<< "$bank"; }; then
    fail "reliefstat --banker layout: $bank"
fi
python3 -I -c '
import json, sys
b = json.loads(sys.argv[1])
assert b["mode"] == "avoid" and b["total"] == [3, 2, 40, 60], b
for p in b["processes"]:
    assert [m - a for m, a in zip(p["max"], p["allocation"])] == p["need"], p
for j in range(4):
    assert sum(p["allocation"][j] for p in b["processes"]) + b["available"][j] == b["total"][j], (j, b)
assert b["safe"] and len(b["safe_sequence"]) == len(b["processes"]), b
' "$json" || fail "reliefstat --banker --json is inconsistent: $json"
pass "reliefstat --banker: textbook layout; Need = Max - Allocation, sum(Allocation) + Available = Total, safe"

# 4. A camp that holds every truck dies: its trucks come back and the waiting camp gets one.
cat > "$RELIEF_HOME/two.conf" <<'EOF'
1 Hadapsar
2 Kothrud
EOF
cat > "$RELIEF_HOME/hold.csv" <<'EOF'
arrival_min,camp_id,severity,resource,quantity,service_time
0,1,URGENT,TRUCK,3,1000
2,2,URGENT,TRUCK,1,1000
EOF
run_reliefd --camps "$RELIEF_HOME/two.conf" --scenario "$RELIEF_HOME/hold.csv" --time-scale 50
wait_for "camp 1 granted 3 trucks" at_least ' event=grant camp=1 .* resource=TRUCK units=3 ' 1
wait_for "camp 2 waiting for a truck" at_least ' event=wait_unavailable camp=2 ' 1
cpid=$(sed -n 's/.* event=camp_spawn camp=1 .* pid=\([0-9]*\) .*/\1/p' "$LOG" | head -1)
kill -KILL "$cpid"
wait_for "camp 1's trucks released" at_least ' event=release_all camp=1 reason=exit returned=3,0,0,0 ' 1
wait_for "camp 2 granted after the release" at_least ' event=grant camp=2 .* resource=TRUCK units=1 ' 1
stop_reliefd
pass "SIGKILLed camp: release_all returned its 3 trucks; the waiting camp was woken and granted"

# 5. Modes and bad input.
rc=0; "${WRAP[@]}" "$BIN/reliefd" --deadlock bogus --camps /dev/null >/dev/null 2>&1 || rc=$?
[[ $rc -eq 2 ]] || fail "--deadlock bogus should exit 2, got $rc"
printf 'TRUCK 3\nPLANES 4\n' > "$RELIEF_HOME/bad.conf"
rc=0; "${WRAP[@]}" "$BIN/reliefd" --resources "$RELIEF_HOME/bad.conf" --camps /dev/null >/dev/null 2>&1 || rc=$?
[[ $rc -eq 1 ]] || fail "a bad resources.conf should exit 1, got $rc"
[[ ! -e /dev/shm/reliefos_status && ! -e /dev/mqueue/reliefos_requests ]] || fail "failed starts left IPC objects"
rc=0; "$CTL" banker >/dev/null 2>&1 || rc=$?
[[ $rc -eq 1 ]] || fail "reliefctl banker without reliefd should exit 1, got $rc"
pass "bad --deadlock mode and bad resources.conf fail cleanly"

if ls /dev/mqueue/reliefos_camp_* >/dev/null 2>&1; then
    fail "camp reply queues left behind: $(ls /dev/mqueue/reliefos_camp_*)"
fi
if grep -q 'WARNING: ThreadSanitizer' "$OUT"; then
    grep -m1 -A20 'WARNING: ThreadSanitizer' "$OUT" >&2
    fail "ThreadSanitizer reported a race"
fi
pass "no reply queues left behind; no ThreadSanitizer reports"

echo "all banker tests passed"
