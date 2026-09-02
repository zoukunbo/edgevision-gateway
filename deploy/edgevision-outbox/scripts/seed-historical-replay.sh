#!/bin/sh
set -eu
bundle=/mnt/edgevision/edgevision-outbox
state=/userdata/edgevision-gateway
database="$state/gateway.db"
mkdir -p "$state"
LD_LIBRARY_PATH="$bundle/lib" "$bundle/bin/sqlite_outbox_delivery_demo" seed "$database" < "$bundle/share/temperature-replay.json"
echo "SEEDED_HISTORICAL_REPLAY database=$database"
