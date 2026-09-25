#!/bin/sh
# 发布包的“只安装文件”层，供 install-board.sh 和 upgrade-board.sh 共用。
#
# 职责：校验发布包和 SHA256 清单，把文件复制到 /userdata 与 /etc，
# 并保留已存在的现场配置。本脚本不调用 systemctl，不启动、停止或验证服务。

# 任一未处理命令失败或引用未定义变量时立即退出。
set -eu

# 环境变量允许测试在临时目录运行，也支持非默认板端布局。
bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
config_dir=${EDGEVISION_CONFIG_DIR:-/etc/edgevision-gateway}
install_dir=${EDGEVISION_INSTALL_DIR:-/userdata/edgevision-gateway/edge-gateway-lite}
state_dir=${EDGEVISION_STATE_DIR:-/userdata/edgevision-gateway}
unit_dir=${EDGEVISION_UNIT_DIR:-/etc/systemd/system}

# 先检查后面必须使用的发布文件，避免复制到一半才发现核心文件缺失。
test -f "$bundle/SHA256SUMS"
test -x "$bundle/bin/gateway"
test -x "$bundle/bin/gatewayctl"
test -f "$bundle/lib/libmosquitto.so.1"
test -f "$bundle/config/edge-gateway-lite.env"
test -x "$bundle/scripts/run-gateway.sh"
test -x "$bundle/scripts/health-check.sh"
test -x "$bundle/scripts/service-activate.sh"
test -x "$bundle/scripts/install-payload.sh"
test -f "$bundle/systemd/edge-gateway-lite.service"
test -f "$bundle/README.md"

# 在修改板端目录前校验发布包所有已列文件的内容。
(cd "$bundle" && sha256sum -c SHA256SUMS)

# 创建固定目录结构，然后按文件类型设置权限并复制。
# 发布包中 scripts 目录里的生产脚本全部随程序安装；tests 不在发布包中。
install -d -m 0755 "$config_dir" "$state_dir" "$unit_dir" \
    "$install_dir/bin" "$install_dir/lib" "$install_dir/config" \
    "$install_dir/scripts" "$install_dir/systemd"
install -m 0755 "$bundle/bin/gateway" "$install_dir/bin/gateway"
install -m 0755 "$bundle/bin/gatewayctl" "$install_dir/bin/gatewayctl"
install -m 0644 "$bundle/lib/libmosquitto.so.1" "$install_dir/lib/libmosquitto.so.1"
install -m 0644 "$bundle/config/edge-gateway-lite.env" "$install_dir/config/edge-gateway-lite.env"
for script in "$bundle"/scripts/*.sh; do
    test -f "$script"
    install -m 0755 "$script" "$install_dir/scripts/$(basename "$script")"
done
install -m 0644 "$bundle/systemd/edge-gateway-lite.service" "$install_dir/systemd/edge-gateway-lite.service"
install -m 0644 "$bundle/README.md" "$install_dir/README.md"
install -m 0644 "$bundle/SHA256SUMS" "$install_dir/SHA256SUMS"

# 首次安装才从模板创建现场配置；已存在的 /etc 配置不会被覆盖。
# 早期模板若还指向 /mnt，只把该默认值迁移到 /userdata 持久化副本。
if [ ! -e "$config_dir/edge-gateway-lite.env" ]; then
    install -m 0644 "$install_dir/config/edge-gateway-lite.env" "$config_dir/edge-gateway-lite.env"
elif grep -q '^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$' "$config_dir/edge-gateway-lite.env"; then
    sed -i 's#^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$#EDGEVISION_BUNDLE=/userdata/edgevision-gateway/edge-gateway-lite#' "$config_dir/edge-gateway-lite.env"
fi

# 把随发布包保存的单元文件安装到 systemd 目录；重载与重启由上层脚本处理。
install -m 0644 "$install_dir/systemd/edge-gateway-lite.service" "$unit_dir/edge-gateway-lite.service"
