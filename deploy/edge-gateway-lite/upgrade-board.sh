#!/bin/sh
# Edge Gateway Lite 的事务升级入口，在已运行旧版本的目标板上以 root 运行。
#
# 数据流：安全预检 → 停止旧服务 → 备份旧版本 → 安装并启动候选版本
#          → 验证实际运行版本和健康状态 → 成功提交或恢复旧版本。
# 只有 running_version < candidate_version 且全部检查通过才输出 upgrade_committed。

# 任一未处理命令失败或引用未定义变量时立即退出。
set -eu

# 板端默认布局。可覆盖变量主要用于测试或明确的非默认部署。
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

# 统一输出稳定结果行并返回失败；finished 防止 EXIT 陷阱再次启动恢复。
result_fail()
{
    finished=1
    printf '%s\n' "$1"
    exit 1
}

# 通过正在运行的网关命令 socket 查询版本，而不是只读磁盘上的二进制。
runtime_version()
{
    output=$(EDGEVISION_COMMAND_SOCKET=${EDGEVISION_COMMAND_SOCKET:-/run/edgevision-gateway/control.sock} \
        "$install_dir/bin/gatewayctl" get_version 2>/dev/null) || return 1
    reply=$(printf '%s\n' "$output" | tail -n 1)
    case "$reply" in "ok version="*) version=${reply#ok version=} ;; *) return 1 ;; esac
    version_is_valid "$version" || return 1
    printf '%s\n' "$version"
}

# systemctl restart 返回时 socket 可能尚未就绪；在有界次数内等待期望版本。
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

# 旧服务已停止、但本次完整备份未就绪时，不安装候选版本；
# 直接重启并验证原有程序。
verify_old_after_backup_failure()
{
    systemctl restart "$service" || result_fail rollback_failed
    wait_for_version "$running_version" || result_fail rollback_failed
    "$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || result_fail rollback_failed
    result_fail backup_failed_old_restored
}

# 候选版本已安装或已启动后的完整回滚：停候选、恢复程序、
# 数据库变化时恢复数据库、重启旧服务，再验证旧版本和健康状态。
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

# OLD_STOPPED/BACKING_UP 阶段还不能依赖本次备份，收到中断或异常退出时
# 清理未发布的 previous.new，直接恢复旧服务。
restart_old_without_backup()
{
    recovering=1
    rm -rf -- "$backup_root/previous.new"
    systemctl restart "$service" || result_fail rollback_failed
    wait_for_version "$running_version" || result_fail rollback_failed
    "$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || result_fail rollback_failed
    result_fail backup_failed_old_restored
}

# 信号处理按当前事务状态选择“直接重启旧版本”或“完整回滚”。
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

# 兜底处理未被显式分支捕获的退出，避免旧服务已停止后静默结束。
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

# 仅供回归测试在指定状态注入 TERM；正常部署不设置该变量。
maybe_inject_signal()
{
    if [ "${UPGRADE_TEST_SIGNAL_AFTER_STATE:-}" = "$state" ]; then
        UPGRADE_TEST_SIGNAL_AFTER_STATE=
        export UPGRADE_TEST_SIGNAL_AFTER_STATE
        kill -TERM $$
    fi
}

# —— 安全预检：任何一项失败都必须发生在停止旧服务之前。 ——
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

# 候选版本来自新发布包；运行版本来自旧服务的 socket。
# 严格要求运行版本小于候选版本，同版或降级均拒绝。
candidate_output=$(LD_LIBRARY_PATH="$bundle/lib" "$bundle/bin/gateway" --version 2>/dev/null) || result_fail precheck_failed
case "$candidate_output" in "edge-gateway-lite "*) candidate_version=${candidate_output#edge-gateway-lite } ;; *) result_fail precheck_failed ;; esac
version_is_valid "$candidate_version" || result_fail precheck_failed
running_version=$(runtime_version) || result_fail precheck_failed
[ "$(version_compare "$running_version" "$candidate_version")" = -1 ] || result_fail precheck_failed
"$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || result_fail precheck_failed
[ "$(sqlite3 "$database" 'PRAGMA integrity_check;' 2>/dev/null)" = ok ] || result_fail precheck_failed

# 备份空间覆盖程序目录、单元文件、现场配置、数据库主文件及 WAL/SHM。
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

# —— 切换开始：从这里起任何失败或中断都必须恢复旧服务。 ——
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
# 只有 upgrade_backup_create 完成并发布 previous 后，才能进入候选版本安装。
state=BACKING_UP
upgrade_backup_create || verify_old_after_backup_failure
state=BACKUP_READY
maybe_inject_signal

# 安装层只复制文件；失败时立即走完整回滚。
if ! EDGEVISION_BUNDLE=$bundle EDGEVISION_CONFIG_DIR=$config_dir \
    EDGEVISION_INSTALL_DIR=$install_dir EDGEVISION_STATE_DIR=$state_dir \
    EDGEVISION_UNIT_DIR=$unit_dir "$bundle/scripts/install-payload.sh" >/dev/null; then
    rollback_upgrade
fi
state=CANDIDATE_INSTALLED
maybe_inject_signal
# 重载单元文件并重启服务，使新磁盘内容真正进入运行态。
if ! "$install_dir/scripts/service-activate.sh" "$service" >/dev/null 2>&1; then
    rollback_upgrade
fi
state=CANDIDATE_RUNNING
maybe_inject_signal
# 提交前同时验证 socket 返回的实际运行版本和完整健康检查。
wait_for_version "$candidate_version" || rollback_upgrade
"$install_dir/scripts/health-check.sh" >/dev/null 2>&1 || rollback_upgrade
finished=1
printf '%s\n' upgrade_committed
