#!/usr/bin/env bash
# OS concept: The system call interface — tracing a request's lifecycle with strace — Unit 6
#
# trace_request.sh: run reliefd with 1 camp and 3 requests (a RESCUE, a TRUCK and a MED_KIT
# crate order) under strace, then stop it, and save:
#   docs/strace_request.txt   strace -f -tt -e trace=process,ipc,signal,desc,memory,futex
#                             (futex is in none of those classes, so it is named on its own)
#   docs/strace_count.txt     strace -f -c: every syscall counted, a second identical run
# The camp stays resident after its 3 requests, so the stop shows reliefd's kill() of it and the
# wait4() that reaps it. docs/SYSCALLS.md explains the key lines.
#   usage: trace_request.sh
#   env: BIN_DIR (default build)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
PROG=${0##*/}
[[ "${1:-}" == -h ]] && { sed -n 's/^#   //p' "${BASH_SOURCE[0]}"; exit 0; }
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
command -v strace > /dev/null || { echo "$PROG: needs strace: sudo apt install strace" >&2; exit 1; }
[[ -x "$BIN/reliefd" ]] || { echo "$PROG: build first: make" >&2; exit 1; }
for f in /dev/mqueue/reliefos_requests /dev/shm/reliefos_status; do
    [[ ! -e "$f" ]] || { echo "$PROG: $f exists (is reliefd running?). Stop it first." >&2; exit 1; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-trace.XXXXXX")
SPID=""
trap '[[ -n "$SPID" ]] && kill "$SPID" 2>/dev/null; rm -rf "$WORK"' EXIT
printf '1 Hadapsar\n' > "$WORK/camps.conf"
cat > "$WORK/scenario.csv" <<'EOF'
arrival_min,camp_id,severity,resource,quantity,service_time
0,1,CRITICAL,RESCUE,1,5
1,1,URGENT,TRUCK,1,3
2,1,ROUTINE,MED_KIT,20,4
EOF

trace_once() {      # trace_once <out file> <strace options...>
    local out=$1; shift
    export RELIEF_HOME="$WORK/home"
    rm -rf "$RELIEF_HOME"
    mkdir -p "$RELIEF_HOME/logs"
    local log="$RELIEF_HOME/logs/reliefd.log"
    strace "$@" -o "$out" "$BIN/reliefd" --camps "$WORK/camps.conf" --scenario "$WORK/scenario.csv" \
        --time-scale 20 2>> "$RELIEF_HOME/logs/reliefd.out" &
    SPID=$!
    # Done: the rescue completed and both Banker's grants were released.
    for _ in $(seq 150); do
        [[ $(grep -c -E ' event=(complete|release) ' "$log" 2>/dev/null || true) -ge 3 ]] && break
        sleep 0.1
    done
    [[ -s "$RELIEF_HOME/reliefd.pid" ]] || { echo "$PROG: reliefd did not start" >&2; exit 1; }
    kill -TERM "$(cat "$RELIEF_HOME/reliefd.pid")"
    wait "$SPID"
    SPID=""
}

trace_once "$ROOT/docs/strace_request.txt" -f -tt -e trace=process,ipc,signal,desc,memory,futex
sed -i -e "s|$WORK|\$WORK|g" -e "s|$ROOT|<repo>|g" "$ROOT/docs/strace_request.txt"
trace_once "$ROOT/docs/strace_count.txt" -f -c
echo "wrote docs/strace_request.txt ($(wc -l < "$ROOT/docs/strace_request.txt") lines) and docs/strace_count.txt"
echo "top 10 syscalls by count:"
awk '$1 ~ /^[0-9.]+$/ && $NF != "total"' "$ROOT/docs/strace_count.txt" | sort -k4,4nr | head -n 10
