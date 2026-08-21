# EdgeVision Gateway 编译、CMake、CTest 与 Sanitizer 学习笔记

本文总结近期 `net_stress_test.c` 开发过程中涉及的手工编译、跨目录引用、CMake、CTest、CI、ASan 和 UBSan。命令默认在仓库根目录执行：

```text
/home/zoukunbo/project/edgevision-gateway
```

## 1. 从 C 源码到可执行文件

C 程序通常经历四个阶段：

```text
源文件（.c）
    ↓ 预处理：展开 #include 和宏
预处理结果
    ↓ 编译：生成汇编代码
汇编代码
    ↓ 汇编：生成目标文件（.o）
目标文件
    ↓ 链接：组合项目代码和依赖库
可执行文件
```

头文件主要提供类型、宏和函数声明，源文件提供函数实现。例如：

```text
modules/net/socket.h  → 声明 net_tcp_socket()
modules/net/socket.c  → 实现 net_tcp_socket()
```

只有头文件而没有实现，通常会在链接阶段出现 `undefined reference`。

## 2. 跨目录手工编译 net_stress_test

完整命令：

```bash
cc \
    -std=c11 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -pthread \
    -Imodules/net \
    tests/net_stress_test.c \
    modules/net/address.c \
    modules/net/socket.c \
    -o /tmp/net_stress_test
```

运行时增加 60 秒限制，避免网络错误造成永久阻塞：

```bash
timeout 60s /tmp/net_stress_test
```

### 2.1 `-Imodules/net` 的作用

测试文件包含：

```c
#include "address.h"
#include "socket.h"
```

头文件实际位于 `modules/net/`，所以使用 `-I` 添加头文件搜索目录。`-I` 只帮助编译器找到声明，不会自动编译 `.c` 实现。

### 2.2 为什么还要列出 address.c 和 socket.c

`tests/net_stress_test.c` 调用了网络模块中的函数，因此手工编译时还要提供：

```text
modules/net/address.c
modules/net/socket.c
```

编译器先分别产生目标文件，链接器再把它们组合成一个可执行文件。

### 2.3 常用参数

| 参数 | 含义 |
|---|---|
| `-std=c11` | 使用 ISO C11 |
| `-Wall -Wextra -Wpedantic` | 开启严格警告 |
| `-Werror` | 把警告当作错误 |
| `-pthread` | 启用 POSIX 线程的编译和链接支持 |
| `-I<目录>` | 添加头文件搜索目录 |
| `-o <文件>` | 指定输出的可执行文件 |

把临时程序输出到 `/tmp`，可以避免污染 Git 仓库。

## 3. CMake 解决了什么问题

手工编译需要重复填写源文件、头文件目录和依赖。CMake 用“目标”描述这些关系：

```cmake
add_library(edgevision_net STATIC
    modules/net/address.c
    modules/net/socket.c
)

target_include_directories(edgevision_net PUBLIC
    modules/net
)
```

其中：

- `add_library` 创建静态库目标 `edgevision_net`；
- `PUBLIC` 表示库本身和链接该库的目标都能继承 `modules/net` 头文件目录；
- CMake 目标名是 `edgevision_net`，生成文件通常是 `libedgevision_net.a`。

压力测试目标写法：

```cmake
add_executable(net_stress_test
    tests/net_stress_test.c
)

target_link_libraries(net_stress_test PRIVATE
    edgevision_net
    Threads::Threads
)

edgevision_enable_warnings(net_stress_test)
```

因为 `edgevision_net` 已经包含 `address.c` 和 `socket.c`，测试目标不应重复列出它们。`edgevision_net` 的 `PUBLIC` include 目录也会自动传播给测试目标。

### 3.1 配置与构建

```bash
cmake -S . -B build
cmake --build build -j
```

- `-S .`：源码目录是当前目录；
- `-B build`：构建文件和产物放入 `build/`；
- `-j`：允许并行构建。

只构建压力测试及其依赖：

```bash
cmake --build build --target net_stress_test -j
```

运行程序时名称必须准确：

```bash
./build/net_stress_test
```

## 4. CTest 是什么

CTest 是 CMake 自带的测试运行器。先在 CMake 中注册测试：

```cmake
add_test(NAME net_stress
    COMMAND net_stress_test
)

set_tests_properties(net_stress PROPERTIES
    TIMEOUT 60
)
```

这里有两个不同名称：

```text
net_stress_test → CMake 可执行目标
net_stress      → CTest 中的测试名称
```

列出测试但不运行：

```bash
ctest --test-dir build -N
```

只运行压力测试：

```bash
ctest --test-dir build -R '^net_stress$' --output-on-failure
```

运行全部测试：

```bash
ctest --test-dir build --output-on-failure
```

CTest 通常根据程序退出码判定结果：`0` 表示通过，非零表示失败。

## 5. CI 与 CTest 的区别

CI 是 Continuous Integration（持续集成），是一套在代码提交后自动执行的工作流程，不是单个测试工具。GitHub Actions、GitLab CI 和 Jenkins 都可以承担 CI。

```text
CI
├── 获取代码
├── 安装依赖
├── 调用 CMake 配置
├── 调用编译器构建
├── 调用 CTest 执行测试
├── 运行 ASan/UBSan
└── 保存并展示结果
```

简而言之：CTest 负责运行测试，CI 负责自动组织整个构建和验证过程。

## 6. ASan 与 UBSan

AddressSanitizer（ASan）主要检查越界访问、释放后访问、重复释放和内存泄漏。UndefinedBehaviorSanitizer（UBSan）主要检查非法移位、溢出、未对齐访问等未定义行为。

使用独立目录，避免污染正常构建：

```bash
cmake \
    -S . \
    -B build-sanitize \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
```

只验证压力测试时：

```bash
cmake --build build-sanitize --target net_stress_test -j

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest \
    --test-dir build-sanitize \
    -R '^net_stress$' \
    --output-on-failure
```

验证全部测试时，必须先构建全部目标：

```bash
cmake --build build-sanitize -j

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitize --output-on-failure
```

如果只构建 `net_stress_test` 却运行全部 CTest，其他测试会因为对应程序不存在而显示 `Not Run` 或脚本启动失败；这不等于那些程序的代码一定存在缺陷。

## 7. 当前压力测试的验收含义

典型成功输出：

```text
connections=100 payload_bytes=1048576 total_bytes=104857600 ... verification=PASS
open_fds_before=4 open_fds_after=4
```

- 100 个连接各发送 1 MiB，总量为 100 MiB；
- `recv_calls` 和 `short_reads` 受系统调度和 TCP 分段影响，每次可能不同；
- `short_reads > 0` 证明程序确实遇到并正确处理了 TCP 短读；
- FD 绝对值可能随运行环境变化，验收重点是前后相等；
- Sanitizer 没有报告且程序返回 0，才算该轮验证通过。

## 8. 日常推荐命令

修改普通代码后：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

只修改网络压力测试时，可先快速验证相关目标：

```bash
cmake --build build --target net_stress_test -j
ctest --test-dir build -R '^net_stress$' --output-on-failure
```

准备提交或合并前，再执行普通全量测试和 Sanitizer 全量测试。
