#!/bin/sh
# Edge Gateway Lite 的首次安装入口，在目标板上以 root 运行。
#
# 默认从 /mnt/edgevision/edge-gateway-lite 读取 prepare-bundle.sh 生成的发布包；
# 先调用 install-payload.sh 校验并安装文件，再激活 systemd 服务并执行健康检查。
#
# 本脚本用于首次安装；已有运行版本时应使用 upgrade-board.sh，
# 否则会绕过升级前备份、版本门槛和失败回滚。

# 任一未处理命令失败或引用未定义变量时立即退出。
set -eu

# 环境变量可在测试或非默认挂载位置下覆盖路径。
bundle=${EDGEVISION_BUNDLE:-/mnt/edgevision/edge-gateway-lite}
install_dir=${EDGEVISION_INSTALL_DIR:-/userdata/edgevision-gateway/edge-gateway-lite}

# 安装需要写入 /userdata、/etc 和 systemd 单元目录，因此必须为 root。
if [ "$(id -u)" -ne 0 ]; then
    echo "run as root: $0" >&2
    exit 1
fi

# 第一阶段：校验 SHA256 清单，并把程序、库、配置模板、脚本和单元文件
# 复制到持久化目录。这一步不启动服务。
EDGEVISION_BUNDLE=$bundle "$bundle/scripts/install-payload.sh"
# 第二阶段：重载 systemd、设置开机启动并显式重启服务。
"$install_dir/scripts/service-activate.sh" edge-gateway-lite.service
# 给 Type=simple 服务留出最小初始化时间，然后再读取状态。
sleep 3
if ! systemctl is-active --quiet edge-gateway-lite.service; then
    systemctl --no-pager --full status edge-gateway-lite.service || true
    exit 1
fi
# 最后验证实际运行版本、设备节点和数据库；失败时返回非零。
"$install_dir/scripts/health-check.sh"

echo "installed edge-gateway-lite; run health check:"
echo "$install_dir/scripts/health-check.sh"
