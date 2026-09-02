#!/bin/sh
set -eu

repo=/home/zoukunbo/project/edgevision-gateway
bundle="$repo/deploy/nfs-root/edgevision-outbox"
evidence="$repo/hardware/storage/systemd-nfs-board-deployment-2026-09-01.log"
board=root@192.168.0.232

echo AUDIT_TIME
date -Is

echo BUNDLE_SHA256
(
    cd "$bundle"
    sha256sum -c SHA256SUMS
)

echo INPUT_SHA256
sha256sum "$bundle/share/temperature-replay.json"

echo INPUT_JSON
cat "$bundle/share/temperature-replay.json"
echo

echo LOCAL_EVIDENCE_SHA256
sha256sum "$evidence"

echo LOCAL_EVIDENCE_CHAIN
rg -n 'received PUBLISH|stm32-dht11-01|PUBACK_THEN_MARKED_SENT|NO_PENDING|REBOOT_VERIFICATION|measurement_count=1 pending_count=0 sent_count=1' "$evidence"

echo BOARD_FINAL_STATE
ssh -o BatchMode=yes -o ConnectTimeout=8 "$board" '
  set -eu
  systemctl is-active mnt-edgevision.mount
  systemctl is-enabled edgevision-outbox.service
  systemctl show edgevision-outbox.service -p Result -p ExecMainStatus -p ActiveState
  LD_LIBRARY_PATH=/mnt/edgevision/edgevision-outbox/lib     /mnt/edgevision/edgevision-outbox/bin/sqlite_outbox_mqtt_demo     status /userdata/edgevision-gateway/gateway.db
  journalctl -u edgevision-outbox.service --no-pager |
    grep -E "PUBACK_THEN_MARKED_SENT|NO_PENDING" | tail -n 8
'
