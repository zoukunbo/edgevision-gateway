#!/bin/sh
set -eu

service=edge-gateway-lite.service
bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
config=${EDGEVISION_CONFIG:-/etc/edgevision-gateway/edge-gateway-lite.env}
config_dir=${EDGEVISION_CONFIG_DIR:-/etc/edgevision-gateway}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
install_dir=${EDGEVISION_INSTALL_DIR:-$state_dir/edge-gateway-lite}
unit_dir=${EDGEVISION_UNIT_DIR:-/etc/systemd/system}
backup_root=${EDGEVISION_BACKUP_ROOT:-$state_dir/upgrade-backup}
state=PRECHECK
finished=0
recovering=0

result_fail()
{
    finished=1
    printf '%s\n' "$1"
    exit 1
}

runtime_version()
{
    output=$(EDGEVISION_COMMAND_SOCKET=${EDGEVISION_COMMAND_SOCKET:-/run/edgevision-gateway/control.sock} \
        "$install_dir/bin/gatewayctl" get_version 2>/dev/null) || return 1
    reply=$(printf '%s\n' "$output" | tail -n 1)
    case "$reply" in "ok version="*) version=${reply#ok version=} ;; *) return 1 ;; esac
    version_is_valid "$version" || return 1
    printf '%s\n' "$version"
}

wait_for_version()
{
    expected_version=$1
    attempts=${UPGRADE_READY_ATTEMPTS:-20}
    delay=${UPGRADE_READY_DELAY:-1}
    while [ "$attempts" -gt 0 ]; do
        ready_version=$(runtime_version 2>/dev/null || true)
        [ "$ready_version" = "$expected_version" ] && return 0
        attempts=$((attempts - 1))
        [ "$attempts" -eq 0 ] || sleep "$delay"
    done
    return 1
}

verify_old_after_backup_failure()
{
    systemctl restart "$service" || result_fail rollback_failed
    wait_for_version "$running_version" || result_fail rollback_failed
    "$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || result_fail rollback_failed
    result_fail backup_failed_old_restored
}

rollback_upgrade()
{
    recovering=1
    state=ROLLING_BACK
    systemctl stop "$service" >/dev/null 2>&1 || result_fail rollback_failed
    upgrade_restore_program || result_fail rollback_failed
    upgrade_restore_database_if_changed || result_fail rollback_failed
    systemctl daemon-reload || result_fail rollback_failed
    systemctl enable "$service" || result_fail rollback_failed
    systemctl restart "$service" || result_fail rollback_failed
    wait_for_version "$running_version" || result_fail rollback_failed
    if "$install_dir/scripts/health-check.sh" >/dev/null 2>&1; then
        upgrade_cleanup_quarantine || result_fail rollback_failed
        result_fail upgrade_failed_rollback_ok
    fi
    result_fail upgrade_failed_rollback_unhealthy
}

restart_old_without_backup()
{
    recovering=1
    rm -rf -- "$backup_root/previous.new"
    systemctl restart "$service" || result_fail rollback_failed
    wait_for_version "$running_version" || result_fail rollback_failed
    "$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || result_fail rollback_failed
    result_fail backup_failed_old_restored
}

on_signal()
{
    trap - HUP INT TERM
    case "$state" in
        PRECHECK) result_fail precheck_failed ;;
        OLD_STOPPED|BACKING_UP) restart_old_without_backup ;;
        *) rollback_upgrade ;;
    esac
}
trap on_signal HUP INT TERM

on_exit()
{
    exit_status=$?
    trap - 0 HUP INT TERM
    [ "$finished" -eq 1 ] || [ "$recovering" -eq 1 ] ||
        case "$state" in
            OLD_STOPPED|BACKING_UP) restart_old_without_backup ;;
            BACKUP_READY|CANDIDATE_INSTALLED|CANDIDATE_RUNNING) rollback_upgrade ;;
        esac
    exit "$exit_status"
}
trap on_exit 0

maybe_inject_signal()
{
    if [ "${UPGRADE_TEST_SIGNAL_AFTER_STATE:-}" = "$state" ]; then
        UPGRADE_TEST_SIGNAL_AFTER_STATE=
        export UPGRADE_TEST_SIGNAL_AFTER_STATE
        kill -TERM $$
    fi
}

[ "$(id -u)" -eq 0 ] || result_fail precheck_failed
test -f "$bundle/SHA256SUMS" || result_fail precheck_failed
(cd "$bundle" && sha256sum -c SHA256SUMS >/dev/null) || result_fail precheck_failed
test -x "$bundle/bin/gateway" || result_fail precheck_failed
test -x "$bundle/bin/gatewayctl" || result_fail precheck_failed
test -x "$bundle/scripts/version-utils.sh" || result_fail precheck_failed
test -x "$bundle/scripts/upgrade-backup.sh" || result_fail precheck_failed
test -x "$bundle/scripts/install-payload.sh" || result_fail precheck_failed
. "$bundle/scripts/version-utils.sh"
. "$bundle/scripts/upgrade-backup.sh"
[ -r "$config" ] && . "$config"
database=${EDGEVISION_DATABASE:-$state_dir/gateway.db}

candidate_output=$(LD_LIBRARY_PATH="$bundle/lib" "$bundle/bin/gateway" --version 2>/dev/null) || result_fail precheck_failed
case "$candidate_output" in "edge-gateway-lite "*) candidate_version=${candidate_output#edge-gateway-lite } ;; *) result_fail precheck_failed ;; esac
version_is_valid "$candidate_version" || result_fail precheck_failed
running_version=$(runtime_version) || result_fail precheck_failed
[ "$(version_compare "$running_version" "$candidate_version")" = -1 ] || result_fail precheck_failed
"$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || result_fail precheck_failed
[ "$(sqlite3 "$database" 'PRAGMA integrity_check;' 2>/dev/null)" = ok ] || result_fail precheck_failed

required_kb=0
for backup_member in "$install_dir" "$unit_dir/edge-gateway-lite.service" \
    "$config_dir/edge-gateway-lite.env" "$database" "$database-wal" "$database-shm"; do
    [ -e "$backup_member" ] || continue
    member_kb=$(du -sk "$backup_member" 2>/dev/null | awk 'NR == 1 {print $1}') || result_fail precheck_failed
    case "$member_kb" in ''|*[!0-9]*) result_fail precheck_failed ;; esac
    required_kb=$((required_kb + member_kb))
done
required_bytes=$((required_kb * 1024))
available_bytes=${UPGRADE_TEST_AVAILABLE_BYTES:-}
if [ -z "$available_bytes" ]; then
    install -d -m 0700 "$backup_root"
    available_bytes=$(df -Pk "$backup_root" | awk 'NR == 2 {print $4 * 1024}')
fi
[ "$available_bytes" -ge "$required_bytes" ] || result_fail precheck_failed

systemctl stop "$service" || result_fail precheck_failed
state=OLD_STOPPED
maybe_inject_signal
UPGRADE_INSTALL_DIR=$install_dir
UPGRADE_UNIT_FILE=$unit_dir/edge-gateway-lite.service
UPGRADE_CONFIG_FILE=$config_dir/edge-gateway-lite.env
UPGRADE_DATABASE=$database
UPGRADE_BACKUP_ROOT=$backup_root
UPGRADE_RUNNING_VERSION=$running_version
UPGRADE_CANDIDATE_VERSION=$candidate_version
EDGEVISION_STATE_DIR=$state_dir
export UPGRADE_INSTALL_DIR UPGRADE_UNIT_FILE UPGRADE_CONFIG_FILE UPGRADE_DATABASE
export UPGRADE_BACKUP_ROOT UPGRADE_RUNNING_VERSION UPGRADE_CANDIDATE_VERSION EDGEVISION_STATE_DIR
state=BACKING_UP
upgrade_backup_create || verify_old_after_backup_failure
state=BACKUP_READY
maybe_inject_signal

if ! EDGEVISION_BUNDLE=$bundle EDGEVISION_CONFIG_DIR=$config_dir \
    EDGEVISION_INSTALL_DIR=$install_dir EDGEVISION_STATE_DIR=$state_dir \
    EDGEVISION_UNIT_DIR=$unit_dir "$bundle/scripts/install-payload.sh" >/dev/null; then
    rollback_upgrade
fi
state=CANDIDATE_INSTALLED
maybe_inject_signal
if ! "$install_dir/scripts/service-activate.sh" "$service" >/dev/null 2>&1; then
    rollback_upgrade
fi
state=CANDIDATE_RUNNING
maybe_inject_signal
wait_for_version "$candidate_version" || rollback_upgrade
"$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || rollback_upgrade
finished=1
printf '%s\n' upgrade_committed
