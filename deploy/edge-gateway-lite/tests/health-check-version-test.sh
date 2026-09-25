#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
health_check="$project_dir/deploy/edge-gateway-lite/scripts/health-check.sh"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

mkdir -p "$test_dir/tools" "$test_dir/bundle/bin" "$test_dir/bundle/lib" \
    "$test_dir/state"
touch "$test_dir/state/gateway.db"

cat >"$test_dir/tools/systemctl" <<'EOF'
#!/bin/sh
exit 0
EOF

cat >"$test_dir/tools/sqlite3" <<'EOF'
#!/bin/sh
case "$*" in
    *integrity_check*) printf 'ok\n' ;;
    *) printf '1|0|1\n' ;;
esac
EOF

cat >"$test_dir/bundle/bin/gateway" <<'EOF'
#!/bin/sh
case "${GATEWAY_MODE:-normal}" in
    normal) printf 'edge-gateway-lite 0.2.0\n' ;;
    empty) ;;
    *) exit 2 ;;
esac
EOF

cat >"$test_dir/bundle/bin/gatewayctl" <<'EOF'
#!/bin/sh
[ "${EDGEVISION_COMMAND_SOCKET:-}" = /run/edgevision-gateway/control.sock ] || {
    printf 'wrong command socket: %s\n' "${EDGEVISION_COMMAND_SOCKET:-unset}" >&2
    exit 2
}
case "${GATEWAYCTL_MODE:-match}" in
    mismatch)
        printf '已连接到网关\n'
        printf 'ok version=0.1.0\n'
        ;;
    match)
        printf '已连接到网关\n'
        printf 'ok version=0.2.0\n'
        ;;
    empty)
        ;;
    empty_version)
        printf 'ok version=\n'
        ;;
    fail|missing_socket)
        exit 1
        ;;
    *)
        printf 'unexpected test mode\n' >&2
        exit 2
        ;;
esac
EOF

chmod +x "$test_dir/tools/systemctl" "$test_dir/tools/sqlite3" \
    "$test_dir/bundle/bin/gateway" "$test_dir/bundle/bin/gatewayctl"

cat >"$test_dir/edge-gateway-lite.env" <<EOF
EDGEVISION_BUNDLE=$test_dir/bundle
EDGEVISION_STATE_DIR=$test_dir/state
EDGEVISION_DATABASE=$test_dir/state/gateway.db
EDGEVISION_SOURCE=simulated
EOF

output="$test_dir/output.txt"

run_health()
{
    GATEWAYCTL_MODE=$1 GATEWAY_MODE=${2:-normal} PATH="$test_dir/tools:$PATH" \
        EDGEVISION_CONFIG="$test_dir/edge-gateway-lite.env" \
        "$health_check" >"$output" 2>&1
}

expect_failure()
{
    mode=$1
    diagnostic=$2
    if run_health "$mode"; then
        echo "expected health check failure for mode: $mode" >&2
        cat "$output" >&2
        exit 1
    fi
    grep -F "$diagnostic" "$output" >/dev/null
}

expect_failure mismatch \
    "FAIL version mismatch: candidate=0.2.0 running=0.1.0"

printf 'PASS health check rejects running version mismatch\n'

if ! run_health match; then
    echo "expected health check to accept matching versions" >&2
    cat "$output" >&2
    exit 1
fi

grep -F "OK running version matches candidate: 0.2.0" "$output" >/dev/null
printf 'PASS health check accepts matching running version\n'

expect_failure empty "FAIL running version reply is empty"
printf 'PASS health check rejects empty running-version output\n'

expect_failure empty_version "FAIL running version is empty"
printf 'PASS health check rejects empty running version\n'

expect_failure fail "FAIL running version probe failed"
printf 'PASS health check rejects gatewayctl failure\n'

expect_failure missing_socket "FAIL running version probe failed"
grep -F "OK database integrity=ok" "$output" >/dev/null
printf 'PASS missing socket does not skip database checks\n'

if run_health match empty; then
    echo "expected health check to reject empty candidate-version output" >&2
    cat "$output" >&2
    exit 1
fi
grep -F "FAIL candidate version reply is empty" "$output" >/dev/null
printf 'PASS health check rejects empty candidate-version output\n'
