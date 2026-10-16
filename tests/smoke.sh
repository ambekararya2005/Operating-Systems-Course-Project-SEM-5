#!/usr/bin/env bash
# Smoke test: reliefd <-> manually started camps over the POSIX message queue.
# reliefd runs with an empty camps config here; tests/camps.sh covers supervised camps.
#   BIN_DIR      directory holding reliefd and camp (default: build)
#   RELIEF_WRAP  optional command prefix for every binary, e.g. "valgrind --error-exitcode=99"
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME
mkdir -p "$RELIEF_HOME/logs"
LOG="$RELIEF_HOME/logs/reliefd.log"     # structured key=value
OUT="$RELIEF_HOME/logs/reliefd.out"     # raw stderr
PIDFILE="$RELIEF_HOME/reliefd.pid"
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
    echo "--- reliefd stderr ---" >&2
    cat "$OUT" >&2 2>/dev/null || true
    exit 1
}
pass() { echo "ok   $*"; }

count() { grep -c -- "$1" "$2" || true; }

start_reliefd() {
    "${WRAP[@]}" "$BIN/reliefd" --camps /dev/null >>"$OUT" 2>&1 &
    DPID=$!
    for _ in $(seq 100); do
        [[ -s "$PIDFILE" ]] && return 0
        kill -0 "$DPID" 2>/dev/null || fail "reliefd exited during startup"
        sleep 0.1
    done
    fail "reliefd did not write its pid file"
}

wait_reliefd() {
    local rc=0
    wait "$DPID" || rc=$?
    DPID=""
    [[ $rc -eq 0 ]] || fail "reliefd exited with status $rc"
}

run_camp() {   # run_camp <logfile> <camp args...>
    local out=$1; shift
    "${WRAP[@]}" "$BIN/camp" "$@" 2>"$out" || fail "camp $* failed (see $out)"
}

# The queue name is global; don't disturb a reliefd the developer is running.
if [[ -e /dev/mqueue/reliefos_requests ]]; then
    echo "SKIP: /dev/mqueue/reliefos_requests exists (is reliefd running?). Stop it first." >&2
    exit 1
fi

# 1. Camps send HELLO, N requests, BYE; SHUTDOWN message stops reliefd.
start_reliefd
run_camp "$RELIEF_HOME/camp1.log" --id 1 --count 5 --seed 42
run_camp "$RELIEF_HOME/camp2.log" --id 2 --count 3 --seed 42
run_camp "$RELIEF_HOME/camp7a.log" --id 7 --count 4 --seed 99
run_camp "$RELIEF_HOME/camp7b.log" --id 7 --count 4 --seed 99
run_camp "$RELIEF_HOME/shutdown.log" --shutdown
wait_reliefd
pass "reliefd stopped on SHUTDOWN message"

[[ $(count ' event=hello ' "$LOG") -eq 4 ]] || fail "expected 4 HELLO"
[[ $(count ' event=request ' "$LOG") -eq 16 ]] || fail "expected 16 REQUEST"
[[ $(count ' event=bye ' "$LOG") -eq 4 ]] || fail "expected 4 BYE"
grep -q 'event=daemon_stop hello=4 request=16 bye=4' "$LOG" || fail "wrong totals"
pass "reliefd received 4 HELLO, 16 REQUEST, 4 BYE"

[[ ! -e "$PIDFILE" ]] || fail "pid file left behind after SHUTDOWN"
[[ ! -e /dev/mqueue/reliefos_requests ]] || fail "queue left behind after SHUTDOWN"
pass "pid file and queue removed"

# 2. Same --id and --seed => identical request stream (strip timestamp and pid).
strip() { grep 'sent REQUEST' "$1" | sed 's/^.*\] //'; }
[[ -n "$(strip "$RELIEF_HOME/camp7a.log")" ]] || fail "camp 7 logged no requests"
diff <(strip "$RELIEF_HOME/camp7a.log") <(strip "$RELIEF_HOME/camp7b.log") >/dev/null \
    || fail "same seed produced different requests"
pass "camp output deterministic for --seed"

# 3. SIGTERM stops reliefd cleanly.
start_reliefd
kill -TERM "$DPID"
wait_reliefd
grep -q 'event=shutdown reason=signal sig=15' "$LOG" || fail "SIGTERM not handled"
[[ ! -e "$PIDFILE" ]] || fail "pid file left behind after SIGTERM"
pass "reliefd stopped cleanly on SIGTERM"

# 4. reliefctl start / status / stop with the real camps config (reliefctl applies RELIEF_WRAP itself).
CTL="$ROOT/scripts/reliefctl"
export RELIEF_BIN_DIR="$BIN"
"$CTL" start >/dev/null || fail "reliefctl start"
"$CTL" status | grep -q '^running' || fail "reliefctl status should report running"
"$CTL" stop >/dev/null || fail "reliefctl stop"
"$CTL" status | grep -q '^stopped' || fail "reliefctl status should report stopped"
pass "reliefctl start/status/stop"

echo "all smoke tests passed"
