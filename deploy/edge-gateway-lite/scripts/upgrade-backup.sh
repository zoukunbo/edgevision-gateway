#!/bin/sh

upgrade_validate_database_path()
{
    [ "$#" -eq 2 ] || return 1
    upgrade_path=$1
    upgrade_root=${2%/}
    [ -n "$upgrade_root" ] && [ "$upgrade_root" != / ] || return 1
    case "$upgrade_path" in
        "$upgrade_root"/*) ;;
        *) return 1 ;;
    esac
    case "$upgrade_path" in
        *'
'*|*'*'*|*'?'*|*'['*|*']'*|*/../*|*/..) return 1 ;;
    esac
    [ -n "${upgrade_path##*/}" ] || return 1
    upgrade_root_real=$(readlink -f "$upgrade_root") || return 1
    upgrade_parent_real=$(readlink -f "$(dirname "$upgrade_path")") || return 1
    case "$upgrade_parent_real/" in "$upgrade_root_real"/*) ;; *) return 1 ;; esac
}

upgrade_validate_managed_path()
{
    [ "$#" -eq 2 ] || return 1
    upgrade_managed_path=$1
    upgrade_managed_root=${2%/}
    case "$upgrade_managed_path" in
        "$upgrade_managed_root"/*) ;;
        *) return 1 ;;
    esac
    case "$upgrade_managed_path" in
        *'
'*|*'*'*|*'?'*|*'['*|*']'*|*/../*|*/..) return 1 ;;
    esac
    [ -n "${upgrade_managed_path##*/}" ] || return 1
    upgrade_managed_root_real=$(readlink -f "$upgrade_managed_root") || return 1
    if [ -e "$upgrade_managed_path" ]; then
        upgrade_managed_real=$(readlink -f "$upgrade_managed_path") || return 1
    else
        upgrade_managed_parent=$(readlink -f "$(dirname "$upgrade_managed_path")") || return 1
        upgrade_managed_real=$upgrade_managed_parent/${upgrade_managed_path##*/}
    fi
    case "$upgrade_managed_real" in "$upgrade_managed_root_real"/*) ;; *) return 1 ;; esac
}

upgrade_validate_layout()
{
    upgrade_state_root=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
    upgrade_validate_database_path "$UPGRADE_DATABASE" "$upgrade_state_root" || return 1
    upgrade_validate_managed_path "$UPGRADE_INSTALL_DIR" "$upgrade_state_root" || return 1
    upgrade_validate_managed_path "$UPGRADE_BACKUP_ROOT" "$upgrade_state_root" || return 1
    [ "$UPGRADE_BACKUP_ROOT" != "$upgrade_state_root" ]
}

upgrade_manifest_get()
{
    [ "$#" -eq 2 ] || return 1
    upgrade_manifest=$1
    upgrade_key=$2
    case "$upgrade_key" in
        running_version|candidate_version|install_dir|unit_file|config_file|database_path|\
        db_exists|db_size|db_sha256|wal_exists|wal_size|wal_sha256|\
        shm_exists|shm_size|shm_sha256|complete) ;;
        *) return 1 ;;
    esac
    awk -F= -v key="$upgrade_key" '
        $1 == key { count += 1; value = substr($0, length(key) + 2) }
        END { if (count == 1) print value; else exit 1 }
    ' "$upgrade_manifest"
}

upgrade_record_file()
{
    [ "$#" -eq 3 ] || return 1
    upgrade_manifest=$1
    upgrade_key=$2
    upgrade_file=$3
    if [ -f "$upgrade_file" ]; then
        upgrade_size=$(wc -c <"$upgrade_file" | tr -d ' ')
        upgrade_sha=$(sha256sum "$upgrade_file" | awk '{print $1}')
        printf '%s_exists=1\n%s_size=%s\n%s_sha256=%s\n' \
            "$upgrade_key" "$upgrade_key" "$upgrade_size" \
            "$upgrade_key" "$upgrade_sha" >>"$upgrade_manifest"
    else
        printf '%s_exists=0\n%s_size=0\n%s_sha256=-\n' \
            "$upgrade_key" "$upgrade_key" "$upgrade_key" >>"$upgrade_manifest"
    fi
}

upgrade_file_matches_manifest()
{
    [ "$#" -eq 3 ] || return 1
    upgrade_manifest=$1
    upgrade_key=$2
    upgrade_file=$3
    upgrade_expected_exists=$(upgrade_manifest_get "$upgrade_manifest" "${upgrade_key}_exists") || return 1
    if [ "$upgrade_expected_exists" = 0 ]; then
        [ ! -e "$upgrade_file" ]
        return
    fi
    [ "$upgrade_expected_exists" = 1 ] && [ -f "$upgrade_file" ] || return 1
    upgrade_expected_size=$(upgrade_manifest_get "$upgrade_manifest" "${upgrade_key}_size") || return 1
    upgrade_expected_sha=$(upgrade_manifest_get "$upgrade_manifest" "${upgrade_key}_sha256") || return 1
    upgrade_actual_size=$(wc -c <"$upgrade_file" | tr -d ' ')
    [ "$upgrade_actual_size" = "$upgrade_expected_size" ] || return 1
    upgrade_actual_sha=$(sha256sum "$upgrade_file" | awk '{print $1}')
    [ "$upgrade_actual_sha" = "$upgrade_expected_sha" ]
}

upgrade_backup_manifest_valid()
{
    upgrade_backup_dir=$1
    upgrade_manifest=$upgrade_backup_dir/manifest
    [ "$(upgrade_manifest_get "$upgrade_manifest" complete 2>/dev/null)" = 1 ] || return 1
    [ -d "$upgrade_backup_dir/install" ] || return 1
    [ -f "$upgrade_backup_dir/systemd/edge-gateway-lite.service" ] || return 1
    [ -f "$upgrade_backup_dir/config/edge-gateway-lite.env" ] || return 1
    upgrade_file_matches_manifest "$upgrade_manifest" db "$upgrade_backup_dir/database/main" || return 1
    upgrade_file_matches_manifest "$upgrade_manifest" wal "$upgrade_backup_dir/database/wal" || return 1
    upgrade_file_matches_manifest "$upgrade_manifest" shm "$upgrade_backup_dir/database/shm"
}

upgrade_backup_create()
{
    upgrade_validate_layout || return 1
    [ -d "$UPGRADE_INSTALL_DIR" ] && [ -f "$UPGRADE_UNIT_FILE" ] &&
        [ -f "$UPGRADE_CONFIG_FILE" ] || return 1

    install -d -m 0700 "$UPGRADE_BACKUP_ROOT" || return 1
    upgrade_new=$UPGRADE_BACKUP_ROOT/previous.new
    upgrade_old=$UPGRADE_BACKUP_ROOT/previous.old
    upgrade_previous=$UPGRADE_BACKUP_ROOT/previous
    rm -rf -- "$upgrade_new" "$upgrade_old" || return 1
    install -d -m 0700 "$upgrade_new/install" "$upgrade_new/systemd" \
        "$upgrade_new/config" "$upgrade_new/database" || return 1
    cp -Rp "$UPGRADE_INSTALL_DIR"/. "$upgrade_new/install/" || return 1
    install -m 0644 "$UPGRADE_UNIT_FILE" "$upgrade_new/systemd/edge-gateway-lite.service" || return 1
    install -m 0644 "$UPGRADE_CONFIG_FILE" "$upgrade_new/config/edge-gateway-lite.env" || return 1

    upgrade_manifest=$upgrade_new/manifest
    : >"$upgrade_manifest" || return 1
    printf 'running_version=%s\ncandidate_version=%s\ninstall_dir=%s\nunit_file=%s\nconfig_file=%s\ndatabase_path=%s\n' \
        "$UPGRADE_RUNNING_VERSION" "$UPGRADE_CANDIDATE_VERSION" \
        "$UPGRADE_INSTALL_DIR" "$UPGRADE_UNIT_FILE" "$UPGRADE_CONFIG_FILE" \
        "$UPGRADE_DATABASE" >>"$upgrade_manifest" || return 1

    upgrade_record_file "$upgrade_manifest" db "$UPGRADE_DATABASE" || return 1
    upgrade_record_file "$upgrade_manifest" wal "$UPGRADE_DATABASE-wal" || return 1
    upgrade_record_file "$upgrade_manifest" shm "$UPGRADE_DATABASE-shm" || return 1
    [ ! -f "$UPGRADE_DATABASE" ] || install -m 0600 "$UPGRADE_DATABASE" "$upgrade_new/database/main" || return 1
    [ ! -f "$UPGRADE_DATABASE-wal" ] || install -m 0600 "$UPGRADE_DATABASE-wal" "$upgrade_new/database/wal" || return 1
    [ ! -f "$UPGRADE_DATABASE-shm" ] || install -m 0600 "$UPGRADE_DATABASE-shm" "$upgrade_new/database/shm" || return 1
    printf 'complete=1\n' >>"$upgrade_manifest" || return 1
    upgrade_backup_manifest_valid "$upgrade_new" || return 1

    if [ -e "$upgrade_previous" ]; then
        mv "$upgrade_previous" "$upgrade_old" || return 1
    fi
    if [ "${UPGRADE_TEST_FAIL_BACKUP_PUBLISH:-0}" = 1 ] ||
       ! mv "$upgrade_new" "$upgrade_previous"; then
        [ ! -e "$upgrade_old" ] || mv "$upgrade_old" "$upgrade_previous"
        rm -rf -- "$upgrade_new"
        return 1
    fi
    if ! upgrade_backup_manifest_valid "$upgrade_previous"; then
        rm -rf -- "$upgrade_previous"
        [ ! -e "$upgrade_old" ] || mv "$upgrade_old" "$upgrade_previous"
        return 1
    fi
    rm -rf -- "$upgrade_old"
}

upgrade_database_changed()
{
    upgrade_validate_layout || return 2
    upgrade_previous=$UPGRADE_BACKUP_ROOT/previous
    upgrade_manifest=$upgrade_previous/manifest
    upgrade_backup_manifest_valid "$upgrade_previous" || return 2
    [ "$(upgrade_manifest_get "$upgrade_manifest" database_path)" = "$UPGRADE_DATABASE" ] || return 2
    upgrade_file_matches_manifest "$upgrade_manifest" db "$UPGRADE_DATABASE" || return 0
    upgrade_file_matches_manifest "$upgrade_manifest" wal "$UPGRADE_DATABASE-wal" || return 0
    upgrade_file_matches_manifest "$upgrade_manifest" shm "$UPGRADE_DATABASE-shm" || return 0
    return 1
}

upgrade_restore_program()
{
    upgrade_validate_layout || return 1
    upgrade_previous=$UPGRADE_BACKUP_ROOT/previous
    upgrade_backup_manifest_valid "$upgrade_previous" || return 1
    upgrade_quarantine=$UPGRADE_BACKUP_ROOT/program-quarantine
    rm -rf -- "$upgrade_quarantine"
    install -d -m 0700 "$upgrade_quarantine"
    [ ! -e "$UPGRADE_INSTALL_DIR" ] ||
        mv "$UPGRADE_INSTALL_DIR" "$upgrade_quarantine/install"
    install -d -m 0755 "$UPGRADE_INSTALL_DIR"
    cp -Rp "$upgrade_previous/install"/. "$UPGRADE_INSTALL_DIR/" || return 1
    install -m 0644 "$upgrade_previous/systemd/edge-gateway-lite.service" \
        "$UPGRADE_UNIT_FILE" || return 1
    install -m 0644 "$upgrade_previous/config/edge-gateway-lite.env" \
        "$UPGRADE_CONFIG_FILE" || return 1
}

upgrade_restore_database_if_changed()
{
    if upgrade_database_changed; then
        :
    else
        upgrade_change_status=$?
        [ "$upgrade_change_status" -eq 1 ] && return 0
        return 1
    fi
    upgrade_previous=$UPGRADE_BACKUP_ROOT/previous
    upgrade_manifest=$upgrade_previous/manifest
    upgrade_quarantine=$UPGRADE_BACKUP_ROOT/database-quarantine
    rm -rf -- "$upgrade_quarantine"
    install -d -m 0700 "$upgrade_quarantine"

    for upgrade_suffix in "" -wal -shm; do
        upgrade_current=$UPGRADE_DATABASE$upgrade_suffix
        [ ! -e "$upgrade_current" ] || mv "$upgrade_current" "$upgrade_quarantine/$(basename "$upgrade_current")" || return 1
    done
    [ "$(upgrade_manifest_get "$upgrade_manifest" db_exists)" = 0 ] ||
        install -m 0600 "$upgrade_previous/database/main" "$UPGRADE_DATABASE" || return 1
    [ "$(upgrade_manifest_get "$upgrade_manifest" wal_exists)" = 0 ] ||
        install -m 0600 "$upgrade_previous/database/wal" "$UPGRADE_DATABASE-wal" || return 1
    [ "$(upgrade_manifest_get "$upgrade_manifest" shm_exists)" = 0 ] ||
        install -m 0600 "$upgrade_previous/database/shm" "$UPGRADE_DATABASE-shm" || return 1

    upgrade_file_matches_manifest "$upgrade_manifest" db "$UPGRADE_DATABASE" || return 1
    upgrade_file_matches_manifest "$upgrade_manifest" wal "$UPGRADE_DATABASE-wal" || return 1
    upgrade_file_matches_manifest "$upgrade_manifest" shm "$UPGRADE_DATABASE-shm" || return 1
}

upgrade_cleanup_quarantine()
{
    upgrade_validate_layout || return 1
    rm -rf -- "$UPGRADE_BACKUP_ROOT/program-quarantine" \
        "$UPGRADE_BACKUP_ROOT/database-quarantine"
}
