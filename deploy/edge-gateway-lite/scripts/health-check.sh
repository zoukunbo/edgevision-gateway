#!/bin/sh
# 对已安装的 Edge Gateway Lite 执行只读健康检查。
# 检查范围包括 systemd 状态、程序可执行性、硬件设备节点和 SQLite 数据库。
# 所有检查都会尽量执行完，以便一次看到多个问题；任一项失败最终返回退出码 1。

# 未定义变量或未显式处理的命令错误会终止脚本。预期可能失败的探测均在下方
# 使用 if、|| 或显式赋值处理，避免第一项失败就丢失后续诊断信息。
set -eu

# 与启动脚本读取同一配置，确保检查的是服务实际使用的路径和数据源。
config=${EDGEVISION_CONFIG:-/etc/edgevision-gateway/edge-gateway-lite.env}
if [ -r "$config" ]; then
    . "$config"
fi

# 设置与 run-gateway.sh 一致的默认值。serial 和 gpiochip 目前只用于健康检查；
# Gateway v0.1 仍使用程序内部对应的默认设备路径。
bundle=${EDGEVISION_BUNDLE:-/userdata/edgevision-gateway/edge-gateway-lite}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
database=${EDGEVISION_DATABASE:-$state_dir/gateway.db}
source_kind=${EDGEVISION_SOURCE:-stm32}
serial=${EDGEVISION_SERIAL:-/dev/ttyS5}
gpiochip=${EDGEVISION_GPIOCHIP:-/dev/gpiochip0}
command_socket=${EDGEVISION_COMMAND_SOCKET:-/run/edgevision-gateway/control.sock}
# failed 是累计失败标志：0 表示目前全部正常，1 表示至少有一项失败。
failed=0

# 统一输出格式，便于人工阅读，也便于部署脚本按行收集结果。
check_ok() {
    printf 'OK %s\n' "$1"
}

check_fail() {
    printf 'FAIL %s\n' "$1" >&2
    failed=1
}

# 第一层检查：systemd 是否认为服务当前处于 active 状态。
if systemctl is-active --quiet edge-gateway-lite.service; then
    check_ok "service active"
else
    check_fail "service inactive"
fi

# 第二层检查：候选程序能否运行，以及 systemd 管理的进程是否确实是该版本。
# 先保留命令退出状态，再解析输出，避免管道末端命令掩盖 gateway/gatewayctl 失败。
candidate_version=""
if [ -x "$bundle/bin/gateway" ]; then
    candidate_output=$(LD_LIBRARY_PATH="$bundle/lib" "$bundle/bin/gateway" --version 2>&1) || {
        check_fail "version probe failed"
        candidate_output=""
    }
    case "$candidate_output" in
        "edge-gateway-lite "*)
            candidate_version=${candidate_output#edge-gateway-lite }
            if [ -n "$candidate_version" ]; then
                check_ok "candidate version=$candidate_version"
            else
                check_fail "candidate version is empty"
            fi
            ;;
        "")
            check_fail "candidate version reply is empty"
            ;;
        *)
            check_fail "unexpected candidate version output: $candidate_output"
            ;;
    esac
else
    check_fail "gateway executable missing"
fi

running_version=""
if [ -x "$bundle/bin/gatewayctl" ]; then
    runtime_output=$(EDGEVISION_COMMAND_SOCKET="$command_socket" \
        "$bundle/bin/gatewayctl" get_version 2>&1) || {
        check_fail "running version probe failed"
        runtime_output=""
    }
    if [ -z "$runtime_output" ]; then
        check_fail "running version reply is empty"
    else
        running_reply=$(printf '%s\n' "$runtime_output" | tail -n 1)
        case "$running_reply" in
            "ok version="*)
                running_version=${running_reply#ok version=}
                if [ -z "$running_version" ]; then
                    check_fail "running version is empty"
                fi
                ;;
            *)
                check_fail "unexpected running version reply: $running_reply"
                ;;
        esac
    fi
else
    check_fail "gatewayctl executable missing"
fi

if [ -n "$candidate_version" ] && [ -n "$running_version" ]; then
    if [ "$running_version" = "$candidate_version" ]; then
        check_ok "running version matches candidate: $running_version"
    else
        check_fail "version mismatch: candidate=$candidate_version running=$running_version"
    fi
fi

# 第三层检查：仅真实 STM32 数据源依赖串口和 GPIO 字符设备。
# -c 要求路径确实是字符设备，而不只是同名普通文件。
if [ "$source_kind" = stm32 ]; then
    [ -c "$serial" ] && check_ok "serial=$serial" || check_fail "serial=$serial"
    [ -c "$gpiochip" ] && check_ok "gpiochip=$gpiochip" || check_fail "gpiochip=$gpiochip"
fi

# 第四层检查：数据库文件存在、sqlite3 工具可用、数据库结构可读取。
# -f 判断数据文件是否存在
if [ -f "$database" ]; then
    # 检查系统有没有安装sqlite3命令
    if command -v sqlite3 >/dev/null 2>&1; then
        # integrity_check 返回 ok 才认为数据库页结构完整。
        integrity=$(sqlite3 "$database" 'PRAGMA integrity_check;' 2>/dev/null) || integrity="error"
        [ "$integrity" = ok ] && check_ok "database integrity=ok" || check_fail "database integrity=$integrity"
        # 输出格式为 measurements 总数|待发送数|已发送数，可快速判断采集和
        # MQTT 发件箱是否持续工作。查询失败时保留空值并报告失败。
        stats=$(sqlite3 "$database" \
            "SELECT (SELECT COUNT(*) FROM measurements),
                    (SELECT COUNT(*) FROM outbox WHERE state='pending'),
                    (SELECT COUNT(*) FROM outbox WHERE state='sent');" 2>/dev/null) || stats=""
        # -n 判断变量非空，查询成功标记ok，失败标记失败
        [ -n "$stats" ] && check_ok "measurements|pending|sent=$stats" || check_fail "database stats unavailable"
    else
        check_fail "sqlite3 command missing"
    fi
else
    check_fail "database missing: $database"
fi

# 所有项目检查完后统一决定退出码：非零表示不能把当前部署视为健康。
if [ "$failed" -ne 0 ]; then
    exit 1
fi
check_ok "edge-gateway-lite healthy"
