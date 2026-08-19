#!/usr/bin/env bash
set -euo pipefail

gateway=${1:?gateway executable is required}
signal_name=${2:-SIGTERM}
test_dir=$(mktemp -d)
gateway_pid=

cleanup()
{
    if [[ -n ${gateway_pid:-} ]] && kill -0 "$gateway_pid" 2>/dev/null; then
        kill -KILL "$gateway_pid" 2>/dev/null || true
        wait "$gateway_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT

"$gateway" "$test_dir/gateway.log" >"$test_dir/stdout.log" 2>"$test_dir/stderr.log" &
gateway_pid=$!
for _ in {1..50}; do
    grep -q "gateway running" "$test_dir/stdout.log" && break
    kill -0 "$gateway_pid" 2>/dev/null || exit 1
    sleep 0.02
done
grep -q "gateway running" "$test_dir/stdout.log"
kill -s "$signal_name" "$gateway_pid"
for _ in {1..100}; do
    kill -0 "$gateway_pid" 2>/dev/null || break
    sleep 0.02
done
if kill -0 "$gateway_pid" 2>/dev/null; then
    echo "gateway exceeded graceful shutdown deadline" >&2
    exit 1
fi
wait "$gateway_pid"
gateway_pid=
grep -q "gateway stopped cleanly" "$test_dir/stdout.log"
grep -q "gateway started" "$test_dir/gateway.log"
grep -q "shutdown requested; draining logger" "$test_dir/gateway.log"
