#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
payload=$project_dir/deploy/edge-gateway-lite/scripts/install-payload.sh
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM
bundle=$root/bundle
mkdir -p "$bundle/bin" "$bundle/lib" "$bundle/config" "$bundle/scripts" "$bundle/systemd" "$root/tools"

for file in gateway gatewayctl; do printf '%s\n' "$file" >"$bundle/bin/$file"; chmod 0755 "$bundle/bin/$file"; done
printf 'library\n' >"$bundle/lib/libmosquitto.so.1"
printf 'EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite\n' >"$bundle/config/edge-gateway-lite.env"
for file in run-gateway.sh health-check.sh service-activate.sh install-payload.sh version-utils.sh upgrade-backup.sh; do
    printf '#!/bin/sh\nexit 0\n' >"$bundle/scripts/$file"
    chmod 0755 "$bundle/scripts/$file"
done
printf 'unit\n' >"$bundle/systemd/edge-gateway-lite.service"
printf 'readme\n' >"$bundle/README.md"
(cd "$bundle" && find bin lib config scripts systemd -type f -print | LC_ALL=C sort | xargs sha256sum >SHA256SUMS && sha256sum README.md >>SHA256SUMS)

cat >"$root/tools/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$SYSTEMCTL_CALLS"
EOF
chmod 0755 "$root/tools/systemctl"
calls=$root/systemctl.calls

run_payload()
{
    EDGEVISION_BUNDLE=$bundle \
    EDGEVISION_CONFIG_DIR=$root/etc \
    EDGEVISION_INSTALL_DIR=$root/state/edge-gateway-lite \
    EDGEVISION_STATE_DIR=$root/state \
    EDGEVISION_UNIT_DIR=$root/system \
    SYSTEMCTL_CALLS=$calls PATH="$root/tools:$PATH" sh "$payload"
}

mkdir -p "$root/state/edge-gateway-lite"
printf 'sentinel\n' >"$root/state/edge-gateway-lite/sentinel"
printf 'corrupt\n' >>"$bundle/bin/gateway"
if run_payload >/dev/null 2>&1; then
    echo 'accepted corrupt bundle' >&2
    exit 1
fi
grep -Fx sentinel "$root/state/edge-gateway-lite/sentinel" >/dev/null
(cd "$bundle" && find bin lib config scripts systemd -type f -print | LC_ALL=C sort | xargs sha256sum >SHA256SUMS && sha256sum README.md >>SHA256SUMS)

mkdir -p "$root/etc"
printf 'EDGEVISION_BUNDLE=/custom/field\n' >"$root/etc/edge-gateway-lite.env"
run_payload >/dev/null
grep -Fx 'EDGEVISION_BUNDLE=/custom/field' "$root/etc/edge-gateway-lite.env" >/dev/null
[ -x "$root/state/edge-gateway-lite/bin/gateway" ]
[ -x "$root/state/edge-gateway-lite/bin/gatewayctl" ]
[ -x "$root/state/edge-gateway-lite/scripts/install-payload.sh" ]
[ "$(stat -c %a "$root/state/edge-gateway-lite/lib/libmosquitto.so.1")" = 644 ]
[ ! -s "$calls" ]

printf 'EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite\n' >"$root/etc/edge-gateway-lite.env"
run_payload >/dev/null
[ "$(grep -c '^EDGEVISION_BUNDLE=/userdata/edgevision-gateway/edge-gateway-lite$' "$root/etc/edge-gateway-lite.env")" -eq 1 ]

printf 'PASS payload install is validated and does not activate service\n'
