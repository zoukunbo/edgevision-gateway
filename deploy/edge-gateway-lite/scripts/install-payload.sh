#!/bin/sh
set -eu

bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
config_dir=${EDGEVISION_CONFIG_DIR:-/etc/edgevision-gateway}
install_dir=${EDGEVISION_INSTALL_DIR:-/userdata/edgevision-gateway/edge-gateway-lite}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
unit_dir=${EDGEVISION_UNIT_DIR:-/etc/systemd/system}

test -f "$bundle/SHA256SUMS"
test -x "$bundle/bin/gateway"
test -x "$bundle/bin/gatewayctl"
test -f "$bundle/lib/libmosquitto.so.1"
test -f "$bundle/config/edge-gateway-lite.env"
test -x "$bundle/scripts/run-gateway.sh"
test -x "$bundle/scripts/health-check.sh"
test -x "$bundle/scripts/service-activate.sh"
test -x "$bundle/scripts/install-payload.sh"
test -f "$bundle/systemd/edge-gateway-lite.service"
test -f "$bundle/README.md"

(cd "$bundle" && sha256sum -c SHA256SUMS)

install -d -m 0755 "$config_dir" "$state_dir" "$unit_dir" \
    "$install_dir/bin" "$install_dir/lib" "$install_dir/config" \
    "$install_dir/scripts" "$install_dir/systemd"
install -m 0755 "$bundle/bin/gateway" "$install_dir/bin/gateway"
install -m 0755 "$bundle/bin/gatewayctl" "$install_dir/bin/gatewayctl"
install -m 0644 "$bundle/lib/libmosquitto.so.1" "$install_dir/lib/libmosquitto.so.1"
install -m 0644 "$bundle/config/edge-gateway-lite.env" "$install_dir/config/edge-gateway-lite.env"
for script in "$bundle"/scripts/*.sh; do
    test -f "$script"
    install -m 0755 "$script" "$install_dir/scripts/$(basename "$script")"
done
install -m 0644 "$bundle/systemd/edge-gateway-lite.service" "$install_dir/systemd/edge-gateway-lite.service"
install -m 0644 "$bundle/README.md" "$install_dir/README.md"
install -m 0644 "$bundle/SHA256SUMS" "$install_dir/SHA256SUMS"

if [ ! -e "$config_dir/edge-gateway-lite.env" ]; then
    install -m 0644 "$install_dir/config/edge-gateway-lite.env" "$config_dir/edge-gateway-lite.env"
elif grep -q '^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$' "$config_dir/edge-gateway-lite.env"; then
    sed -i 's#^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$#EDGEVISION_BUNDLE=/userdata/edgevision-gateway/edge-gateway-lite#' "$config_dir/edge-gateway-lite.env"
fi

install -m 0644 "$install_dir/systemd/edge-gateway-lite.service" "$unit_dir/edge-gateway-lite.service"
