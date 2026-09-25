#!/bin/sh
set -eu

service=${1:-edge-gateway-lite.service}
systemctl daemon-reload
systemctl enable "$service"
if ! systemctl restart "$service"; then
    systemctl --no-pager --full status "$service" || true
    exit 1
fi
