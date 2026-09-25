#!/bin/sh
set -eu

bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
install_dir=${EDGEVISION_INSTALL_DIR:-/userdata/edgevision-gateway/edge-gateway-lite}

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: $0" >&2
    exit 1
fi

EDGEVISION_BUNDLE=$bundle "$bundle/scripts/install-payload.sh"
"$install_dir/scripts/service-activate.sh" edge-gateway-lite.service
sleep 3
if ! systemctl is-active --quiet edge-gateway-lite.service; then
    systemctl --no-pager --full status edge-gateway-lite.service || true
    exit 1
fi
"$install_dir/scripts/health-check.sh"

echo "installed edge-gateway-lite; run health check:"
echo "$install_dir/scripts/health-check.sh"
