#!/usr/bin/env bash
# OS concept: System boot and service management with systemd — Unit 6
#
# install_service.sh: install reliefd as the systemd service reliefd.service (boots with
# multi-user.target, restarted 2 s after a crash). Prints every sudo command before running it.
#   usage: install_service.sh [-n]
#     -n  only print the commands
# Afterwards:
#   systemctl status reliefd
#   journalctl -u reliefd -n 20
#   sudo kill -9 "$(systemctl show -p MainPID --value reliefd)"   # restarted after RestartSec=2
# Remove it with scripts/uninstall_service.sh.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PROG=${0##*/}
UNIT=/etc/systemd/system/reliefd.service
SVC_USER=${SUDO_USER:-$(id -un)}

dry=0
while getopts ":nh" opt; do
    case "$opt" in
        n) dry=1 ;;
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

[[ -x "$ROOT/build/reliefd" && -x "$ROOT/build/camp" ]] || { echo "$PROG: build first: make" >&2; exit 1; }
[[ -d /run/systemd/system ]] || { echo "$PROG: systemd is not running (WSL: [boot] systemd=true in /etc/wsl.conf)" >&2; exit 1; }
if [[ -e /dev/mqueue/reliefos_requests ]] && ! systemctl is-active --quiet reliefd; then
    echo "$PROG: a reliefd is already running (scripts/reliefctl stop first): the queue is system-wide" >&2
    exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/reliefd-unit.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT
tmp="$tmpdir/reliefd.service"
sed -e "s|@RELIEF_PREFIX@|$ROOT|g" -e "s|@RELIEF_USER@|$SVC_USER|g" "$ROOT/systemd/reliefd.service" > "$tmp"
if command -v systemd-analyze > /dev/null; then
    systemd-analyze verify "$tmp" || { echo "$PROG: systemd-analyze verify rejected the unit" >&2; exit 1; }
fi

echo "Installing reliefd.service (User=$SVC_USER, Group=relief-admin, RELIEF_HOME=/var/lib/reliefos):"
run sudo groupadd -f relief-admin
run sudo install -m 0644 "$tmp" "$UNIT"
run sudo systemctl daemon-reload
run sudo systemctl enable --now reliefd
(( dry )) && exit 0
sleep 1
systemctl --no-pager status reliefd | head -n 12
