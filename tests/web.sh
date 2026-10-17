#!/usr/bin/env bash
# OS concept: A local observer of the status table (IPC) and process control over HTTP — Units 1/2
#
# Tests web/server.py (the offline web dashboard) on a free local port:
#   1. GET / serves web/index.html; /api/status says running=false without reliefd
#   2. POST {"cmd":"start"} starts ReliefOS; /api/status then shows 8 camps with /proc states,
#      the scheduler, the Banker's state and the dock; /api/events streams log lines as JSON
#   3. the allow-list: an unknown cmd, a camp that is not 1-64/"all" (e.g. "3; touch FILE"),
#      a non-JSON POST, a foreign Origin and a foreign Host are all refused, and nothing ran
#   4. /api/experiments lists the figures of EXPERIMENTS.md; /figures/... serves them; paths
#      outside docs/figures are refused; POST {"cmd":"stop"} stops ReliefOS
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix for reliefd/reliefstat, e.g. valgrind
#   TEST_TIMEOUT  seconds to wait for each condition (default 10)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
TIMEOUT=${TEST_TIMEOUT:-10}
CTL="$ROOT/scripts/reliefctl"

RELIEF_HOME=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
export RELIEF_HOME RELIEF_BIN_DIR="$BIN" RELIEF_TIME_SCALE=50
SPID=""
cleanup() {
    if [[ -n "$SPID" ]]; then
        kill "$SPID" 2>/dev/null || true
        wait "$SPID" 2>/dev/null || true     # 143: it was stopped by our SIGTERM
    fi
    "$CTL" stop >/dev/null 2>&1 || true
    rm -rf "$RELIEF_HOME"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
wait_for() {
    local desc=$1; shift
    for _ in $(seq $((TIMEOUT * 10))); do
        "$@" && return 0
        sleep 0.1
    done
    fail "timed out waiting for: $desc"
}
command -v curl >/dev/null || { echo "SKIP: curl not installed"; exit 0; }
for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    [[ -e "$f" ]] && { echo "SKIP: $f exists (is reliefd running?). Stop it first." >&2; exit 1; }
done

PORT=$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
U="http://127.0.0.1:$PORT"
"$CTL" web --port "$PORT" > "$RELIEF_HOME/web.out" 2>&1 &
SPID=$!
wait_for "the server's URL line" grep -q "http://127.0.0.1:$PORT/" "$RELIEF_HOME/web.out"
json() { python3 -I -c "import json, sys; d = json.load(sys.stdin); print($1)"; }
post() { curl -s -X POST -H "Content-Type: application/json" -d "$1" "$U/api/action"; }
code() { curl -s -o /dev/null -w "%{http_code}" "$@"; }

# 1. Page and status without reliefd.
if ! { [[ $(code "$U/") == 200 ]] && curl -s "$U/" | grep -q '<title>ReliefOS Dashboard</title>'; }; then
    fail "GET / should serve index.html"
fi
[[ $(curl -s "$U/api/status" | json 'd["running"]') == False ]] || fail "status should say running=false"
pass "GET / serves the page; /api/status says not running"

# 2. Start, status, events.
[[ $(post '{"cmd":"start"}' | json 'd["ok"]') == True ]] || fail "POST start"
running8() { [[ $(curl -s "$U/api/status" | json 'sum(c["state"] == "RUNNING" for c in d.get("camps", []))') -ge 8 ]]; }
wait_for "8 RUNNING camps in /api/status" running8
st=$(curl -s "$U/api/status")
[[ $(json 'all(c["proc_state"] in "RSD" for c in d["camps"]) and d["banker"]["mode"] == "avoid" and d["dock"]["capacity"] == 8 and len(d["scheduler"]["team_list"]) == 3' <<< "$st") == True ]] \
    || fail "status content: $st"
ev=$(curl -s -N --max-time 3 "$U/api/events" || true)
[[ $(grep -c '^data: ' <<< "$ev") -ge 1 ]] || fail "no Server-Sent Events"
grep -m1 '^data: ' <<< "$ev" | cut -c7- | json '"event" in d and "fields" in d' | grep -q True || fail "event is not JSON with event/fields"
pass "start via POST; 8 camps with /proc states, Banker's, dock, teams; /api/events streams JSON lines"

# 3. The allow-list and the cross-site guards.
marker="$RELIEF_HOME/pwned"
[[ $(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" -d '{"cmd":"rm"}' "$U/api/action") == 400 ]] || fail "unknown cmd should be 400"
[[ $(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" -d "{\"cmd\":\"alert\",\"camp\":\"3; touch $marker\"}" "$U/api/action") == 400 ]] \
    || fail "a shell-ish camp should be 400"
[[ $(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: text/plain" -d '{"cmd":"stop"}' "$U/api/action") == 415 ]] || fail "text/plain should be 415"
[[ $(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" -H "Origin: http://evil.example" -d '{"cmd":"stop"}' "$U/api/action") == 403 ]] \
    || fail "a foreign Origin should be 403"
[[ $(code -H "Host: evil.example:$PORT" "$U/api/status") == 403 ]] || fail "a foreign Host should be 403"
[[ ! -e "$marker" ]] || fail "user input reached a shell"
[[ $(post '{"cmd":"dump","camp":"all"}' | json 'd["ok"]') == True ]] || fail "dump all"
[[ $(post '{"cmd":"alert","camp":2}' | json 'd["ok"] and "camp 2" in d["output"]') == True ]] || fail "alert 2"
pass "allow-list: bad cmd/camp 400, non-JSON 415, foreign Origin/Host 403, nothing executed; alert/dump work"

# 4. Experiments, figures, stop.
n=$(curl -s "$U/api/experiments" | json 'len(d)')
src=$(curl -s "$U/api/experiments" | json 'd[0]["src"]')
[[ $n -ge 1 && $(code "$U$src") == 200 ]] || fail "experiments: $n figures, first $src"
[[ $(curl -s --path-as-is -o /dev/null -w "%{http_code}" "$U/figures/../../web/server.py.png") == 404 ]] || fail "path traversal should be 404"
[[ $(post '{"cmd":"stop"}' | json 'd["ok"]') == True ]] || fail "POST stop"
[[ $(curl -s "$U/api/status" | json 'd["running"]') == False ]] || fail "status after stop"
pass "$n experiment figures served (traversal refused); stop via POST"

echo "all web tests passed"
