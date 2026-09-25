#!/bin/sh
set -eu

: "${GATEWAY_EXE:?GATEWAY_EXE is required}"
: "${GATEWAYCTL_EXE:?GATEWAYCTL_EXE is required}"
: "${PROJECT_VERSION:?PROJECT_VERSION is required}"

test_dir=$(mktemp -d)
gateway_pid=""
active_socket=""

cleanup()
{
    if [ -n "$gateway_pid" ]; then
        kill -TERM "$gateway_pid" 2>/dev/null || true
        wait "$gateway_pid" 2>/dev/null || true
    fi
    if [ -n "$active_socket" ]; then
        rm -f "$active_socket"
    fi
    rm -rf "$test_dir"
}
trap cleanup EXIT HUP INT TERM

wait_for_socket()
{
    socket_path=$1
    attempts=0
    while [ "$attempts" -lt 30 ]; do
        [ -S "$socket_path" ] && return 0
        sleep 0.1
        attempts=$((attempts + 1))
    done
    return 1
}

run_version_case()
{
    label=$1
    socket_value=$2
    expected_socket=$3

    active_socket=$expected_socket
    rm -f "$expected_socket"

    EDGEVISION_COMMAND_SOCKET="$socket_value" \
        "$GATEWAY_EXE" --source simulated \
        "$test_dir/$label.db" "$test_dir/$label.log" \
        >"$test_dir/$label.stdout" 2>"$test_dir/$label.stderr" &
    gateway_pid=$!

    if ! wait_for_socket "$expected_socket"; then
        echo "gateway did not create expected socket: $expected_socket" >&2
        cat "$test_dir/$label.stderr" >&2
        return 1
    fi

    reply=$(EDGEVISION_COMMAND_SOCKET="$socket_value" \
        "$GATEWAYCTL_EXE" get_version)
    last_line=$(printf '%s\n' "$reply" | tail -n 1)
    expected_reply="ok version=$PROJECT_VERSION"
    if [ "$last_line" != "$expected_reply" ]; then
        echo "unexpected version reply: $last_line" >&2
        return 1
    fi

    kill -TERM "$gateway_pid"
    wait "$gateway_pid"
    gateway_pid=""
    [ ! -e "$expected_socket" ]
    active_socket=""
}

run_version_case custom "$test_dir/control.sock" "$test_dir/control.sock"

default_socket=/tmp/edgevision-study.sock
if [ -e "$default_socket" ]; then
    echo "default command socket already exists: $default_socket" >&2
    exit 1
fi
run_version_case default "" "$default_socket"

long_path=$(awk 'BEGIN { for (i = 0; i < 108; ++i) printf "x" }')
if EDGEVISION_COMMAND_SOCKET="$long_path" \
        "$GATEWAYCTL_EXE" get_version \
        >"$test_dir/long.stdout" 2>"$test_dir/long.stderr"; then
    echo "gatewayctl accepted an overlong command socket path" >&2
    exit 1
fi
grep -F "command socket path too long" "$test_dir/long.stderr" >/dev/null

printf 'PASS gateway and gatewayctl share configurable command socket\n'
