# D50 systemd 命令通道与升级健康检查设计

日期：2026-09-25

## 背景

Edge Gateway Lite 的本地命令客户端 `gatewayctl` 与 Gateway command worker 当前都使用
`/tmp/edgevision-study.sock`。systemd unit 同时启用了 `PrivateTmp=true`，因此服务内创建
的 socket 与服务外运行的安装、健康检查进程不处于同一个 `/tmp` 视图。直接在
`health-check.sh` 中调用 `gatewayctl get_version` 会连接失败。

当前安装脚本还使用 `systemctl enable --now`。服务已经 active 时，该命令不会重新启动
旧进程；复制新二进制后可能形成“磁盘是新版本、内存仍运行旧版本”的混合状态。

## 目标

- systemd 服务与板端维护命令共享一个明确、受 systemd 管理的 Unix socket。
- 保留普通用户本地运行时的 `/tmp` 默认路径，不破坏已有学习与测试流程。
- 升级安装必须显式重启服务。
- 健康检查只有在运行进程版本与磁盘候选版本完全一致时才通过。
- 探测失败时继续收集硬件和数据库诊断，最后统一返回失败。

## 非目标

- 本次不实现程序、动态库、unit 和数据库的自动回滚。
- 本次不修改 MQTT 远程命令协议或 request_id 去重语义。
- 本次不调整 Gateway 的运行用户、GPIO/UART 权限模型或 socket 的非 root 访问策略。
- 本次不修改数据库 schema。

## 命令 socket 设计

Gateway 和 `gatewayctl` 都从环境变量 `EDGEVISION_COMMAND_SOCKET` 读取 socket 路径。
变量未设置或为空时，继续使用 `/tmp/edgevision-study.sock`。该回退保证开发机前台运行、
现有测试以及独立示例无需 root 权限，也不要求预先创建 `/run` 目录。

systemd unit 增加：

```ini
RuntimeDirectory=edgevision-gateway
RuntimeDirectoryMode=0755
Environment=EDGEVISION_COMMAND_SOCKET=/run/edgevision-gateway/control.sock
```

`RuntimeDirectory` 在启动服务前创建目录，并在服务停止后管理其生命周期。
`PrivateTmp=true` 保持不变。当前 unit 使用 `User=root`，安装和健康检查也由 root 执行，
因此本次不新增 group 或 ACL 规则。

socket 环境变量名称、默认路径和取值规则放在一个共享的小型头文件中，由 command worker
与 `gatewayctl` 共用，避免两个 C 文件再次维护不同常量。`gateway_local` 是独立学习示例，
继续使用自己的 `/tmp` 路径，不纳入部署命令通道。

## 安装与启动顺序

`prepare-bundle.sh` 继续对 `gateway` 与 `gatewayctl` 做前置检查，将二者复制进发布包，
并由现有 SHA256 清单覆盖。

`install-board.sh` 在校验发布包并复制文件、安装 unit、执行 `daemon-reload` 后：

1. 执行 `systemctl enable edge-gateway-lite.service`，只建立开机启动关系；
2. 执行 `systemctl restart edge-gateway-lite.service`，首次安装时启动，升级时替换旧进程；
3. restart 失败时打印完整 service status 并返回失败；
4. 短暂等待后运行健康检查。

安装成功不再由 `active` 或磁盘版本单独决定。

## 健康检查数据流

健康检查按以下顺序处理版本：

1. 运行已安装的 `gateway --version`，解析 `edge-gateway-lite X.Y.Z` 得到候选版本；
2. 使用部署 socket 路径运行 `gatewayctl get_version`；
3. 保留 `gatewayctl` 自身退出状态，再从输出最后一行解析 `ok version=X.Y.Z`；
4. 要求运行版本非空并与候选版本完全一致；
5. 版本检查失败只调用 `check_fail`，继续检查设备节点和 SQLite；
6. 所有检查结束后，根据累计的 `failed` 标志统一返回退出码。

以下情况都必须失败：候选程序缺失或执行失败、候选输出格式异常、`gatewayctl` 缺失或
执行失败、空输出、`ok version=` 空版本、回复格式异常、运行版本与候选版本不一致。

## 测试策略

先用隔离 Shell 测试运行真实 `health-check.sh`，只替换外部的 `systemctl` 与 `sqlite3`：

- 候选 v0.2、运行 v0.1：健康检查返回失败并报告版本不一致；
- 候选 v0.2、运行 v0.2：健康检查通过版本检查；
- `gatewayctl` 成功但无输出：健康检查返回失败；
- `gatewayctl` 返回 `ok version=`：健康检查返回失败。

再增加真实进程级验证：使用临时目录作为 `EDGEVISION_COMMAND_SOCKET`，启动模拟源
Gateway，通过同一环境变量运行 `gatewayctl get_version`，确认两端能够通信并返回构建
版本。该验证不访问硬件、不修改 systemd，也不连接外部网络。

最后运行 Shell 语法检查、差异检查与现有 CTest。若目标板验证尚未执行，结果必须明确
标注为主机侧验证，不能声称 systemd 板端部署已经通过。

## 失败语义与后续边界

健康检查失败时，安装命令返回非零，但本次不自动恢复旧程序或数据库。完整安全回滚仍需
另外设计旧版本保留、SQLite 一致性备份、schema 兼容和回滚后二次健康检查。

本次实现由导师负责 Shell 脚本、测试外壳及跨文件样板；学习者已能说明升级成功条件、
按实际改变决定回滚范围，以及回滚成功不等于服务健康恢复。
