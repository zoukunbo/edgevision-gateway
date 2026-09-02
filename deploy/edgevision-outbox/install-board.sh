#!/bin/sh
set -eu
bundle=/mnt/edgevision/edgevision-outbox
install -d -m 0755 /mnt/edgevision /userdata/edgevision-gateway /etc/systemd/system
install -m 0755 "$bundle/tools/mount.nfs" /usr/sbin/mount.nfs
ln -sf mount.nfs /usr/sbin/mount.nfs4
install -m 0644 "$bundle/systemd/mnt-edgevision.mount" /etc/systemd/system/mnt-edgevision.mount
install -m 0644 "$bundle/systemd/edgevision-outbox.service" /etc/systemd/system/edgevision-outbox.service
systemctl daemon-reload
systemctl enable mnt-edgevision.mount edgevision-outbox.service
