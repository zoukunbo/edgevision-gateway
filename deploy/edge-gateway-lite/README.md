# Edge Gateway Lite v0.1 部署说明

本目录用于把交叉编译完成的 `gateway` 及其运行依赖组装成发布包，并将其安装为
目标板上的常驻 systemd 服务。旧的 `deploy/edgevision-outbox/` oneshot 教学部署
继续保留；两套部署使用不同入口和数据库，不会互相覆盖。

完整流程如下：

```text
构建主机上的 ARM64 gateway + Buildroot sysroot
                    │
                    ▼ prepare-bundle.sh
              可传输的发布包
                    │ NFS、U 盘或其他方式
                    ▼ install-board.sh（目标板 root）
    /userdata 中的程序副本 + /etc 中的配置 + systemd 服务
                    │
                    ▼ health-check.sh
             服务、设备和数据库检查
```

## 目录中的文件

| 文件 | 执行位置 | 用途 |
| --- | --- | --- |
| `prepare-bundle.sh` | 构建主机 | 收集二进制、目标架构共享库和部署文件，生成 `SHA256SUMS` |
| `install-board.sh` | 目标板 | 校验发布包，复制到持久化目录，安装并启动 systemd 服务 |
| `config/edge-gateway-lite.env` | 模板；目标板使用其副本 | 定义运行目录、状态文件和数据源 |
| `scripts/run-gateway.sh` | 目标板 | 加载配置并以前台主进程方式启动 Gateway |
| `scripts/health-check.sh` | 目标板 | 只读检查服务、版本、设备节点和数据库 |
| `scripts/service-activate.sh` | 目标板 | 重载 unit、设置开机启动并显式重启服务 |
| `systemd/edge-gateway-lite.service` | 模板；安装到 `/etc/systemd/system` | 定义常驻服务、重启策略和安全限制 |

## 发布包结构

```text
edge-gateway-lite/
├── bin/gateway
├── bin/gatewayctl
├── lib/libmosquitto.so.1
├── config/edge-gateway-lite.env
├── scripts/run-gateway.sh
├── scripts/health-check.sh
├── scripts/service-activate.sh
├── systemd/edge-gateway-lite.service
├── install-board.sh
├── README.md
└── SHA256SUMS
```

安装后，程序和共享库位于 `/userdata/edgevision-gateway/edge-gateway-lite`；数据库
和日志位于其上一级 `/userdata/edgevision-gateway`。程序与运行状态分开，因此升级
二进制不会覆盖数据库或日志。

正式 v0.1 默认使用 `edge-gateway-lite-v0.1.db`，不会自动打开历史 oneshot 的
未版本化 `gateway.db`。历史数据迁移必须通过单独、可审计的迁移步骤完成，不应
简单覆盖或改名旧数据库。

## 前置条件

### 构建主机

- 已完成目标板架构（当前为 ARM64）的 Gateway 构建。
- 构建输出目录中存在可执行的 `gateway` 和 `gatewayctl`。
- Buildroot sysroot 中存在 `usr/lib/libmosquitto.so.1`。
- 系统提供 POSIX `sh`、`install`、`find`、`xargs`、`sha256sum`、`readlink` 和 `file`。

### 目标板

- 使用 systemd，且能以 root 身份执行安装脚本。
- 提供 `sha256sum` 和 `sqlite3`；缺少 `sqlite3` 时安装可以复制文件，但最终健康
  检查会失败。
- 使用真实 STM32 数据源时，默认应存在 `/dev/ttyS5` 和 `/dev/gpiochip0`。
- 本机或网络中的 MQTT broker 应与 Gateway 当前内置配置相匹配。到达
  `network-online.target` 只表示网络初始化完成，不保证 broker 已可用。

## 1. 组装发布包

先完成 ARM64、Storage+MQTT 构建，再显式传入三个参数：

```text
prepare-bundle.sh BUILD_DIR SYSROOT OUTPUT_DIR
                  │         │       └─ 新发布包的输出目录
                  │         └───────── Buildroot 目标 sysroot
                  └─────────────────── 含 gateway 的构建输出目录
```

示例：

```sh
deploy/edge-gateway-lite/prepare-bundle.sh \
  build-arm64-d40-stm32-gateway-20260907 \
  /path/to/buildroot/sysroot \
  deploy/nfs-root/edge-gateway-lite
```

脚本会执行以下操作：

1. 确认 `BUILD_DIR/gateway` 和 `BUILD_DIR/gatewayctl` 可执行，并确认 sysroot 中存在目标架构的
   `libmosquitto.so.1`。
2. 拒绝非空的 `OUTPUT_DIR`，避免把旧版本和新版本混装。
3. 创建发布目录并复制二进制、共享库、配置模板、脚本、unit 和本文档。
4. 为发布文件生成 `SHA256SUMS`。
5. 用 `file` 输出 Gateway 的 ELF 信息，供操作者确认架构。

正常结束时应看到类似输出：

```text
... ELF 64-bit LSB pie executable, ARM aarch64 ...
bundle ready: deploy/nfs-root/edge-gateway-lite
```

请确认 ELF 架构与目标板一致。脚本不会在构建主机运行交叉编译产物，因为 x86
主机通常无法直接执行 ARM64 程序。发布包由 `.gitignore` 排除，源码仓库只保存
可复现的模板和脚本。

若要重新组包，请使用一个新的空目录，或在确认旧包已不再需要后自行清空原目录；
脚本不会自动删除任何旧文件。

## 2. 安装到目标板

通过 NFS、U 盘或其他方式，让目标板可以读取整个发布包。默认源路径是
`/mnt/edgevision/edge-gateway-lite`，然后以 root 执行：

```sh
sudo /mnt/edgevision/edge-gateway-lite/install-board.sh
```

发布包不在默认位置时，通过环境变量指定源目录：

```sh
sudo EDGEVISION_BUNDLE=/media/usb/edge-gateway-lite \
  /media/usb/edge-gateway-lite/install-board.sh
```

安装脚本按以下顺序工作：

1. 检查 root 权限和关键文件。
2. 在修改系统前执行 `sha256sum -c SHA256SUMS`，拒绝损坏或混装的发布包。
3. 将文件复制到 `/userdata/edgevision-gateway/edge-gateway-lite`。传输介质随后可以
   卸载，服务启动不再依赖 `/mnt` 或 U 盘。
4. 首次安装时创建 `/etc/edgevision-gateway/edge-gateway-lite.env`。
5. 安装 systemd unit，执行 `daemon-reload`、`enable` 和显式 `restart`。
6. 等待三秒，检查 systemd 状态并运行应用级健康检查。

已有的 `/etc/edgevision-gateway/edge-gateway-lite.env` 不会被模板覆盖，以免升级时
丢失现场配置。新模板保存在
`/userdata/edgevision-gateway/edge-gateway-lite/config/edge-gateway-lite.env`，升级后
可以人工比较两者。

安装成功后，运行时不再使用“安装命令中临时指定的 `EDGEVISION_BUNDLE`”；服务读取
的是 `/etc/edgevision-gateway/edge-gateway-lite.env` 中的持久化路径。

systemd 启动服务时会创建 `/run/edgevision-gateway`，并把本地命令 socket
固定为 `/run/edgevision-gateway/control.sock`。这使得开启 `PrivateTmp=true` 后，
板端 `gatewayctl` 仍能连接到 systemd 服务。

## 3. 配置说明

板端实际配置文件：

```text
/etc/edgevision-gateway/edge-gateway-lite.env
```

该文件使用 systemd `EnvironmentFile`/POSIX shell 都能读取的简单 `KEY=value` 格式。
等号两侧不要添加空格；路径当前不应包含空格或 shell 特殊字符。修改后需重启服务：

```sh
sudo systemctl restart edge-gateway-lite.service
```

| 配置项 | 默认值 | 作用 |
| --- | --- | --- |
| `EDGEVISION_BUNDLE` | `/userdata/edgevision-gateway/edge-gateway-lite` | 已安装程序和共享库的位置 |
| `EDGEVISION_STATE_DIR` | `/userdata/edgevision-gateway` | 数据库、日志等可写状态的基础目录 |
| `EDGEVISION_DATABASE` | `/userdata/edgevision-gateway/edge-gateway-lite-v0.1.db` | SQLite 数据库文件 |
| `EDGEVISION_LOG` | `/userdata/edgevision-gateway/gateway.log` | Gateway 日志文件参数 |
| `EDGEVISION_SOURCE` | `stm32` | 数据源，只允许 `stm32` 或 `simulated` |
| `EDGEVISION_SERIAL` | `/dev/ttyS5` | 健康检查期望的串口字符设备 |
| `EDGEVISION_GPIOCHIP` | `/dev/gpiochip0` | 健康检查期望的 GPIO 字符设备 |

注意：v0.1 的 Gateway CLI 只接收 source、database 和 log path。`EDGEVISION_SERIAL`
与 `EDGEVISION_GPIOCHIP` 当前只改变健康检查目标，尚不能改变 Gateway 内部使用的
设备路径。修改它们不会重新配置 Gateway 本身。

需要在没有 STM32 硬件的环境中验证基础流程时，可将配置改为：

```sh
EDGEVISION_SOURCE=simulated
```

此时健康检查会跳过串口和 GPIO 设备节点检查。

## 4. 日常运维

```sh
# 查看当前状态和最近的错误
systemctl status edge-gateway-lite.service

# 配置变更或故障处理后重启
systemctl restart edge-gateway-lite.service

# 停止服务
systemctl stop edge-gateway-lite.service

# 检查已安装的持久化副本（不要依赖仍挂载的安装源）
/userdata/edgevision-gateway/edge-gateway-lite/scripts/health-check.sh

# 查看本次启动以来的日志
journalctl -u edge-gateway-lite.service -b --no-pager

# 持续跟踪日志，按 Ctrl-C 退出
journalctl -u edge-gateway-lite.service -f

# 查看所有历史日志
journalctl -u edge-gateway-lite.service --no-pager
```

健康检查只读取状态，不会启动、停止或修改 Gateway。成功输出类似：

```text
OK service active
OK candidate version=0.1.0
OK running version matches candidate: 0.1.0
OK serial=/dev/ttyS5
OK gpiochip=/dev/gpiochip0
OK database integrity=ok
OK measurements|pending|sent=120|0|120
OK edge-gateway-lite healthy
```

统计行三个数字依次表示：`measurements` 总记录数、MQTT 发件箱中 `pending` 数、
发件箱中 `sent` 数。任一检查失败时会输出 `FAIL ...` 并最终返回退出码 1，适合接入
部署脚本或监控系统。
候选程序版本与正在运行的进程版本必须都非空且完全相同；连接不到 socket、
客户端返回非零、空回复或非 `ok version=...` 格式都会使健康检查失败。

## 5. 升级与回滚

升级步骤与首次安装相同：在构建主机生成一个全新的发布包，将其传到目标板，再运行
新包中的 `install-board.sh`。升级会替换 `/userdata/.../edge-gateway-lite` 下的程序、
脚本和模板，但不会覆盖 `/etc` 中的现场配置、数据库或日志。

当前安装流程不是整包事务切换，安装过程中不要并行执行另一个安装，也不要主动断电。
安装或健康检查返回失败，只表示本次部署不能提交为成功；当前脚本不会自动
恢复旧程序或数据库。操作者必须保留旧发布包和升级前备份，再按明确的回滚流程处理。
正式升级前建议备份以下内容：

```sh
/etc/edgevision-gateway/edge-gateway-lite.env
/userdata/edgevision-gateway/edge-gateway-lite-v0.1.db
```

如需回滚，使用之前保存的完整旧发布包重新运行其 `install-board.sh`。若新旧版本的数据
库 schema 不兼容，还必须按对应版本的迁移/回滚方案处理数据库，不能只替换二进制。

## 6. 常见故障排查

### `output directory is not empty`

`prepare-bundle.sh` 为避免版本混装，只接受不存在或为空的输出目录。换用新目录，或先
人工确认并处理旧目录内容。

### `libmosquitto.so.1 not found in sysroot`

确认第二个参数是目标板 Buildroot 的 sysroot，而不是主机根目录；同时确认目标固件已
启用 Mosquitto 客户端库。不要复制 x86 主机上的库到 ARM64 发布包。

### `sha256sum: WARNING ... did NOT match`

发布包在传输后损坏、缺失或被修改。不要绕过校验继续安装；重新传输完整发布包，并
检查存储介质或 NFS 是否正常。

### 服务为 `inactive` 或反复重启

依次查看：

```sh
systemctl status edge-gateway-lite.service --no-pager --full
journalctl -u edge-gateway-lite.service -b --no-pager
```

重点检查配置路径、Gateway 架构、共享库、串口/GPIO 权限和数据库错误。

### `FAIL serial=...` 或 `FAIL gpiochip=...`

先确认配置中的设备路径，再确认设备节点确实存在且为字符设备：

```sh
ls -l /dev/ttyS5 /dev/gpiochip0
```

若当前确实没有 STM32 硬件，应明确使用 `EDGEVISION_SOURCE=simulated`，而不是创建同名
普通文件来绕过检查。

### `FAIL sqlite3 command missing`

健康检查依赖目标板上的 `sqlite3` 命令执行完整性和统计查询。将 sqlite3 CLI 加入目标
固件或板端运行环境后重新检查。

### `database stats unavailable`

数据库可能尚未初始化、schema 与 v0.1 不匹配，或表已损坏。先查看服务日志，再使用
只读方式检查 `PRAGMA user_version;` 和 `.schema`；不要直接用空数据库覆盖现场文件。

## 7. 当前版本边界

- 默认使用 STM32 Modbus 源、板端 UART5、GPIO22 和本机 MQTT 1883。
- systemd 以 root 运行以访问当前板端受限的 GPIO 字符设备；生产环境应再收紧权限。
- 当前 Gateway CLI 只允许配置 source、database 和 log path；串口/GPIO/MQTT 参数仍采用程序默认值。
- v0.1 是 at-least-once；消费端必须按持久化 `message_id` 幂等。
