#!/bin/sh
set -eu

repo=/home/zoukunbo/project/edgevision-gateway
bundle="$repo/deploy/nfs-root/edgevision-outbox"
board=root@192.168.0.232
stage=/tmp/edgevision-board-install

test -f "$bundle/SHA256SUMS"
ssh "$board" "rm -rf '$stage' && mkdir -p '$stage'"
scp "$bundle/tools/mount.nfs" "$board:$stage/mount.nfs"
scp "$bundle/systemd/mnt-edgevision.mount" "$board:$stage/mnt-edgevision.mount"
scp "$bundle/systemd/edgevision-outbox.service" "$board:$stage/edgevision-outbox.service"

ssh "$board" "set -eu
install -d -m 0755 /mnt/edgevision /userdata/edgevision-gateway /etc/systemd/system
install -m 0755 '$stage/mount.nfs' /usr/sbin/mount.nfs
ln -sf mount.nfs /usr/sbin/mount.nfs4
install -m 0644 '$stage/mnt-edgevision.mount' /etc/systemd/system/mnt-edgevision.mount
install -m 0644 '$stage/edgevision-outbox.service' /etc/systemd/system/edgevision-outbox.service
systemctl daemon-reload
systemctl enable --now mnt-edgevision.mount
cd /mnt/edgevision/edgevision-outbox
sha256sum -c SHA256SUMS
systemctl enable edgevision-outbox.service
systemctl start edgevision-outbox.service
systemctl show edgevision-outbox.service -p Result -p ExecMainStatus -p ActiveState
rm -rf '$stage'"
