#!/bin/sh
set -eu

bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
config_dir=/etc/edgevision-gateway
install_dir=/userdata/edgevision-gateway/edge-gateway-lite
state_dir=/userdata/edgevision-gateway
unit_dir=/etc/systemd/system

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: $0" >&2
    exit 1
fi

test -f "$bundle/SHA256SUMS"
test -x "$bundle/bin/gateway"
test -x "$bundle/scripts/run-gateway.sh"
test -x "$bundle/scripts/health-check.sh"

# 校验必须发生在修改系统配置之前，避免安装损坏或混装的发布包。
(
    cd "$bundle"
    sha256sum -c SHA256SUMS
)

install -d -m 0755 "$config_dir" "$state_dir" "$unit_dir" \
    "$install_dir/bin" "$install_dir/lib" "$install_dir/config" \
    "$install_dir/scripts" "$install_dir/systemd"

# NFS/USB 只作为安装源。将校验后的文件复制到持久化目录，保证板端重启后
# 不依赖传输介质挂载，也能独立启动 Gateway。
install -m 0755 "$bundle/bin/gateway" "$install_dir/bin/gateway"
install -m 0644 "$bundle/lib/libmosquitto.so.1" "$install_dir/lib/libmosquitto.so.1"
install -m 0644 "$bundle/config/edge-gateway-lite.env" "$install_dir/config/edge-gateway-lite.env"
install -m 0755 "$bundle/scripts/run-gateway.sh" "$install_dir/scripts/run-gateway.sh"
install -m 0755 "$bundle/scripts/health-check.sh" "$install_dir/scripts/health-check.sh"
install -m 0644 "$bundle/systemd/edge-gateway-lite.service" "$install_dir/systemd/edge-gateway-lite.service"
install -m 0644 "$bundle/README.md" "$install_dir/README.md"
install -m 0644 "$bundle/SHA256SUMS" "$install_dir/SHA256SUMS"

# 用户已经调整过的设备路径或状态路径不能被升级脚本静默覆盖。
if [ ! -e "$config_dir/edge-gateway-lite.env" ]; then
    install -m 0644 "$install_dir/config/edge-gateway-lite.env" \
        "$config_dir/edge-gateway-lite.env"
elif grep -q '^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$' \
        "$config_dir/edge-gateway-lite.env"; then
    # 只迁移旧版默认值；用户自定义的 bundle 路径仍保持不变。
    sed -i 's#^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$#EDGEVISION_BUNDLE=/userdata/edgevision-gateway/edge-gateway-lite#' \
        "$config_dir/edge-gateway-lite.env"
fi

install -m 0644 "$install_dir/systemd/edge-gateway-lite.service" \
    "$unit_dir/edge-gateway-lite.service"
systemctl daemon-reload
systemctl enable --now edge-gateway-lite.service

# 服务可能在 systemctl start 返回后才暴露初始化错误；短暂等待后必须同时验证
# systemd 状态和应用级健康，不能把瞬时 active 当成安装成功。
sleep 3
if ! systemctl is-active --quiet edge-gateway-lite.service; then
    systemctl --no-pager --full status edge-gateway-lite.service || true
    exit 1
fi
"$install_dir/scripts/health-check.sh"

echo "installed edge-gateway-lite; run health check:"
echo "$install_dir/scripts/health-check.sh"
