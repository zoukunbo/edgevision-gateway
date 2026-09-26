# D29 OK1126B-S 板端验证证据

验证日期：2026-08-20

## 环境基线

```text
board=Forlinx OK1126B-S / Rockchip RV1126B
architecture=aarch64
system=Buildroot
adb_device=192.168.0.232:5555
reachable_ip=192.168.0.232
sdk=/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot
toolchain=Buildroot 2024.02 / GCC 12.4.0
cmake_toolchain=share/buildroot/toolchainfile.cmake
target_zlib=1.3.1
```

`/home/zoukunbo/work/OK1126B-linux-source/kernel-6.1` 是 Linux 内核源码与内核产物目录，用于内核、设备树和模块构建，不是用户态 Buildroot SDK。

`/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot` 是 Buildroot SDK，包含交叉编译器、官方 CMake toolchainfile 和目标 sysroot。SDK 根目录的 `lib/` 是 x86-64 主机工具依赖；ARM64 目标库位于 `aarch64-buildroot-linux-gnu/sysroot/usr/lib/`。

## 板端压力测试

ARM64 `net_stress_test` 通过 ADB 推送到板端 `/tmp` 运行：

```text
connections=100
payload_bytes=1048576
total_bytes=104857600
recv_calls=13066
short_reads=12966
verification=PASS
open_fds_before=4
open_fds_after=4
```

结论：100 次连接、100 MiB 数据传输、TCP 短读处理、内容校验和 FD 回收均通过。

## 主机到板端 TCP

ARM64 TCP 服务端在板端监听 `0.0.0.0:18080`，WSL 客户端连接 `192.168.0.232:18080`：

```text
connected to 192.168.0.232:18080
echo: hello
```

结论：WSL 主机到 OK1126B-S 的真实 TCP 建连、发送和回显通过。

## Gateway 交叉构建

使用 SDK 官方工具链文件：

```bash
cmake \
    -S . \
    -B /tmp/edgevision-build-ok1126b \
    -DCMAKE_TOOLCHAIN_FILE=/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot/share/buildroot/toolchainfile.cmake \
    -DCMAKE_BUILD_TYPE=Release

cmake --build /tmp/edgevision-build-ok1126b --target gateway -j
```

CMake 找到目标 zlib：

```text
Found ZLIB: .../aarch64-buildroot-linux-gnu/sysroot/usr/lib/libz.so
found version 1.3.1
```

Gateway 产物为 ARM64 ELF，动态依赖为 `libz.so.1`、`libc.so.6` 和 `ld-linux-aarch64.so.1`，均与板端环境匹配。

## Gateway smoke

```text
SMOKE_PASS
gateway started
gateway smoke passed: loopback TCP request/response
```

结论：Gateway 的网络封装、TCP 双向请求/响应和异步日志在板端通过。

## SIGTERM 优雅退出

板端使用 `timeout --preserve-status -s TERM 2` 向普通模式 Gateway 发送 SIGTERM：

```text
gateway running; send SIGINT or SIGTERM to stop
gateway stopped cleanly
gateway started
shutdown requested; draining logger
```

结论：板端 SIGTERM 被正确捕获，Gateway 正常退出并完成异步日志排空。

## 当前验收状态

| 项目 | 状态 |
|---|---|
| OK1126B-S 板端 100 连接/100 MiB 压力测试 | 通过 |
| 板端数据一致性与 FD 泄漏检查 | 通过 |
| WSL 主机到板端 TCP echo | 通过 |
| ARM64 Gateway 交叉构建 | 通过 |
| 板端 `gateway --smoke` | 通过 |
| 板端 SIGTERM 优雅退出与日志排空 | 通过 |
