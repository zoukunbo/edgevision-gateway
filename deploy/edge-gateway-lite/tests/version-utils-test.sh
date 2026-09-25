#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$script_dir/../scripts/version-utils.sh"

assert_compare()
{
    expected=$1
    left=$2
    right=$3
    actual=$(version_compare "$left" "$right") || {
        echo "comparison failed: $left $right" >&2
        exit 1
    }
    [ "$actual" = "$expected" ] || {
        echo "expected $left compared with $right to be $expected, got $actual" >&2
        exit 1
    }
}

assert_invalid()
{
    value=$1
    if version_is_valid "$value"; then
        echo "accepted invalid version: $value" >&2
        exit 1
    fi
    if version_compare "$value" 1.0.0 >/dev/null 2>&1; then
        echo "compared invalid version: $value" >&2
        exit 1
    fi
}

assert_compare -1 0.1.0 0.2.0
assert_compare 1 0.10.0 0.2.0
assert_compare 0 1.002.0003 1.2.3
assert_compare 1 999999999999999999999.0.0 2.0.0
assert_compare -1 2.0.0 10.0.0

assert_invalid ""
assert_invalid 1.2
assert_invalid 1.2.3.4
assert_invalid 1.2.x
assert_invalid -1.2.3

printf 'PASS strict numeric version comparison\n'
