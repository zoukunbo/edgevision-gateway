# D34 Buildroot MQTT / SDK 验证记录

## 结论

- Mosquitto 2.0.18 已成功加入 `rockchip_ok1126b-s` 的 Buildroot 目标，并已安装到目标根文件系统和交叉编译 sysroot。
- `libmosquitto.so.1` 已确认是 AArch64 ELF 动态库；目标系统运行时依赖包括 `libssl.so.3`、`libcrypto.so.3`、`libc.so.6` 和 `ld-linux-aarch64.so.1`。
- QtWebEngine 的大型增量编译已经完成。
- `make ... sdk` 最终在厂商应用 `flapp` 阶段失败，因此新的“可搬移 SDK 压缩包”未生成；但 Buildroot 输出目录中的工具链和 sysroot 完整可用，不影响本机交叉编译。
- 该失败与 Mosquitto 无关。首个根因是绕过厂商 `build.sh` 直接运行 Buildroot `make sdk` 后，`flapp/build.sh` 需要的 `RK_SDK_DIR` 和 `RK_BUILDROOT_CFG` 没有设置，最终把 qmake 路径展开为 `/buildroot/output//host/bin/qmake`。
- 已使用现有 Buildroot 工具链以 `EDGEVISION_ENABLE_MQTT=ON` 成功交叉编译项目；产物确认为 AArch64 ELF，并动态依赖 `libmosquitto.so.1`。
- 已在真实 OK1126B 开发板完成 QoS 1 发布验证，板端收到 PUBACK，电脑订阅端收到 topic 和完整 JSON。
- 当前主机测试清单为 18 项，复测结果为 `18/18` 全部通过。

验证时间：2026-08-26 至 2026-08-28（Asia/Shanghai）。

## 目标与路径

```text
BSP:        /home/zoukunbo/work/OK1126B-linux-source
Buildroot:  /home/zoukunbo/work/OK1126B-linux-source/buildroot
输出目录:   /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s
项目:       /home/zoukunbo/project/edgevision-gateway
架构:       AArch64
```

厂商 SDK 没有创建常见的 `output/.../staging` 符号链接。实际 sysroot 是：

```text
/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/host/aarch64-buildroot-linux-gnu/sysroot
```

## 当前环境可用性清单

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| `output/rockchip_ok1126b-s` | 可用 | 正确的 AArch64 Buildroot 输出，必须保留 |
| Buildroot 内置交叉编译器 | 可用 | GCC 12.4.0，已成功编译项目 |
| Buildroot 内置 sysroot | 可用 | 包含 Mosquitto 2.0.18、OpenSSL 等目标库 |
| `mosquitto.h` / `libmosquitto.so.1` / `.pc` | 可用 | target 与 sysroot 中均存在 |
| `mosquitto_pub` / `mosquitto_sub` | 可用 | 已进入新 target，但旧板端根文件系统尚未包含 |
| `/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot` | 部分可用 | 旧的独立 SDK，编译器可用，但其中没有 Mosquitto |
| 新 SDK 导出压缩包 | 不可用 | `flapp` 阶段失败，尚未生成 |
| 开发板 `192.168.0.232` | 可用 | root SSH 密钥登录成功，AArch64 OK1126B |
| 板端永久 Mosquitto 安装 | 不可用 | 当前板上 `/usr/lib` 尚无 `libmosquitto.so.1` |
| 板端临时 MQTT 运行 | 可用 | 从 `/tmp` 加载库后已完成真实发布验证 |

## 已完成的 Mosquitto 验证

配置检查结果：

```text
BR2_aarch64=y
BR2_ARCH="aarch64"
BR2_PACKAGE_MOSQUITTO=y
Mosquitto broker 未启用，仅使用客户端库和工具。
```

单包构建命令：

```sh
cd /home/zoukunbo/work/OK1126B-linux-source/buildroot
make O=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s mosquitto
```

结果：Mosquitto 2.0.18 构建和安装成功，`.stamp_installed` 存在。

检查命令：

```sh
find /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/target/usr \
  \( -name mosquitto.h -o -name 'libmosquitto.so*' -o -name libmosquitto.pc \) -print

file /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/target/usr/lib/libmosquitto.so.1

readelf -d /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/target/usr/lib/libmosquitto.so.1
```

已确认目标文件：

```text
target/usr/include/mosquitto.h
target/usr/lib/libmosquitto.so
target/usr/lib/libmosquitto.so.1
target/usr/lib/pkgconfig/libmosquitto.pc
```

`file` 结果为 AArch64 ELF64。动态依赖：

```text
libssl.so.3
libcrypto.so.3
libc.so.6
ld-linux-aarch64.so.1
```

## 构建过程及异常记录

### 1. 错误输出目录事件

早期曾检查 `output/ok1126b`，该目录实际为 i386 配置，不是本板卡的有效输出。后续统一使用：

```text
output/rockchip_ok1126b-s
```

未删除错误目录，也没有破坏性清理。

### 2. PATH 含非法字符

首次单包构建曾出现：

```text
Your PATH contains spaces, TABs, and/or newline characters.
```

恢复方式是使用不含 Windows 空格路径的 Linux PATH 后重新执行单包构建。Mosquitto 随后成功。

### 3. Qt WebChannel 并行安装竞争

首次执行：

```sh
cd /home/zoukunbo/work/OK1126B-linux-source
./build.sh buildroot-sdk
```

Qt WebChannel 的 `qwclient` 示例在并行安装时，`install_exampleassets` 与 `install_sources` 同时写入 README，导致目标安装失败。

一次恢复命令误写为：

```sh
make -C buildroot \
  O=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s \
  PARALLEL_JOBS=-j1 sdk
```

它被展开为 `make -j-j1` 并立即退出，没有修改构建产物。正确语法是：

```sh
make -C buildroot \
  O=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s \
  PARALLEL_JOBS=1 sdk
```

串行恢复成功完成 Qt WebChannel 目标安装，相关 stamp 文件生成。

### 4. QtWebEngine 编译加速与内存限制

QtWebEngine 包含 Chromium/V8/PDFium，远大于 Mosquitto，是耗时的主要来源。

尝试四并发：

```sh
export PATH=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/host/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ninja -j4 \
  -C /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/build/qt5webengine-5.15.11/src/core/release \
  QtWebEngineCore
```

结果：主机约 7.7 GiB 内存不足，`cc1plus` 被系统终止：

```text
aarch64-buildroot-linux-gnu-g++.br_real: fatal error: Killed signal terminated program cc1plus
ninja: build stopped: subcommand failed.
```

没有清理中间产物。确认旧进程退出后，安全降为二并发增量恢复：

```sh
ninja -j2 \
  -C /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/build/qt5webengine-5.15.11/src/core/release \
  QtWebEngineCore
```

结果：`3054/3054` 成功，`QtWebEngineCore.stamp` 生成。

### 5. SDK 恢复后的首个根因

QtWebEngine 增量目标完成后执行：

```sh
cd /home/zoukunbo/work/OK1126B-linux-source
make -C buildroot \
  O=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s \
  PARALLEL_JOBS=2 sdk
```

QtWebEngine 继续完成 PDFium 和安装，随后进入厂商 `flapp` 包。失败证据：

```text
>>> flapp 0.0.1 Building
cd .../app/forlinx/flapp; ./build.sh
./build.sh: line 79: /buildroot/output//host/bin/qmake: No such file or directory
build qt demo fail

>>> flapp 0.0.1 Installing to target
cp: cannot stat '.../flapp_out/release/bin/*': No such file or directory
make: *** [package/pkg-generic.mk:441: .../flapp-0.0.1/.stamp_target_installed] Error 1
```

`flapp/build.sh` 中的路径来自：

```sh
build_qmake=$RK_SDK_DIR/buildroot/output/$RK_BUILDROOT_CFG/host/bin/qmake
```

直接运行 Buildroot `make sdk` 时两个厂商变量为空，路径因此错误。后续 `cp` 失败只是前面 qmake 未执行产生的连锁错误，不是首个根因。

## 安全恢复步骤

不要执行 `make clean`、删除 `output/rockchip_ok1126b-s` 或重新全量编译。当前 QtWebEngine 和 Mosquitto 成果都可以复用。

推荐从 BSP 根目录重新使用厂商入口恢复，它会提供 `flapp` 需要的板卡环境：

```sh
cd /home/zoukunbo/work/OK1126B-linux-source
./build.sh buildroot-sdk
```

执行前应先确认没有残留构建进程：

```sh
ps -ef | grep -E '[b]uild\.sh buildroot-sdk|[m]ake .*rockchip_ok1126b-s|[n]inja'
```

如果厂商入口仍未传递变量，先只读确认当前板卡值，再使用同一输出目录恢复：

```sh
cd /home/zoukunbo/work/OK1126B-linux-source
export RK_SDK_DIR=/home/zoukunbo/work/OK1126B-linux-source
export RK_BUILDROOT_CFG=rockchip_ok1126b-s
make -C buildroot \
  O=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s \
  PARALLEL_JOBS=2 sdk
```

上述第二种方式应在确认 `RK_BUILDROOT_CFG` 与厂商脚本实际使用值一致后再执行。

## SDK 失败后的交叉编译验证

完整 SDK 导出不是本机交叉编译的前置条件。直接使用正确 Buildroot 输出中的工具链文件：

```sh
cd /home/zoukunbo/project/edgevision-gateway
cmake -S . -B build-arm64-mqtt-buildroot \
  -DCMAKE_TOOLCHAIN_FILE=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/host/share/buildroot/toolchainfile.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DEDGEVISION_ENABLE_MQTT=ON \
  -DEDGEVISION_BUILD_MQTT_EXAMPLE=ON
cmake --build build-arm64-mqtt-buildroot -j2
```

配置阶段成功识别：

```text
C compiler: aarch64-buildroot-linux-gnu-gcc 12.4.0
Found libmosquitto, version 2.0.18
```

`mqtt_publish_once`、`mqtt_reconnect_demo` 和 `gateway` 均构建成功。`file` 结果均为：

```text
ELF 64-bit LSB pie executable, ARM aarch64
interpreter /lib/ld-linux-aarch64.so.1
```

`mqtt_publish_once` 的直接动态依赖：

```text
libm.so.6
libmosquitto.so.1
libc.so.6
ld-linux-aarch64.so.1
```

## 主机测试结果

```sh
cd /home/zoukunbo/project/edgevision-gateway
cmake --build build-mqtt -j2
ctest --test-dir build-mqtt --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 18
Total Test time (real) = 7.38 sec
```

第 18 项为 `mqtt_publisher`，已通过。

## 板端验证

开发板探测结果：

```text
地址: 192.168.0.232
主机名: OK1126B-buildroot
内核: Linux 6.1.141
架构: aarch64
系统: Buildroot 2024.02
登录: root SSH 密钥可用
```

旧板端根文件系统没有 `/usr/lib/libmosquitto.so.1`，但已存在兼容的 OpenSSL 3、glibc 和 AArch64 动态加载器。为避免修改板端永久系统，测试文件仅复制到：

```text
/tmp/edgevision-mqtt/mqtt_publish_once
/tmp/edgevision-mqtt/lib/libmosquitto.so.1
```

板端 `ldd` 成功解析：

```text
libmosquitto.so.1 => /tmp/edgevision-mqtt/lib/libmosquitto.so.1
libssl.so.3       => /lib/libssl.so.3
libcrypto.so.3    => /lib/libcrypto.so.3
libc.so.6         => /lib/libc.so.6
```

因为示例固定连接 `127.0.0.1:1883`，通过 SSH `-R` 把板端回环端口转发到电脑现有 broker。板端结果：

```text
PUBLISH_CONFIRMED mid=1 business_id=(sim-temperature-01,1)
payload={"device_id":"sim-temperature-01","metric":"temperature","unit":"celsius","schema_version":1,"sequence":1,"timestamp_ms":1787623200000,"value":26.5,"quality":"good"}
```

电脑订阅端实际收到：

```text
edgevision/v1/devices/sim-temperature-01/measurements {"device_id":"sim-temperature-01","metric":"temperature","unit":"celsius","schema_version":1,"sequence":1,"timestamp_ms":1787623200000,"value":26.5,"quality":"good"}
```

结论：当前 Buildroot 生成的 `libmosquitto` 与项目程序可在真实 OK1126B 上运行，MQTT QoS 1 发布链路通过。

## 安全清理记录

已删除两个经过核实且可重建的无用目录：

```text
/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/ok1126b
  原因：误建的 i586 输出，不属于 OK1126B，约 6.2 GB。

/home/zoukunbo/project/edgevision-gateway/build-arm64-mqtt
  原因：指向不含 Mosquitto 的旧 SDK，仅有 CMake 配置缓存，无应用产物，约 156 KB。
```

清理后磁盘可用空间约从 877 GiB 增加到 883 GiB。正确的 AArch64 输出、源码改动和其他测试目录均保留。

## 下次添加交叉编译库的最短流程

通常**不需要每次都生成完整 SDK**。先区分目标：

### 只在当前电脑交叉编译应用

1. 在正确 defconfig 中启用库。
2. 只构建该包：

   ```sh
   cd /home/zoukunbo/work/OK1126B-linux-source/buildroot
   make O=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s <package-name>
   ```

3. 检查头文件、库和 `.pc` 是否进入 `host/.../sysroot`。
4. 项目直接使用 `host/share/buildroot/toolchainfile.cmake` 交叉编译。

这样只编译新增包及其缺失依赖，不会重编 QtWebEngine，也不需要导出 SDK。

### 要让库永久进入开发板镜像

单包成功后还需要重新生成目标根文件系统/固件镜像，再烧写或升级开发板。可以使用厂商针对 Buildroot/rootfs 的构建入口；这不是“生成 SDK”，而是“更新板端镜像”。如果只做临时验证，也可像本次一样把应用和 `.so` 放到 `/tmp` 或 `/opt`，并设置 `LD_LIBRARY_PATH`。

### 要把工具链交给另一台电脑或 CI

只有需要可搬移、可分发的独立工具链时才执行：

```sh
./build.sh buildroot-sdk
```

它可能触发整个 SDK 依赖闭包和厂商包安装，因此耗时远大于单包构建。本次 Mosquitto 使用并不依赖它成功。

## 板端部署注意事项

仅成功构建 Mosquitto 单包并不足以证明应用一定能在板端启动。实际部署至少应保证：

- 应用程序由相同 AArch64 Buildroot 工具链编译；
- 板端存在匹配版本的 `libmosquitto.so.1`；
- 板端同时存在 OpenSSL 3 和动态加载器等依赖；
- 应用的 ELF interpreter、RPATH/RUNPATH 和 ABI 与目标根文件系统一致；
- 最终通过板端 `ldd`（若系统提供）、启动日志和真实 broker 发布确认测试。

本轮已完成临时目录方式的板端验证；尚未执行的是把 Mosquitto 永久写入 rootfs、重新生成并烧写固件后的开机验证。
