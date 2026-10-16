#!/usr/bin/env bash
# OS concept: Phase 1 gate M1 "camps reach reliefd" — processes, IPC and clean teardown — Unit 1/2
#
# End-to-end check of the finished Phase 1 system through its public CLI:
#   reliefctl simulate flood --camps N --seed 42
# then asserts N HELLO, N BYE and 200 requests in the run's log, no zombie processes
# (sampled while the run is live and checked afterwards), no leftover message queue,
# no leftover shared-memory segment, and a non-empty report.
#   BIN_DIR        directory holding the binaries (default: build)
#   RELIEF_WRAP    optional command prefix for reliefd (applied by reliefctl), e.g. valgrind
#   PHASE1_CAMPS   number of camps (default 8; `make valgrind` uses 2)
#   TEST_TIMEOUT   extra seconds allowed for the run (default 10)
# Exit status: 0 if every check passed, 1 otherwise.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
CAMPS=${PHASE1_CAMPS:-8}
SEED=42
ROWS=200
CTL="$ROOT/scripts/reliefctl"
MQ=/dev/mqueue/reliefos_requests
SHM=/dev/shm/reliefos_status

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN"
LOG="$RELIEF_HOME/logs/reliefd.log"
OUT="$RELIEF_HOME/logs/reliefd.out"
REPORT="$RELIEF_HOME/simulate.out"
SIM_PID=""
FAILED=0

cleanup() {
    if [[ -n "$SIM_PID" ]] && kill -0 "$SIM_PID" 2>/dev/null; then
        kill -TERM "$SIM_PID" 2>/dev/null || true
    fi
    "$CTL" stop >/dev/null 2>&1 || true
    if [[ -n "${KEEP_HOME:-}" || $FAILED -ne 0 ]]; then
        echo "logs kept in $RELIEF_HOME" >&2
    else
        rm -rf "$RELIEF_HOME"
    fi
}
trap cleanup EXIT

pass() { echo "ok   $*"; }
fail() {    # record and keep going, so one run reports every failed check
    echo "FAIL $*" >&2
    FAILED=1
}
count() { grep -c -- "$1" "$2" 2>/dev/null || true; }
check() {   # check <pass message> <fail message> <test command...>
    local ok=$1 bad=$2
    shift 2
    if "$@"; then pass "$ok"; else fail "$bad"; fi
}

for f in "$MQ" "$SHM"; do
    if [[ -e "$f" ]]; then
        echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2
        exit 1
    fi
done

# Run the simulation in the background so zombies can be sampled while it is live.
"$CTL" simulate flood --camps "$CAMPS" --seed "$SEED" --rows "$ROWS" > "$REPORT" 2>&1 &
SIM_PID=$!
zombies_seen=0
dpid=""
while kill -0 "$SIM_PID" 2>/dev/null; do
    if [[ -z "$dpid" && -s "$RELIEF_HOME/reliefd.pid" ]]; then
        dpid=$(cat "$RELIEF_HOME/reliefd.pid")
    fi
    if [[ -n "$dpid" ]]; then
        # A camp that exited stays a zombie only until reliefd reaps it on SIGCHLD,
        # so count a zombie only if it is still there 200 ms later.
        z=$(ps -o pid=,stat= --ppid "$dpid" 2>/dev/null | awk '$2 ~ /^Z/ { print $1 }' || true)
        if [[ -n "$z" ]]; then
            sleep 0.2
            for p in $z; do
                if [[ "$(ps -o stat= -p "$p" 2>/dev/null || true)" == Z* ]]; then
                    zombies_seen=$((zombies_seen + 1))
                fi
            done
        fi
    fi
    sleep 0.05
done
rc=0
wait "$SIM_PID" || rc=$?
SIM_PID=""

# 1. The CLI run itself.
if [[ $rc -eq 0 ]]; then
    pass "reliefctl simulate flood --camps $CAMPS --seed $SEED exited 0"
else
    fail "reliefctl simulate exited $rc"
    sed 's/^/     | /' "$REPORT" >&2
fi

# 2. Message counts for this run (the log holds exactly one run: fresh RELIEF_HOME).
hello=$(count ' event=hello ' "$LOG")
bye=$(count ' event=bye ' "$LOG")
req=$(count ' event=request ' "$LOG")
check "$hello HELLO" "expected $CAMPS HELLO, got $hello" [ "$hello" -eq "$CAMPS" ]
check "$bye BYE" "expected $CAMPS BYE, got $bye" [ "$bye" -eq "$CAMPS" ]
check "$req requests" "expected $ROWS requests, got $req" [ "$req" -eq "$ROWS" ]
if grep -q "event=daemon_stop hello=$CAMPS request=$ROWS bye=$CAMPS" "$LOG"; then
    pass "reliefd's own totals agree (daemon_stop hello=$CAMPS request=$ROWS bye=$CAMPS)"
else
    fail "daemon_stop totals: $(grep -o 'event=daemon_stop.*' "$LOG" || echo none)"
fi
exits=$(count 'event=camp_exit .* status=0 ' "$LOG")
check "$exits camps exited with status 0" "expected $CAMPS clean camp exits, got $exits" [ "$exits" -eq "$CAMPS" ]

# 3. No zombies: none lingered during the run, and nothing from the run is left now.
check "no lingering zombie children of reliefd during the run" \
      "$zombies_seen zombie(s) lingered under reliefd" [ "$zombies_seen" -eq 0 ]
mapfile -t spawned < <(sed -n 's/.* event=camp_spawn .* pid=\([0-9]*\) .*/\1/p' "$LOG")
[[ -n "$dpid" ]] && spawned+=("$dpid")
left=""
for p in "${spawned[@]}"; do
    if ps -o pid= -p "$p" >/dev/null 2>&1; then
        left="$left $p($(ps -o stat= -p "$p" | tr -d ' '))"
    fi
done
check "all ${#spawned[@]} processes of the run are gone (no zombies, no orphans)" \
      "processes left behind:$left" [ -z "$left" ]
sys_z=$(ps -eo stat=,comm= | awk '$1 ~ /^Z/ && ($2 == "camp" || $2 == "reliefd")' | wc -l)
check "no zombie camp/reliefd processes anywhere on the system" \
      "$sys_z zombie camp/reliefd processes on the system" [ "$sys_z" -eq 0 ]

# 4. IPC objects removed.
check "no leftover queue $MQ" "leftover queue $MQ" [ ! -e "$MQ" ]
check "no leftover shared memory $SHM" "leftover shared memory $SHM" [ ! -e "$SHM" ]
check "pid file removed" "reliefd.pid left behind" [ ! -e "$RELIEF_HOME/reliefd.pid" ]
check "camp table camps.pids removed" "camps.pids left behind" [ ! -e "$RELIEF_HOME/camps.pids" ]

# 5. Non-empty report.
if grep -q "^  requests: $ROWS " "$REPORT" && grep -q '^Busiest camp: [0-9]' "$REPORT" &&
   [[ $(grep -cE '^ +[0-9]+  [A-Za-z0-9]+ +[0-9]+ +[0-9.]+%$' "$REPORT") -eq $CAMPS ]]; then
    pass "report: $ROWS requests, $CAMPS camp rows, $(grep '^Busiest camp' "$REPORT")"
else
    fail "report missing or incomplete:"
    sed 's/^/     | /' "$REPORT" >&2
fi

if [[ $FAILED -ne 0 ]]; then
    echo "--- reliefd stderr (tail) ---" >&2
    tail -n 20 "$OUT" >&2 2>/dev/null || true
    echo "PHASE 1 GATE M1: FAILED" >&2
    exit 1
fi
echo "PHASE 1 GATE M1: PASSED (camps reach reliefd)"
