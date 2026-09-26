#!/bin/sh
# Edge Gateway Lite 的统一启动入口，由 systemd 调用，也可用于前台调试。
# 它负责加载配置、准备可写目录和动态库搜索路径，最后用 exec 启动 gateway。

# 配置错误、文件缺失或任一准备命令失败时立即退出。
set -eu

# systemd 会通过 EnvironmentFile 读取同一配置；手工执行本脚本时也主动读取它，
# 保证两种入口行为一致。EDGEVISION_CONFIG 可用于临时指定另一份配置文件。
# ${EDGEVISION_CONFIG:-默认} 变量有值则用他，未设置/为空则用默认
config=${EDGEVISION_CONFIG:-/etc/edgevision-gateway/edge-gateway-lite.env}
# -r 检查文件可读
if [ -r "$config" ]; then
    # “.” 使用当前 shell 加载 KEY=value。
    # 配置由 root 管理；这里不使用 eval，
    # 避免对变量内容进行第二次字符串解释。文件不存在时继续使用下方默认值。
    . "$config"
fi

# ${VAR:-default} 表示变量未设置或为空时使用默认值。数据库和日志默认放在
# state_dir，而不是发布包目录，因此替换程序文件不会覆盖运行状态。
bundle=${EDGEVISION_BUNDLE:-/userdata/edgevision-gateway/edge-gateway-lite}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
database=${EDGEVISION_DATABASE:-$state_dir/gateway.db}
log_path=${EDGEVISION_LOG:-$state_dir/gateway.log}
source_kind=${EDGEVISION_SOURCE:-stm32}

# 只接受程序明确支持的两种数据源。提前拒绝拼写错误，比把错误传给程序更清楚。
case "$source_kind" in
    simulated|stm32) ;;
    *)
        echo "invalid EDGEVISION_SOURCE: $source_kind" >&2
        exit 2
        ;;
esac

# -x 同时检查文件存在且可执行。
test -x "$bundle/bin/gateway" || {
    echo "gateway executable missing: $bundle/bin/gateway" >&2
    # 退出当前脚本，状态码设置为2，返回给调用方shell,systemd或父进程
    exit 2
}

# 创建状态目录和日志文件，并明确检查它们可写；发布目录仍可保持只读。
# test 的 -a 在这里表示两个条件都成立。
mkdir -p "$state_dir"
touch "$log_path"
test -w "$state_dir" -a -w "$log_path" || {
    echo "state or log path is not writable" >&2
    exit 2
}

# exec 用新程序替换当前shell进程，而不是“起一个紫荆城等他技术”
# gateway 变成脚本进程本女神，进程号pid不变，脚本没有多余的中间进程
# 对systemd很重要：守护进程截关的就是gateway本体，能正确跟踪，停止它；脚本本身不残留
# env LD_LIBRARY_PATH="$bundle/lib"
# env 先设置环境变量，再执行后面的命令
# 设置 `LD_LIBRARY_PATH="$bundle/lib"`：告诉动态链接器去 `$bundle/lib` 找共享库
# 仅为本进程设置 LD_LIBRARY_PATH，使动态链接器优先找到随发布包携带的
# libmosquitto.so.1，而不依赖板端系统库版本。
# exec 用 Gateway 替换当前 shell，使它成为 systemd 直接跟踪的主进程；退出码和
# SIGTERM 等信号不会经过中间 shell。后面的三个实参依次是数据源、数据库、日志。
exec env LD_LIBRARY_PATH="$bundle/lib" \
    "$bundle/bin/gateway" --source "$source_kind" "$database" "$log_path"
