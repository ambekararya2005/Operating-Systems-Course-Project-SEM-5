#!/usr/bin/env bash
# OS concept: Shell programming — cron-scheduled job, getopts, awk over a log (Unit 1)
#
# stockcheck.sh: warn when relief demand spikes. Counts event=request lines that reliefd
# logged in the last WINDOW minutes; if the count exceeds THRESHOLD, it prints a WARN line
# and appends it to $RELIEF_HOME/logs/stockcheck.log.
#
#   usage: stockcheck.sh [-t THRESHOLD] [-w WINDOW_MIN] [-q]
#     -t  requests allowed per window before warning (default 50)
#     -w  window length in minutes (default 10)
#     -q  quiet: print nothing when below the threshold
#   exit: 0 below threshold, 1 threshold exceeded, 2 usage error, 3 no log yet
#
# Cron (NOT installed automatically; add it with `crontab -e`). Every 10 minutes, with a
# 10-minute window so every request is counted exactly once:
#
#   */10 * * * * RELIEF_HOME=$HOME/os-course-project-sem5/run $HOME/os-course-project-sem5/scripts/stockcheck.sh -t 50 -w 10 -q
#
# cron runs with a minimal environment, so RELIEF_HOME must be set on the line. Under
# WSL2, cron only runs while the distro is up and the service is started
# (`sudo service cron start`).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RELIEF_HOME=${RELIEF_HOME:-$ROOT/run}
EVENTLOG="$RELIEF_HOME/logs/reliefd.log"
WARNLOG="$RELIEF_HOME/logs/stockcheck.log"
PROG=${0##*/}

usage() {
    sed -n 's/^#   usage: /usage: /p; s/^#     /  /p' "${BASH_SOURCE[0]}" >&2
}

threshold=50
window=10
quiet=0
while getopts ":t:w:qh" opt; do
    case "$opt" in
        t) threshold=$OPTARG ;;
        w) window=$OPTARG ;;
        q) quiet=1 ;;
        h) usage; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; usage; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; usage; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; usage; exit 2; }
[[ "$threshold" =~ ^[0-9]+$ && "$window" =~ ^[1-9][0-9]*$ ]] \
    || { echo "$PROG: -t and -w need whole numbers (-w at least 1)" >&2; exit 2; }
[[ -r "$EVENTLOG" ]] || { echo "$PROG: no log at $EVENTLOG" >&2; exit 3; }

# Log timestamps are local ISO-8601, so plain string comparison orders them.
cutoff=$(date -d "-$window minutes" +%Y-%m-%dT%H:%M:%S)
count=$(awk -v cutoff="$cutoff" '/ event=request / && substr($1, 4, 19) >= cutoff { n++ } END { print n + 0 }' "$EVENTLOG")
now=$(date +%Y-%m-%dT%H:%M:%S)

if (( count > threshold )); then
    line="$now WARN stockcheck: $count requests in the last $window min (threshold $threshold): relief stock may run short"
    echo "$line" >&2
    mkdir -p "$(dirname "$WARNLOG")"
    echo "$line" >> "$WARNLOG"
    exit 1
fi
(( quiet )) || echo "$now OK stockcheck: $count requests in the last $window min (threshold $threshold)"
exit 0
