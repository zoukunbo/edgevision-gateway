#!/bin/sh
set -eu
bundle=/mnt/edgevision/edgevision-outbox
state=/userdata/edgevision-gateway
database="$state/gateway.db"
mkdir -p "$state"
if [ ! -f "$database" ]; then
    echo "NO_DATABASE: $database; nothing to deliver"
    exit 0
fi
set +e
output=$(LD_LIBRARY_PATH="$bundle/lib" "$bundle/bin/sqlite_outbox_mqtt_demo" deliver-mqtt "$database" 2>&1)
rc=$?
set -e
printf '%s
' "$output"
if [ "$rc" -eq 0 ]; then exit 0; fi
case "$output" in
    *NO_PENDING*) exit 0 ;;
    *) exit "$rc" ;;
esac
