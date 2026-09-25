#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$script_dir/../scripts/upgrade-backup.sh"

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM
state=$root/state
install_dir=$state/edge-gateway-lite
backup_root=$state/upgrade-backup
unit=$root/system/edge-gateway-lite.service
config=$root/etc/edge-gateway-lite.env
database=$state/gateway.db
mkdir -p "$install_dir/bin" "$(dirname "$unit")" "$(dirname "$config")"

configure()
{
    UPGRADE_INSTALL_DIR=$install_dir
    UPGRADE_UNIT_FILE=$unit
    UPGRADE_CONFIG_FILE=$config
    UPGRADE_DATABASE=$database
    UPGRADE_BACKUP_ROOT=$backup_root
    UPGRADE_RUNNING_VERSION=0.1.0
    UPGRADE_CANDIDATE_VERSION=0.2.0
    EDGEVISION_STATE_DIR=$state
    export UPGRADE_INSTALL_DIR UPGRADE_UNIT_FILE UPGRADE_CONFIG_FILE
    export UPGRADE_DATABASE UPGRADE_BACKUP_ROOT UPGRADE_RUNNING_VERSION
    export UPGRADE_CANDIDATE_VERSION EDGEVISION_STATE_DIR
}

write_snapshot()
{
    printf '%s\n' "$1" >"$install_dir/bin/gateway"
    printf 'unit-%s\n' "$1" >"$unit"
    printf 'config-%s\n' "$1" >"$config"
    printf 'db-%s\n' "$1" >"$database"
    printf 'wal-%s\n' "$1" >"$database-wal"
    printf 'shm-%s\n' "$1" >"$database-shm"
}

assert_original_database()
{
    grep -Fx 'db-one' "$database" >/dev/null
    grep -Fx 'wal-one' "$database-wal" >/dev/null
    grep -Fx 'shm-one' "$database-shm" >/dev/null
}

configure
write_snapshot one
upgrade_backup_create
grep -Fx 'complete=1' "$backup_root/previous/manifest" >/dev/null
[ "$(upgrade_manifest_get "$backup_root/previous/manifest" running_version)" = 0.1.0 ]

inode_before=$(stat -c %i "$database")
if upgrade_database_changed; then
    echo 'unchanged database reported changed' >&2
    exit 1
fi
upgrade_restore_database_if_changed
[ "$(stat -c %i "$database")" = "$inode_before" ]

printf 'db-two\n' >"$database"
upgrade_database_changed
upgrade_restore_database_if_changed
assert_original_database

printf 'wal-two\n' >"$database-wal"
upgrade_database_changed
upgrade_restore_database_if_changed
assert_original_database

rm -f "$database-shm"
upgrade_database_changed
upgrade_restore_database_if_changed
assert_original_database

printf 'new-sidecar\n' >"$database-wal"
rm -f "$database-shm"
upgrade_database_changed
upgrade_restore_database_if_changed
assert_original_database

old_manifest_sha=$(sha256sum "$backup_root/previous/manifest")
write_snapshot two
UPGRADE_TEST_FAIL_BACKUP_PUBLISH=1
export UPGRADE_TEST_FAIL_BACKUP_PUBLISH
if upgrade_backup_create; then
    echo 'injected publication failure succeeded' >&2
    exit 1
fi
unset UPGRADE_TEST_FAIL_BACKUP_PUBLISH
[ "$(sha256sum "$backup_root/previous/manifest")" = "$old_manifest_sha" ]

upgrade_backup_create
grep -Fx 'db-two' "$backup_root/previous/database/main" >/dev/null
[ ! -e "$backup_root/previous.old" ]
[ ! -e "$backup_root/previous.new" ]

printf 'candidate-program\n' >"$install_dir/bin/gateway"
printf 'candidate-unit\n' >"$unit"
printf 'candidate-config\n' >"$config"
upgrade_restore_program
grep -Fx two "$install_dir/bin/gateway" >/dev/null
grep -Fx unit-two "$unit" >/dev/null
grep -Fx config-two "$config" >/dev/null

sentinel=$root/sentinel
printf 'safe\n' >"$sentinel"
for bad_path in \
    /etc/passwd \
    /userdata/edgevision-gateway/../escape.db \
    '/userdata/edgevision-gateway/*.db'; do
    if upgrade_validate_database_path "$bad_path" /userdata/edgevision-gateway; then
        echo "accepted unsafe database path: $bad_path" >&2
        exit 1
    fi
done
bad_newline='/userdata/edgevision-gateway/bad
name.db'
if upgrade_validate_database_path "$bad_newline" /userdata/edgevision-gateway; then
    echo 'accepted database path containing newline' >&2
    exit 1
fi
grep -Fx safe "$sentinel" >/dev/null

UPGRADE_BACKUP_ROOT=$state/../outside
export UPGRADE_BACKUP_ROOT
if upgrade_validate_layout; then
    echo 'accepted traversing backup root' >&2
    exit 1
fi
configure

printf 'PASS atomic backup and changed-only database restore\n'
