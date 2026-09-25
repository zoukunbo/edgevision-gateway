#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM
mkdir -p "$root/build" "$root/sysroot/usr/lib"
for file in gateway gatewayctl; do printf '#!/bin/sh\nexit 0\n' >"$root/build/$file"; chmod 0755 "$root/build/$file"; done
printf 'library\n' >"$root/sysroot/usr/lib/libmosquitto.so.1"
"$project_dir/deploy/edge-gateway-lite/prepare-bundle.sh" "$root/build" "$root/sysroot" "$root/bundle" >/dev/null
for file in upgrade-board.sh scripts/install-payload.sh scripts/version-utils.sh scripts/upgrade-backup.sh; do
    [ -x "$root/bundle/$file" ] || { echo "missing executable: $file" >&2; exit 1; }
done
(cd "$root/bundle" && sha256sum -c SHA256SUMS >/dev/null)
printf 'PASS transactional upgrade files are bundled and checksummed\n'
