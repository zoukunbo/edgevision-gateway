#!/usr/bin/env bash

set -euo pipefail

gateway=${1:?gateway executable is required}
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT

"$gateway" --smoke "$test_dir/gateway.log" >"$test_dir/stdout.log" 2>"$test_dir/stderr.log"

grep -q '^SMOKE_PASS$' "$test_dir/stdout.log"
grep -q 'gateway started' "$test_dir/gateway.log"
grep -q 'gateway smoke passed: loopback TCP request/response' "$test_dir/gateway.log"

if grep -q 'SMOKE_FAIL' "$test_dir/stdout.log"; then
    echo "gateway reported smoke failure" >&2
    exit 1
fi
