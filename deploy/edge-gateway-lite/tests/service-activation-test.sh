#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
helper="$script_dir/../scripts/service-activate.sh"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

cat >"$test_dir/systemctl" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >>"$SYSTEMCTL_CALLS"
if [ "${SYSTEMCTL_FAIL_RESTART:-0}" = 1 ] && [ "${1:-}" = restart ]; then
    exit 1
fi
exit 0
EOF
chmod 0755 "$test_dir/systemctl"

calls="$test_dir/calls"
SYSTEMCTL_CALLS="$calls" PATH="$test_dir:$PATH" \
    sh "$helper" edge-gateway-lite.service

cat >"$test_dir/expected-success" <<'EOF'
daemon-reload
enable edge-gateway-lite.service
restart edge-gateway-lite.service
EOF
cmp "$test_dir/expected-success" "$calls"

: >"$calls"
if SYSTEMCTL_CALLS="$calls" SYSTEMCTL_FAIL_RESTART=1 PATH="$test_dir:$PATH" \
        sh "$helper" edge-gateway-lite.service; then
    echo "service activation succeeded after restart failure" >&2
    exit 1
fi

cat >"$test_dir/expected-failure" <<'EOF'
daemon-reload
enable edge-gateway-lite.service
restart edge-gateway-lite.service
--no-pager --full status edge-gateway-lite.service
EOF
cmp "$test_dir/expected-failure" "$calls"

printf 'PASS service activation always restarts and reports failure\n'
