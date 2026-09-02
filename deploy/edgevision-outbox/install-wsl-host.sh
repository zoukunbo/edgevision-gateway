#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

repo=/home/zoukunbo/project/edgevision-gateway
export_root="$repo/deploy/nfs-root"
board_ip=192.168.0.232
export_line="$export_root $board_ip(ro,sync,no_subtree_check,insecure,fsid=0)"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

awk -v root="$export_root" 'NF == 0 || $1 != root { print }' /etc/exports > "$tmp"
printf '%s\n' "$export_line" >> "$tmp"
install -m 0644 "$tmp" /etc/exports
exportfs -rav
systemctl restart nfs-kernel-server

install -m 0644 "$repo/deploy/edgevision-outbox/systemd/edgevision-board-mqtt-tunnel.service" \
    /etc/systemd/system/edgevision-board-mqtt-tunnel.service
systemctl daemon-reload
systemctl enable --now edgevision-board-mqtt-tunnel.service
systemctl --no-pager --full status nfs-kernel-server edgevision-board-mqtt-tunnel.service
