#!/bin/sh
# 在目标板上安装或升级 Edge Gateway Lite，并立即验证服务是否健康。
#
# 默认从 /mnt/edgevision/edge-gateway-lite 读取发布包。若发布包位于其他位置，
# 可在命令前设置 EDGEVISION_BUNDLE，例如：
#   EDGEVISION_BUNDLE=/media/usb/edge-gateway-lite ./install-board.sh
#
# 本脚本会写入 /etc 和 /userdata、安装 systemd unit，并启用和启动服务，
# 因此必须以 root 身份运行。

# 任何命令失败或使用未定义变量时立即停止，避免继续安装不完整的发布包。
set -eu

# bundle 是本次安装的“源目录”；install_dir 是复制后的板端持久化运行目录。
# 二者有意分开：NFS/USB 卸载后，服务仍然可以从 /userdata 启动。
bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
config_dir=/etc/edgevision-gateway
install_dir=/userdata/edgevision-gateway/edge-gateway-lite
state_dir=/userdata/edgevision-gateway
unit_dir=/etc/systemd/system

# id -u 为 0 表示 root。尽早检查权限，避免执行到一半才因写 /etc 失败。
if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: $0" >&2
    exit 1
fi

# 先检查安装所需的关键文件是否存在且权限正确。set -e 会让任一检查失败时退出。
test -f "$bundle/SHA256SUMS"
test -x "$bundle/bin/gateway"
test -x "$bundle/bin/gatewayctl"
test -x "$bundle/scripts/run-gateway.sh"
test -x "$bundle/scripts/health-check.sh"
test -x "$bundle/scripts/service-activate.sh"

# 校验必须发生在修改系统配置之前，避免安装损坏、传输不完整或版本混装的发布包。
# 子 shell 中的 cd 不会改变后续命令的工作目录。
(
    cd "$bundle"
    sha256sum -c SHA256SUMS
)

# 创建配置、状态、systemd 及应用目录。重复安装时 install -d 可安全复用已有目录。
install -d -m 0755 "$config_dir" "$state_dir" "$unit_dir" \
    "$install_dir/bin" "$install_dir/lib" "$install_dir/config" \
    "$install_dir/scripts" "$install_dir/systemd"

# NFS/USB 只作为安装源。将已经校验的文件逐一复制到持久化目录，保证板端
# 重启后不依赖传输介质挂载，也能独立启动 Gateway。install 会原子地替换单个
# 目标文件，但整个目录的升级不是事务性的；因此不要并行运行两个安装进程。
install -m 0755 "$bundle/bin/gateway" "$install_dir/bin/gateway"
install -m 0755 "$bundle/bin/gatewayctl" "$install_dir/bin/gatewayctl"
install -m 0644 "$bundle/lib/libmosquitto.so.1" "$install_dir/lib/libmosquitto.so.1"
install -m 0644 "$bundle/config/edge-gateway-lite.env" "$install_dir/config/edge-gateway-lite.env"
install -m 0755 "$bundle/scripts/run-gateway.sh" "$install_dir/scripts/run-gateway.sh"
install -m 0755 "$bundle/scripts/health-check.sh" "$install_dir/scripts/health-check.sh"
install -m 0755 "$bundle/scripts/service-activate.sh" "$install_dir/scripts/service-activate.sh"
install -m 0644 "$bundle/systemd/edge-gateway-lite.service" "$install_dir/systemd/edge-gateway-lite.service"
install -m 0644 "$bundle/README.md" "$install_dir/README.md"
install -m 0644 "$bundle/SHA256SUMS" "$install_dir/SHA256SUMS"

# 默认配置只在首次安装时创建。用户已经调整过的设备路径、数据源或状态路径
# 不能被升级脚本静默覆盖；新版本模板仍保存在 install_dir/config 中供人工比较。
# 分支一：配置不存在 -> 首次安装，创建默认配置
# ! -e ：目标配置不存在
# install -m 0644: 把模板文件复制到$config_dir
# 分支二：配置存在，且BUNDLE还是旧的默认值 -> 迁移到新值
if [ ! -e "$config_dir/edge-gateway-lite.env" ]; then
    install -m 0644 "$install_dir/config/edge-gateway-lite.env" \
        "$config_dir/edge-gateway-lite.env"
elif grep -q '^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$' \
        "$config_dir/edge-gateway-lite.env"; then
    # 兼容早期版本：只迁移完全匹配的旧默认值。只要用户改过该行，就保持不变。
    # sed 指令的作用是：把配置文件里的一整行默认值，从旧路径替换成新路径。
    # s 替换命令(substitute)
    # # 分隔符(代替默认的 /)
    # 旧内容 ^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$ 要匹配的旧行
    # 新内容 EDGEVISION_BUNDLE=/userdata/edgevision-gateway/edge-gateway-lite 替换成的新值
    # -i 就地编辑(in-place),直接改写文案金
    sed -i 's#^EDGEVISION_BUNDLE=/mnt/edgevision/edge-gateway-lite$#EDGEVISION_BUNDLE=/userdata/edgevision-gateway/edge-gateway-lite#' \
        "$config_dir/edge-gateway-lite.env"
fi

# 将 unit 安装到 systemd 的系统级目录。daemon-reload 让 systemd 重新读取 unit；
install -m 0644 "$install_dir/systemd/edge-gateway-lite.service" \
    "$unit_dir/edge-gateway-lite.service"
# 显式 restart 保证已在运行的旧进程也会切换到新程序和新 unit。
"$install_dir/scripts/service-activate.sh" edge-gateway-lite.service

# 服务可能在 systemctl start 返回后才暴露串口、GPIO、数据库等初始化错误。
# 短暂等待后同时验证 systemd 状态和应用级健康，不能把瞬时 active 当成成功。
# 若服务退出，先打印完整 status 方便定位，再让安装脚本返回失败。
sleep 3
if ! systemctl is-active --quiet edge-gateway-lite.service; then
    systemctl --no-pager --full status edge-gateway-lite.service || true
    exit 1
fi
# 健康检查失败会因 set -e 使安装命令返回非零，便于自动化部署识别失败。
"$install_dir/scripts/health-check.sh"

echo "installed edge-gateway-lite; run health check:"
echo "$install_dir/scripts/health-check.sh"
