#!/usr/bin/env bash
# OS concept: Shell programming — a guided demo of ReliefOS, one syllabus topic per step (Unit 1)
#
# demo.sh: walks through what ReliefOS shows so far (Phases 1-3), pausing after each step.
#   usage: demo.sh [-y] [-u]
#     -y  do not pause between steps (run straight through)
#     -u  include the live dashboard step (reliefctl ui): watch it, press q to continue
#   env: RELIEF_HOME (default <repo>/run/demo), RELIEF_TIME_SCALE (default 50)
#
# Steps:
#   1. processes: reliefctl start, status, ps (PCB fields from /proc), tree        (Unit 2)
#   2. scheduling: the live MLFQ scheduler (reliefctl sched)                        (Unit 2)
#   3. signals: an evacuation alert (SIGUSR1) and a status dump (SIGUSR2)          (Unit 2)
#   4. deadlock avoidance: the Banker's matrices and a safe sequence              (Unit 3)
#   5. optional (-u): the live ncurses dashboard (reliefui)                     (Units 2/3)
#   6. awk report of the run, then stop                                         (Unit 1)
#   7. deadlock: deadlock.csv under --deadlock none (a real circular wait, shown with
#      reliefctl waitfor), then under --deadlock detect (found and broken)         (Unit 3)
# Exit status: 0 done, 1 a step failed, 2 usage error.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CTL="$ROOT/scripts/reliefctl"
PROG=${0##*/}
export RELIEF_HOME=${RELIEF_HOME:-$ROOT/run/demo}
export RELIEF_TIME_SCALE=${RELIEF_TIME_SCALE:-50}

pause=1
ui=0
while getopts ":yuh" opt; do
    case "$opt" in
        y) pause=0 ;;
        u) ui=1 ;;
        h) sed -n 's/^#   //p; s/^#     /  /p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "$PROG: unknown option -$OPTARG (try -h)" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ -x "$ROOT/build/reliefd" ]] || { echo "$PROG: build first: make" >&2; exit 1; }

step() {        # step <title>
    echo
    echo "=== $1"
}
next() {
    if (( pause )); then
        read -r -p "--- press Enter for the next step " _ || true
    fi
}
cleanup() { "$CTL" stop >/dev/null 2>&1 || true; }
trap cleanup EXIT

step "1. Processes: reliefd fork/execs one camp per relief camp (Unit 2)"
RELIEF_POLICY=mlfq "$CTL" start
sleep 3
"$CTL" status
"$CTL" ps
"$CTL" tree
next

step "2. Scheduling: rescue requests on 3 teams under MLFQ with aging (Unit 2)"
"$CTL" sched
next

step "3. Signals: evacuation alert to camp 3 (SIGUSR1), status dump of camp 3 (SIGUSR2) (Unit 2)"
"$CTL" alert 3
"$CTL" dump 3
next

step "4. Deadlock avoidance: the Banker's state of the trucks, boats and kit crates (Unit 3)"
"$CTL" banker
next

if (( ui )); then
    step "5. The live dashboard (reliefui): watch it update; k kills a camp, q comes back here"
    next
    "$CTL" ui
else
    step "5. (skipped: run with -u for the live dashboard, or: scripts/reliefctl ui)"
fi

step "6. awk report of this run, then stop (Unit 1)"
sleep 2
"$CTL" report --last-run | head -40
"$CTL" stop
next

step "7. Deadlock: two camps each hold a truck and a boat and want the other's (Unit 3)"
conf=$(mktemp "${TMPDIR:-/tmp}/reliefos-demo.XXXXXX")
printf '1 Hadapsar\n2 Kothrud\n' > "$conf"
export RELIEF_CAMPS=$conf RELIEF_SCENARIO=$ROOT/scenarios/deadlock.csv
RELIEF_DOCK_ARGS="--deadlock none --resources $ROOT/scenarios/deadlock_resources.conf --exit-when-done" \
    RELIEF_TIME_SCALE=20 "$CTL" start
sleep 3
echo "--deadlock none: the camps are stuck; the wait-for graph has a cycle:"
"$CTL" waitfor
"$CTL" stop
next
RELIEF_DOCK_ARGS="--deadlock detect --detect-interval 0.2 --resources $ROOT/scenarios/deadlock_resources.conf --exit-when-done" \
    RELIEF_TIME_SCALE=20 "$CTL" start
sleep 4
echo "--deadlock detect: the detector found the cycle and preempted a truck:"
grep -E ' event=(deadlock_detected|recovery) ' "$RELIEF_HOME/logs/reliefd.log" | tail -2 | sed 's/^ts=[^ ]* //'
"$CTL" stop
rm -f "$conf"

echo
echo "Demo done. Logs: $RELIEF_HOME/logs/reliefd.log (awk-friendly key=value)."
