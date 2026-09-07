#!/bin/sh
set -eu

config=${EDGEVISION_CONFIG:-/etc/edgevision-gateway/edge-gateway-lite.env}
if [ -r "$config" ]; then
    . "$config"
fi

bundle=${EDGEVISION_BUNDLE:-/userdata/edgevision-gateway/edge-gateway-lite}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
database=${EDGEVISION_DATABASE:-$state_dir/gateway.db}
source_kind=${EDGEVISION_SOURCE:-stm32}
serial=${EDGEVISION_SERIAL:-/dev/ttyS5}
gpiochip=${EDGEVISION_GPIOCHIP:-/dev/gpiochip0}
failed=0

check_ok() {
    printf 'OK %s\n' "$1"
}

check_fail() {
    printf 'FAIL %s\n' "$1" >&2
    failed=1
}

if systemctl is-active --quiet edge-gateway-lite.service; then
    check_ok "service active"
else
    check_fail "service inactive"
fi

if [ -x "$bundle/bin/gateway" ]; then
    version=$(LD_LIBRARY_PATH="$bundle/lib" "$bundle/bin/gateway" --version 2>&1) || {
        check_fail "version probe failed"
        version=""
    }
    [ -z "$version" ] || check_ok "version=$version"
else
    check_fail "gateway executable missing"
fi

if [ "$source_kind" = stm32 ]; then
    [ -c "$serial" ] && check_ok "serial=$serial" || check_fail "serial=$serial"
    [ -c "$gpiochip" ] && check_ok "gpiochip=$gpiochip" || check_fail "gpiochip=$gpiochip"
fi

if [ -f "$database" ]; then
    if command -v sqlite3 >/dev/null 2>&1; then
        integrity=$(sqlite3 "$database" 'PRAGMA integrity_check;' 2>/dev/null) || integrity="error"
        [ "$integrity" = ok ] && check_ok "database integrity=ok" || check_fail "database integrity=$integrity"
        stats=$(sqlite3 "$database" \
            "SELECT (SELECT COUNT(*) FROM measurements),
                    (SELECT COUNT(*) FROM outbox WHERE state='pending'),
                    (SELECT COUNT(*) FROM outbox WHERE state='sent');" 2>/dev/null) || stats=""
        [ -n "$stats" ] && check_ok "measurements|pending|sent=$stats" || check_fail "database stats unavailable"
    else
        check_fail "sqlite3 command missing"
    fi
else
    check_fail "database missing: $database"
fi

if [ "$failed" -ne 0 ]; then
    exit 1
fi
check_ok "edge-gateway-lite healthy"
