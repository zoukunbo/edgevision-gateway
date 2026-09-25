#!/bin/sh
# 在构建主机上组装可复制到目标板的最小发布包。
#
# 用法：prepare-bundle.sh BUILD_DIR SYSROOT OUTPUT_DIR
#   BUILD_DIR  交叉编译输出目录，其中必须包含可执行文件 gateway
#   SYSROOT    目标板工具链的 sysroot，用于取得 ARM64 版 libmosquitto.so.1
#   OUTPUT_DIR 新发布包目录；可以不存在，也可以存在但必须为空
#
# 本脚本只负责收集发布文件和生成校验清单，不会编译 Gateway，也不会修改目标板。
# 使用 POSIX sh 语法，便于在精简的构建环境中运行。

# -e：任一未处理的命令失败时立即退出。
# -u：引用未定义变量时立即退出，防止空路径造成误操作。
set -eu

# 必须显式传入三个位置参数，避免错误猜测构建目录或 sysroot。
# $# ： 参数数量
# $0 ： 脚本自身名称
if [ "$#" -ne 3 ]; then
    echo "usage: $0 BUILD_DIR SYSROOT OUTPUT_DIR" >&2
    exit 2
fi

#  $1 $2 $3 三个参数位置
build_dir=$1
sysroot=$2
output_dir=$3
# 取得本脚本所在目录的绝对路径。CDPATH= 避免用户的 CDPATH 让 cd 输出额外文本；
# dirname 后的 -- 防止以连字符开头的路径被当成命令选项。
# $0 是脚本自身被调用时的路径（可能是相对路径，绝对路径或纯文件名）
# dirname -- "$0" 取出$0的目录部分（去掉最后的文件名）
# -- 是 选项结束符： 防止路径以连字符开头（如 -foo/script.sh）被误当成dirname的选项参数
# cd -- "..." 移动到该目录
# -- 防止目录名以-开头被当作cd的选项
# CDPATH= 置空环境变量
# 默认情况下 `cd` 会受 `CDPATH` 影响 —— 如果里面有 `$0` 的目录路径，`cd` 可能跑到**别的地方**
# 只有 `cd` 成功才执行 `pwd`，输出**当前工作目录的绝对路径**（经过 cd 后即脚本所在目录）
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# 在创建输出目录前先验证两个核心输入，尽量做到失败时不留下半成品。
# test -x 路径： 检查该路径存在且具有可执行权限（-x = executable）
# || 是或短路：仅当test 返回失败（文件不存在或不可执行时）才执行大括号分支
test -x "$build_dir/gateway" || {
    echo "gateway not found in build directory: $build_dir" >&2
    exit 2
}
# test -f 路径： 检查该路径存在且是常规文件（-f  = regular file）不关心是否可执行
test -f "$sysroot/usr/lib/libmosquitto.so.1" || {
    echo "libmosquitto.so.1 not found in sysroot: $sysroot" >&2
    exit 2
}

test -x "$build_dir/gatewayctl" || {
    echo "gatewayctl not found in build directory: $build_dir" >&2
    exit 2
}

# 输出目录只允许不存在或为空，避免把不同版本的二进制、共享库和脚本混在一起。
# find 找到第一个目录项便停止；-n 表示只要有任何输出就判定为非空。
# `[ -d "$output_dir" ]`输出目录**存在**且是目录
# `[ -n "$(find ...)" ]``find` 的结果**非空**（`-n` 判断字符串长度不为零）
# > &2 是把错误信息重定向到stderr
if [ -d "$output_dir" ] && [ -n "$(find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    echo "output directory is not empty: $output_dir" >&2
    exit 2
fi
# install 指令的作用是，创建固定的发布目录结构，并同时设置权限
# install -m <权限> <源文件> <目的文件>
# -d 当作目录处理(创建目录，不是复制文件)
# -m 0755 创建时直接设置权限为0755(属于主可rwx,其他人只能rx)
# 创建固定的发布目录结构。install 同时设置权限，比 mkdir 后再 chmod 更明确。
install -d -m 0755 \
    "$output_dir/bin" \
    "$output_dir/lib" \
    "$output_dir/config" \
    "$output_dir/scripts" \
    "$output_dir/systemd"

# gateway 和 shell 脚本需要执行权限；配置、库、unit 和文档只需读取权限。
# 0755：所有人可读、可执行，只有所有者可写
# 0644：所有人可读，只有所有者可写
install -m 0755 "$build_dir/gateway" "$output_dir/bin/gateway"
install -m 0755 "$build_dir/gatewayctl" "$output_dir/bin/gatewayctl"
# libmosquitto.so.1 通常是符号链接。readlink -f 复制其最终指向的真实文件，
# 这样发布包不会包含一个指向 sysroot 外部位置的失效链接。
install -m 0644 "$(readlink -f "$sysroot/usr/lib/libmosquitto.so.1")" \
    "$output_dir/lib/libmosquitto.so.1"
install -m 0644 "$script_dir/config/edge-gateway-lite.env" "$output_dir/config/"
install -m 0755 "$script_dir/scripts/run-gateway.sh" "$output_dir/scripts/"
install -m 0755 "$script_dir/scripts/health-check.sh" "$output_dir/scripts/"
install -m 0755 "$script_dir/scripts/service-activate.sh" "$output_dir/scripts/"
install -m 0755 "$script_dir/scripts/install-payload.sh" "$output_dir/scripts/"
install -m 0755 "$script_dir/scripts/version-utils.sh" "$output_dir/scripts/"
install -m 0755 "$script_dir/scripts/upgrade-backup.sh" "$output_dir/scripts/"
install -m 0644 "$script_dir/systemd/edge-gateway-lite.service" "$output_dir/systemd/"
install -m 0755 "$script_dir/install-board.sh" "$output_dir/install-board.sh"
install -m 0755 "$script_dir/upgrade-board.sh" "$output_dir/upgrade-board.sh"
install -m 0644 "$script_dir/README.md" "$output_dir/README.md"

# 为板端安装生成完整性校验清单。
# 清单不包含自身；稳定排序让相同输入得到顺序一致、可比较的发布元数据。
# 子目录中的文件先统一排序，顶层安装脚本和 README 再按固定顺序追加。
# find ... -type f -print 找出5个目录下的所有常规文件，逐个输出路径
# LC_ALL=C sort 按C语言排序规则(字节序)稳定排序，相同输入得到顺序一致的清单
# 保证可复现，可比较
# xargs sha256sum 把文件路径作为参数批量传给sha256sum，算出每个文件的哈希
# > SHA256SUM 写入文件清单（覆盖写，从头开始）
# >> 追加
(
    cd "$output_dir"
    find bin lib config scripts systemd -type f -print | LC_ALL=C sort | \
        xargs sha256sum > SHA256SUMS
    sha256sum install-board.sh upgrade-board.sh README.md >> SHA256SUMS
)

# 交叉编译产物通常不能在 x86 构建主机执行，因此这里不运行 gateway --version；
# 版本探测留给目标板上的健康检查。file 只读取 ELF 头，可用于尽早发现误把
# x86 主机程序装进 ARM64 发布包等问题。
file "$output_dir/bin/gateway"
echo "bundle ready: $output_dir"
