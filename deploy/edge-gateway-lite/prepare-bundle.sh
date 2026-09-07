#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 BUILD_DIR SYSROOT OUTPUT_DIR" >&2
    exit 2
fi

build_dir=$1
sysroot=$2
output_dir=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

test -x "$build_dir/gateway" || {
    echo "gateway not found in build directory: $build_dir" >&2
    exit 2
}
test -f "$sysroot/usr/lib/libmosquitto.so.1" || {
    echo "libmosquitto.so.1 not found in sysroot: $sysroot" >&2
    exit 2
}

# 输出目录只允许不存在或为空，避免把两个版本的二进制和库混在一起。
if [ -d "$output_dir" ] && [ -n "$(find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    echo "output directory is not empty: $output_dir" >&2
    exit 2
fi

install -d -m 0755 \
    "$output_dir/bin" \
    "$output_dir/lib" \
    "$output_dir/config" \
    "$output_dir/scripts" \
    "$output_dir/systemd"
install -m 0755 "$build_dir/gateway" "$output_dir/bin/gateway"
install -m 0644 "$(readlink -f "$sysroot/usr/lib/libmosquitto.so.1")" \
    "$output_dir/lib/libmosquitto.so.1"
install -m 0644 "$script_dir/config/edge-gateway-lite.env" "$output_dir/config/"
install -m 0755 "$script_dir/scripts/run-gateway.sh" "$output_dir/scripts/"
install -m 0755 "$script_dir/scripts/health-check.sh" "$output_dir/scripts/"
install -m 0644 "$script_dir/systemd/edge-gateway-lite.service" "$output_dir/systemd/"
install -m 0755 "$script_dir/install-board.sh" "$output_dir/install-board.sh"
install -m 0644 "$script_dir/README.md" "$output_dir/README.md"

# 清单不包含自身；稳定排序让同一输入得到可比较的发布元数据。
(
    cd "$output_dir"
    find bin lib config scripts systemd -type f -print | LC_ALL=C sort | \
        xargs sha256sum > SHA256SUMS
    sha256sum install-board.sh README.md >> SHA256SUMS
)

# 交叉编译产物不能假设能在构建主机执行；版本探测留给目标板健康检查。
# 这里显示 ELF 信息，及早发现误把 x86 主机程序装进 ARM64 发布包。
file "$output_dir/bin/gateway"
echo "bundle ready: $output_dir"
