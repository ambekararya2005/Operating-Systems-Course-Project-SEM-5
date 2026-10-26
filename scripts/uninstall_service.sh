#!/usr/bin/env bash
# OS concept: System boot and service management with systemd — Unit 6
#
# uninstall_service.sh: stop and remove reliefd.service (installed by install_service.sh).
# Prints every sudo command before running it.
#   usage: uninstall_service.sh [-n] [-p]
#     -n  only print the commands
#     -p  purge: also remove /var/lib/reliefos (logs, journal) and the relief-admin group
set -euo pipefail

PROG=${0##*/}
UNIT=/etc/systemd/system/reliefd.service

dry=0
purge=0
while getopts ":nph" opt; do
    case "$opt" in
        n) dry=1 ;;
        p) purge=1 ;;
        h) sed -n 's/^#   //p; s/^#     /  /p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }

run() {         # print, then run unless -n
    echo "+ $*"
    (( dry )) || "$@"
}

echo "Removing reliefd.service:"
run sudo systemctl disable --now reliefd
run sudo rm -f "$UNIT"
run sudo systemctl daemon-reload
run sudo systemctl reset-failed reliefd
if (( purge )); then
    run sudo rm -rf /var/lib/reliefos
    run sudo groupdel relief-admin
fi
