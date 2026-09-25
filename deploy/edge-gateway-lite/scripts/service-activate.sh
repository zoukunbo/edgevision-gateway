#!/bin/sh
# 统一的 systemd 服务激活封装，供首次安装和事务升级共用。
# 用法：service-activate.sh [SERVICE]；未传参时默认处理 edge-gateway-lite.service。

# 任一未处理命令失败或引用未定义变量时立即退出。
set -eu

service=${1:-edge-gateway-lite.service}
# 先让 systemd 重新读取单元文件，再建立开机启动链接。
systemctl daemon-reload
systemctl enable "$service"
# enable 不会替换已运行进程，因此必须显式 restart，使磁盘上的当前版本真正运行。
# 重启失败时输出完整状态，便于看到 ExecStart 和最近错误。
if ! systemctl restart "$service"; then
    systemctl --no-pager --full status "$service" || true
    exit 1
fi
