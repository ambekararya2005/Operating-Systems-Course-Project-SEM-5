#!/usr/bin/env bash
# OS concept: IPC via POSIX message queues — paced producers, bounded queue, back-pressure — Unit 2
#
# Tests realistic camp traffic:
#   1. experiments/gen_scenario.py reproduces scenarios/flood_pune.csv (15/35/50 mix, 8 camps)
#   2. reliefd + 8 camps replay the 200-row scenario on a simulated clock: exactly 200 requests
#      (the CSV's ids, each camp in arrival order except Banker's requests held back within its claim, spread over time), 8 HELLO, 8 BYE
#   3. queue full: with reliefd frozen (SIGSTOP), camps log "queue full, retrying" instead of
#      hanging, and every message is still delivered once reliefd continues
#   BIN_DIR       directory holding reliefd and camp (default: build)
#   RELIEF_WRAP   optional command prefix for every binary, e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TIMEOUT=${TEST_TIMEOUT:-10}
SCENARIO="$ROOT/scenarios/flood_pune.csv"
SCALE=5     # ms per simulated minute: 360 minutes in ~1.8 s

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME
mkdir -p "$RELIEF_HOME/logs"
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
PIDFILE="$RELIEF_HOME/reliefd.pid"
DPID=""
CAMP_PIDS=()

cleanup() {
    kill -CONT "$DPID" 2>/dev/null || true
    for p in "${CAMP_PIDS[@]}"; do kill -TERM "$p" 2>/dev/null || true; done
    if [[ -n "$DPID" ]] && kill -0 "$DPID" 2>/dev/null; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    if [[ -n "${KEEP_HOME:-}" ]]; then
        echo "kept $RELIEF_HOME" >&2
    else
        rm -rf "$RELIEF_HOME"
    fi
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
requests_at_least() { [[ $(count ' event=request ') -ge $1 ]]; }
pidfile_written() { [[ -s "$PIDFILE" ]]; }
camps_saw_full_queue() { cat "$RELIEF_HOME"/camp*.log 2>/dev/null | grep -q 'queue full, retrying'; }
stop_reliefd() {
    local rc=0
    wait "$DPID" || rc=$?
    DPID=""
    [[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"
}
check_totals() {    # check_totals <label>
    local h r b
    h=$(count ' event=hello '); r=$(count ' event=request '); b=$(count ' event=bye ')
    [[ $h -eq 8 && $r -eq $ROWS && $b -eq 8 ]] \
        || fail "$1: expected hello=8 request=$ROWS bye=8, got hello=$h request=$r bye=$b"
    grep -q "event=daemon_stop hello=8 request=$ROWS bye=8" "$LOG" || fail "$1: wrong daemon_stop totals"
}

if [[ -e /dev/mqueue/reliefos_requests ]]; then
    echo "SKIP: /dev/mqueue/reliefos_requests exists (is reliefd running?). Stop it first." >&2
    exit 1
fi

# 1. The scenario is reproducible from its generator.
python3 -I "$ROOT/experiments/gen_scenario.py" | cmp -s - "$SCENARIO" \
    || fail "gen_scenario.py output differs from $SCENARIO (regenerate it or fix the generator)"
ROWS=$(awk 'NR > 1' "$SCENARIO" | wc -l)
mix=$(awk -F, 'NR > 1 { s[$3]++; c[$2] = 1 } END { printf "%d/%d/%d camps=%d", s["CRITICAL"], s["URGENT"], s["ROUTINE"], length(c) }' "$SCENARIO")
[[ $ROWS -eq 200 && "$mix" == "30/70/100 camps=8" ]] || fail "unexpected scenario shape: rows=$ROWS mix=$mix"
pass "gen_scenario.py reproduces flood_pune.csv: 200 rows, CRITICAL/URGENT/ROUTINE = $mix"

# 2. Paced replay through reliefd's 8 camps.
"${WRAP[@]}" "$BIN/reliefd" --camps "$ROOT/scenarios/camps.conf" --scenario "$SCENARIO" \
    --time-scale "$SCALE" >>"$OUT" 2>&1 &
DPID=$!
wait_for "all $ROWS requests" requests_at_least "$ROWS"
sleep 0.3   # any duplicate would show up now
kill -TERM "$DPID"
stop_reliefd
check_totals "paced run"
pass "reliefd logged exactly $ROWS event=request lines, 8 HELLO, 8 BYE"

diff <(awk -F, 'NR > 1 { print $2 * 1000000 + NR }' "$SCENARIO" | sort -n) \
     <(sed -n 's/.* event=request id=\([0-9]*\) .*/\1/p' "$LOG" | sort -n) >/dev/null \
    || fail "request ids differ from the scenario rows"
pass "request ids match the scenario rows exactly (no loss, no duplicates)"

# Banker's requests (banker=1) may be held back by their camp until its own releases keep it
# within its declared claim (Phase 3), so only the other requests must keep arrival order.
awk '/ event=request .* banker=0 / {
        for (i = 1; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
        if (f["arrival"] + 0 < last[f["camp"]] + 0) { bad = 1 }
        last[f["camp"]] = f["arrival"]
     } END { exit bad }' "$LOG" || fail "a camp sent requests out of arrival order"
pass "every camp sent its non-Banker requests in arrival order"

# Real time between the first and last request must reflect the simulated clock.
span_ms=$(awk '/ event=request / {
        split(substr($1, 4), dt, "T"); split(dt[2], t, ":")
        ms = (t[1] * 3600 + t[2] * 60 + t[3]) * 1000
        if (first == "") first = ms
        last = ms
     } END { printf "%d", last - first }' "$LOG")
read -r amin amax < <(awk -F, 'NR > 1 { if (min == "" || $1 < min) min = $1; if ($1 > max) max = $1 }
                                END { print min, max }' "$SCENARIO")
expect_ms=$(( (amax - amin) * SCALE ))
[[ $span_ms -ge $(( expect_ms * 8 / 10 )) ]] \
    || fail "requests not paced: spread over ${span_ms} ms, expected about ${expect_ms} ms"
pass "requests paced by the simulated clock: ${span_ms} ms real for $((amax - amin)) simulated minutes at ${SCALE} ms/min"

# 3. Queue full: freeze reliefd so the 10-slot queue fills up.
: > "$LOG"
"${WRAP[@]}" "$BIN/reliefd" --camps /dev/null >>"$OUT" 2>&1 &
DPID=$!
wait_for "reliefd pid file" pidfile_written
kill -STOP "$DPID"
while read -r id _; do
    [[ "$id" =~ ^[0-9]+$ ]] || continue
    "${WRAP[@]}" "$BIN/camp" --id "$id" --scenario "$SCENARIO" --time-scale 0 \
        2>"$RELIEF_HOME/camp$id.log" &
    CAMP_PIDS+=("$!")
done < "$ROOT/scenarios/camps.conf"
wait_for "a camp to report a full queue" camps_saw_full_queue
for p in "${CAMP_PIDS[@]}"; do
    kill -0 "$p" 2>/dev/null || fail "camp pid $p exited while the queue was full (should retry)"
done
kill -CONT "$DPID"
wait_for "all $ROWS requests after SIGCONT" requests_at_least "$ROWS"
for p in "${CAMP_PIDS[@]}"; do
    kill -TERM "$p"
    rc=0; wait "$p" || rc=$?
    [[ $rc -eq 0 ]] || fail "camp pid $p exited with status $rc"
done
CAMP_PIDS=()
"${WRAP[@]}" "$BIN/camp" --shutdown 2>/dev/null
stop_reliefd
check_totals "queue-full run"
warnings=$(cat "$RELIEF_HOME"/camp*.log | grep -c 'queue full, retrying')
pass "queue full: camps retried ($warnings warnings) without hanging, all $ROWS requests delivered"

[[ ! -e "$PIDFILE" && ! -e /dev/mqueue/reliefos_requests ]] || fail "pid file or queue left behind"
echo "all traffic tests passed"
