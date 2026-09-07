#!/bin/sh
set -eu

# systemd 会读取同一配置；手工执行时也读取它，保证两种入口行为一致。
config=${EDGEVISION_CONFIG:-/etc/edgevision-gateway/edge-gateway-lite.env}
if [ -r "$config" ]; then
    # 配置由 root 管理；这里不使用 eval，避免二次字符串解释。
    . "$config"
fi

bundle=${EDGEVISION_BUNDLE:-/userdata/edgevision-gateway/edge-gateway-lite}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
database=${EDGEVISION_DATABASE:-$state_dir/gateway.db}
log_path=${EDGEVISION_LOG:-$state_dir/gateway.log}
source_kind=${EDGEVISION_SOURCE:-stm32}

case "$source_kind" in
    simulated|stm32) ;;
    *)
        echo "invalid EDGEVISION_SOURCE: $source_kind" >&2
        exit 2
        ;;
esac

test -x "$bundle/bin/gateway" || {
    echo "gateway executable missing: $bundle/bin/gateway" >&2
    exit 2
}

# 状态目录必须可写；发布目录仍可保持只读挂载。
mkdir -p "$state_dir"
touch "$log_path"
test -w "$state_dir" -a -w "$log_path" || {
    echo "state or log path is not writable" >&2
    exit 2
}

# exec 让 Gateway 成为 systemd 直接跟踪的主进程，SIGTERM 不经过中间 shell。
exec env LD_LIBRARY_PATH="$bundle/lib" \
    "$bundle/bin/gateway" --source "$source_kind" "$database" "$log_path"
