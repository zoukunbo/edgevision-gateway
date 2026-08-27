# Buildroot 第三方库接入、SDK 导出与项目交叉编译指南

本文回答两个问题：

1. 如何把 `libmosquitto` 正确加入 OK1126B 的 Buildroot、目标根文件系统和交叉编译 SDK；
2. 下次遇到 SDK 中不存在的第三方库时，如何按同一流程接入，避免误用宿主机库。

## 1. 当前环境与实际状态

本机已确认：

- Buildroot 源码：`/home/zoukunbo/work/OK1126B-linux-source/buildroot`
- Buildroot 版本：2024.02
- Buildroot 自带 Mosquitto：2.0.18
- 当前 SDK：`/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot`
- CMake 工具链：`share/buildroot/toolchainfile.cmake`
- 项目：`/home/zoukunbo/project/edgevision-gateway`

OK1126B 的厂商配置名是 `rockchip_ok1126b-s`，对应AArch64配置片段
`configs/rockchip/chips/rv1126b_aarch64.config`。正确输出目录是
`output/rockchip_ok1126b-s`。

曾经直接运行 `make menuconfig` 和 `make savedefconfig` 生成的
`output/ok1126b` 是误用目录，其中配置已经退化为 `BR2_i386=y`，生成的
`libmosquitto.so.1` 是32位Intel 80386文件，绝对不能用于OK1126B。不要从该
目录导出SDK或复制库。厂商原始AArch64 defconfig没有被这个误用目录覆盖。

```bash
rg '^BR2_PACKAGE_MOSQUITTO' \
    /home/zoukunbo/work/OK1126B-linux-source/buildroot/configs/rockchip_ok1126b-s_defconfig

rg '^BR2_PACKAGE_MOSQUITTO' \
    /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/.config

/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot/bin/pkg-config \
    --modversion libmosquitto
```

第二条必须输出版本号，才能说明正在使用的 SDK 真正包含该开发库。

## 2. 先理解四个不同位置

| 位置 | 用途 | 应包含的内容 |
| --- | --- | --- |
| `output/build/` | 软件包源码和中间产物 | 临时文件，不供项目直接使用 |
| `output/staging/` | 交叉编译 sysroot | 头文件、库、`.pc`、CMake package 文件 |
| `output/target/` | 目标板根文件系统 | 运行时共享库和程序 |
| 导出的 SDK | 可搬移工具链和 staging sysroot | 编译器、toolchainfile、头文件、库及发现元数据 |

关键结论：

- 项目交叉编译依赖 `staging` 或由它导出的 SDK；
- 目标板运行依赖 `target` 中的共享库；
- 只把 `.so` 复制到板端，不能解决交叉编译缺少头文件；
- 只把库装进 SDK，也不能保证板端运行时能找到共享库。

## 3. 将 libmosquitto 加入 Buildroot

### 3.1 使用正确的板级配置

这个BSP使用带 `#include` 的分层defconfig，必须通过BSP根目录的
`build.sh buildroot-config` 修改。不要直接在Buildroot目录执行裸
`make menuconfig` 或 `make savedefconfig`；后者无法正确维护厂商的配置片段。

WSL 默认会把 Windows PATH 合并进 Linux PATH，其中常有 `Program Files`
等带空格目录。Buildroot 会在编译前主动拒绝这种 PATH。推荐只对当前命令
传入纯 Linux PATH，不必永久修改 shell 配置：

```bash
cd /home/zoukunbo/work/OK1126B-linux-source
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
./build.sh buildroot-config
```

菜单路径：

```text
Target packages
  -> Networking applications
    -> mosquitto
```

本项目只需要 MQTT 客户端库，推荐：

```text
BR2_PACKAGE_MOSQUITTO=y
# BR2_PACKAGE_MOSQUITTO_BROKER is not set
```

- 启用 `BR2_PACKAGE_MOSQUITTO` 会构建客户端库和客户端工具；
- 板端只作为发布客户端时，不必安装 Broker；
- 当前普通 TCP 不必强行启用 TLS；使用 MQTT over TLS 时再启用 OpenSSL并部署证书。

在menuconfig中保存并退出后，`buildroot-config`会自动调用厂商的
`scripts/update_defconfig.sh`，把变化写回
`buildroot/configs/rockchip_ok1126b-s_defconfig`。不要再手动执行
`make savedefconfig`。

### 3.2 构建并验证 staging/target

```bash
cd /home/zoukunbo/work/OK1126B-linux-source
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
./build.sh buildroot-make:mosquitto
```

若以前构建过 Mosquitto，但更改了 TLS、c-ares、cJSON 或静态/动态选项：

```bash
./build.sh buildroot-make:mosquitto-dirclean
./build.sh buildroot-make:mosquitto
```

验证交叉编译开发文件：

```bash
find buildroot/output/rockchip_ok1126b-s/staging/usr \
    \( -name 'mosquitto.h' \
    -o -name 'libmosquitto.so*' \
    -o -name 'libmosquitto.a' \
    -o -name 'libmosquitto.pc' \) -print
```

验证板端运行文件：

```bash
find buildroot/output/rockchip_ok1126b-s/target/usr/lib \
    -name 'libmosquitto.so*' -print

file buildroot/output/rockchip_ok1126b-s/target/usr/lib/libmosquitto.so.1
```

至少应看到头文件、目标架构库和 `libmosquitto.pc`。如果 staging 有库而 target 没有共享库，检查工具链静态/动态选项以及包安装过程。

### 3.3 重建镜像并导出 SDK

```bash
./build.sh buildroot
./build.sh buildroot-sdk
find buildroot/output/rockchip_ok1126b-s/images \
    -maxdepth 1 -type f -name '*sdk*' -print
```

把新 SDK 解压到新目录，不要直接删除仍可工作的旧 SDK。解压后执行路径重定位：

```bash
cd <新SDK目录>
./relocate-sdk.sh
```

验证新 SDK，而不是旧目录：

```bash
<新SDK目录>/bin/pkg-config --modversion libmosquitto
<新SDK目录>/bin/pkg-config --cflags --libs libmosquitto

find <新SDK目录>/aarch64-buildroot-linux-gnu/sysroot/usr \
    \( -name 'mosquitto.h' \
    -o -name 'libmosquitto.so*' \
    -o -name 'libmosquitto.pc' \) -print
```

禁止把宿主机 `/usr/lib/x86_64-linux-gnu/pkgconfig` 加入交叉构建搜索路径。这会让 CMake 找到 x86-64 库，产生架构错误或错误链接。

## 4. 使用新 SDK 构建 edgevision-gateway

用新构建目录，避免旧 CMake 缓存继续引用旧 SDK：

```bash
cd /home/zoukunbo/project/edgevision-gateway

cmake -S . -B build-arm64-mqtt \
    -DCMAKE_TOOLCHAIN_FILE=<新SDK目录>/share/buildroot/toolchainfile.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DEDGEVISION_ENABLE_MQTT=ON \
    -DEDGEVISION_BUILD_MQTT_EXAMPLE=ON

cmake --build build-arm64-mqtt -j
```

检查架构和动态依赖：

```bash
file build-arm64-mqtt/mqtt_reconnect_demo

<新SDK目录>/bin/aarch64-buildroot-linux-gnu-readelf \
    -d build-arm64-mqtt/mqtt_reconnect_demo
```

预期：

- `file` 显示 ARM aarch64，而不是 x86-64；
- 动态依赖出现 `libmosquitto.so.1`；
- `PkgConfig::MOSQUITTO` 来自新 SDK 的 sysroot。

## 5. 部署到目标板

推荐使用重新生成的 Buildroot 根文件系统镜像，使运行库及依赖一起进入板端。

临时冒烟可以复制程序，但仍需保证全部 ARM64 运行库存在：

```bash
scp build-arm64-mqtt/mqtt_reconnect_demo root@<板端IP>:/usr/bin/
```

板端检查：

```bash
file /usr/bin/mqtt_reconnect_demo
ldd /usr/bin/mqtt_reconnect_demo
```

若出现 `libmosquitto.so.1 => not found`，应把 Buildroot
`output/rockchip_ok1126b-s/target` 中的 ARM64 运行库加入根文件系统；不要
复制 Ubuntu 主机 `/usr/lib` 中的库。

```bash
/usr/bin/mqtt_reconnect_demo
```

Broker侧订阅：

```bash
mosquitto_sub -h <Broker-IP> -p 1883 \
    -t 'edgevision/v1/devices/+/measurements' -q 1 -v
```

## 6. 下次遇到 SDK 中没有的库

### 6.1 标准决策顺序

1. 在 `make menuconfig` 中搜索库名；
2. 检查 Buildroot `package/<库名>/` 是否存在；
3. 查清许可证、架构支持、线程、TLS及其他依赖；
4. 优先启用 Buildroot 已有包；
5. 没有现成包时才创建自定义 package；
6. 同时验证 staging、target 和导出 SDK；
7. 最后修改业务项目 CMake。

```bash
make menuconfig
# 界面中按 / 搜索库名

find package -maxdepth 2 -iname '*库名*'
rg 'config BR2_PACKAGE_.*库名' package
```

不要只执行 `apt install <库>-dev`。APT安装的是 x86-64主机库，不能供 ARM64 目标程序链接。

### 6.2 Buildroot 没有该包

长期维护优先使用 `BR2_EXTERNAL` 保存自定义包，避免直接散改 Buildroot 主仓库：

```text
board-support/
├── external.desc
├── Config.in
├── external.mk
└── package/
    └── foo/
        ├── Config.in
        ├── foo.mk
        ├── foo.hash
        └── 0001-fix-cross-build.patch
```

`Config.in` 示例：

```kconfig
config BR2_PACKAGE_FOO
    bool "foo"
    depends on BR2_TOOLCHAIN_HAS_THREADS
    select BR2_PACKAGE_ZLIB
    help
      Short description and upstream URL.
```

CMake库的 `foo.mk` 示例：

```make
FOO_VERSION = 1.2.3
FOO_SITE = https://example.com/releases
FOO_SOURCE = foo-$(FOO_VERSION).tar.gz
FOO_LICENSE = MIT
FOO_LICENSE_FILES = LICENSE
FOO_INSTALL_STAGING = YES
FOO_DEPENDENCIES = zlib

$(eval $(cmake-package))
```

Autotools库通常使用：

```make
$(eval $(autotools-package))
```

没有标准构建系统时使用 `generic-package`，并显式编写 build、staging安装和target安装命令。

`FOO_INSTALL_STAGING = YES` 对开发库非常重要。缺少它时，库可能进入目标根文件系统，却不会进入 SDK，业务项目仍无法交叉编译。

`foo.hash` 应记录源码包和许可证文件校验值，以保证构建可复现并发现下载内容被替换。

## 7. 业务项目的 CMake 接入原则

优先使用库提供的 CMake package；没有时使用 pkg-config：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(
    FOO
    REQUIRED
    IMPORTED_TARGET
    foo
)

target_link_libraries(your_target PRIVATE
    PkgConfig::FOO
)
```

不要硬编码宿主机路径：

```cmake
# 错误示例
include_directories(/usr/include)
link_directories(/usr/lib/x86_64-linux-gnu)
```

Buildroot toolchainfile 会设置 sysroot 和查找根路径。项目应让 `find_package` 或 SDK 的 `pkg-config` 在 sysroot 内查找。

可选能力可以沿用当前工程模式：

```cmake
option(EDGEVISION_ENABLE_FOO "Build foo adapter" OFF)

if(EDGEVISION_ENABLE_FOO)
    # 查找依赖并创建相关目标
endif()
```

这能保持缺少依赖的平台继续构建，但不能代替真正的板端依赖接入。

## 8. 常见错误

### CMake 报 `Package ... not found`

```bash
<SDK>/bin/pkg-config --print-sysroot
<SDK>/bin/pkg-config --variable pc_path pkg-config
<SDK>/bin/pkg-config --modversion <库名>
find <SDK> -name '<库名>.pc' -o -name 'lib<库名>*'
```

若 Buildroot `output/rockchip_ok1126b-s/staging` 能找到，而 SDK 找不到，
说明忘记重新导出SDK，或CMake仍指向旧SDK。

### 链接器报告 `file in wrong format`

通常混入了 x86-64主机库：

```bash
file <可疑库文件>
```

ARM64库应显示 AArch64。清除旧构建目录并用正确 toolchainfile 重新配置。

### 板端报告 `not found`

可能是程序不存在，也可能是 ELF解释器或共享库不存在：

```bash
file /usr/bin/<程序>
readelf -l /usr/bin/<程序> | grep interpreter
ldd /usr/bin/<程序>
```

### 头文件存在但链接失败

检查 `.pc` 的 `Libs`、静态链接传递依赖、库构建选项和链接顺序。静态链接通常更容易暴露遗漏依赖。

## 9. 新库接入验收清单

- [ ] Buildroot defconfig 已保存，而不只是修改 `.config`
- [ ] 软件包版本和许可证明确
- [ ] `output/staging` 有头文件、库和发现元数据
- [ ] `output/target` 有板端运行文件
- [ ] 已重新执行 `make sdk`
- [ ] 新目录解压并重定位 SDK
- [ ] SDK的 pkg-config 能查到目标库
- [ ] CMake使用 Buildroot toolchainfile
- [ ] `file` 确认产物为 AArch64
- [ ] `readelf`/`ldd` 确认依赖完整
- [ ] 根文件系统或部署包包含全部运行库
- [ ] 主机测试、ARM64构建和板端冒烟全部通过

完成这条链路后，才能说一个第三方库真正“加入项目并可在目标板使用”。
