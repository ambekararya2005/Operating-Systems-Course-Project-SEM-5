#!/usr/bin/env bash
# OS concept: Shell programming — a guided demo of ReliefOS, one syllabus topic per step (Unit 1)
#
# demo.sh: walks through what ReliefOS shows (Phases 1-6), pausing after each step.
#   usage: demo.sh [-y|--auto] [-u]
#     -y, --auto  do not pause between steps (run straight through)
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
#   8. memory: the warehouse floor under the buddy system and slab caches (memsim --show),
#      and LRU page replacement on the textbook reference string (pagesim --verbose)  (Unit 4)
#   9. disk scheduling: the truck's route under SSTF and C-SCAN (routesim track diagrams);
#      the journal: kill -9 of reliefd, restart, event=recovered                    (Unit 5)
#  10. boot and syscalls: systemctl status reliefd (if installed), then 10 key lines of a
#      request's strace with what made each one (docs/SYSCALLS.md)                  (Unit 6)
# Exit status: 0 done, 1 a step failed, 2 usage error.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CTL="$ROOT/scripts/reliefctl"
PROG=${0##*/}
export RELIEF_HOME=${RELIEF_HOME:-$ROOT/run/demo}
export RELIEF_TIME_SCALE=${RELIEF_TIME_SCALE:-50}

pause=1
ui=0
args=()
for a in "$@"; do                   # long option for getopts
    [[ $a == --auto ]] && a=-y
    args+=("$a")
done
set -- "${args[@]+"${args[@]}"}"
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
next

step "8. Memory: the warehouse floor (1024 units) as memory, a camp's shelves as page frames (Unit 4)"
echo "Buddy system: each shipment gets a power-of-two block; '~' is space wasted inside a block:"
"$ROOT/build/memsim" --alloc buddy --seed 1 --ops 60 --show 30 --width 96 | grep -A1 '^after'
echo
echo "Slab caches: F/M/H are FOOD/MED/HYGIENE kits in their own slabs, '_' a free slot:"
"$ROOT/build/memsim" --alloc slab --seed 1 --ops 60 --show 60 --width 96 | grep -A1 '^after'
echo
echo "LRU on the textbook reference string with 3 shelf slots (frames):"
"$ROOT/build/pagesim" --policy lru --frames 3 --refs 7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1 --verbose
next

step "9. Files: the highway as a disk (routesim), the Banker's journal surviving kill -9 (Unit 5)"
echo "SSTF: always the nearest stop next (236 km on the textbook queue):"
"$ROOT/build/routesim" --policy sstf --stops 98,183,37,122,14,124,65,67 --width 80
echo
echo "C-SCAN: sweep up to the end, jump back to 0, sweep up again (382 km incl. the 199 km jump):"
"$ROOT/build/routesim" --policy cscan --stops 98,183,37,122,14,124,65,67 --width 80
next
unset RELIEF_CAMPS RELIEF_SCENARIO
"$CTL" start
sleep 4
pid=$(cat "$RELIEF_HOME/reliefd.pid")
echo "reliefd (pid $pid) has fsync'ed $(awk '/ event=daemon_start /{n = 0} / event=grant /{n++} END {print n + 0}' "$RELIEF_HOME/logs/reliefd.log") grants to journal.log; kill -9 $pid:"
kill -KILL "$pid"
sleep 2                                  # the camps die with it (PR_SET_PDEATHSIG)
"$CTL" start
sleep 1
echo "the restarted reliefd replayed snapshot.bin + journal.log:"
grep ' event=recovered ' "$RELIEF_HOME/logs/reliefd.log" | tail -1 | sed 's/^ts=[^ ]* //'
"$CTL" stop
next

step "10. Boot and system calls: reliefd as a systemd service, one request under strace (Unit 6)"
if systemctl cat reliefd.service > /dev/null 2>&1; then
    systemctl --no-pager status reliefd 2>&1 | head -n 8 || true
else
    echo "reliefd.service is not installed (scripts/install_service.sh installs it): skipped."
fi
echo
echo "10 key system calls of one request (scripts/trace_request.sh; full table in docs/SYSCALLS.md):"
awk -F' \\| ' -v want=" 2 4 5 7 8 9 10 11 13 14 " '/^\| [0-9]+ \|/ && index(want, " " substr($1, 3) " ") {
        line = $2; why = $3
        gsub(/\\\|/, "|", line); gsub(/`/, "", line); gsub(/`/, "", why); sub(/ \|$/, "", why)
        sub(/^[0-9]+ \.[0-9]+ /, "", line)
        printf "%2d. %s\n      %s\n", ++n, substr(line, 1, 92), why }' "$ROOT/docs/SYSCALLS.md"

echo
echo "Demo done. Logs: $RELIEF_HOME/logs/reliefd.log (awk-friendly key=value)."
