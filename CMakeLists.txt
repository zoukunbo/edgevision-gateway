# cmake_minimum_required 的通用语法：
#   cmake_minimum_required(VERSION <最低版本>)
#
# 它告诉 CMake：这个工程至少需要哪个版本。版本过低时直接报错，
# 避免旧版 CMake 对后面命令的解释方式不同。
cmake_minimum_required(VERSION 3.16)

# project 的常用语法：
#   project(<工程名> VERSION <版本号> LANGUAGES <语言列表>)
#
# 这里会创建工程 edgevision_gateway，并声明只使用 C 语言。
# project() 还会生成一些内置变量，例如：
#   PROJECT_NAME             = edgevision_gateway
#   PROJECT_VERSION          = 0.1.0
#   CMAKE_PROJECT_NAME       = edgevision_gateway
project(edgevision_gateway VERSION 0.1.0 LANGUAGES C)

# set 的基本语法：
#   set(<变量名> <值>)
# 读取变量时使用 ${变量名}，例如 ${CMAKE_C_STANDARD}。
#
# 下面三项要求使用标准 C11，不允许 CMake 悄悄降级，也不使用 GNU 扩展方言。
# 最终效果类似编译参数：-std=c11，而不是 -std=gnu11。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# option 的语法：
#   option(<选项名> "给使用者看的说明" <默认值 ON/OFF>)
#
# option 会创建一个布尔缓存变量。配置工程时可以覆盖它：
#   cmake -S . -B build -DEDGEVISION_WARNINGS_AS_ERRORS=OFF
option(EDGEVISION_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# find_package 用来寻找外部依赖：
#   find_package(<包名> REQUIRED)
#
# REQUIRED 表示找不到就停止配置。找到后，现代 CMake 通常提供“导入目标”：
#   Threads::Threads 代表线程库及其正确编译/链接参数；
#   ZLIB::ZLIB       代表 zlib 的头文件和链接库。
# 使用导入目标比手写 -pthread、-lz 更便于跨平台。
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# function/endfunction 定义一个可重复调用的 CMake 函数：
#   function(<函数名> <形参...>)
#       ...
#   endfunction()
#
# 调用 edgevision_enable_warnings(gateway) 时，字符串 gateway 会赋给形参 target。
# ${target} 是“配置阶段变量展开”，会被替换成实际目标名称。
function(edgevision_enable_warnings target)
    # target_compile_options 的语法：
    #   target_compile_options(<目标> <作用域> <编译选项...>)
    #
    # PRIVATE 表示这些警告只用于编译当前目标，不传播给依赖它的其他目标。
    # -Wall/-Wextra/-Wpedantic 开启较严格的 GCC/Clang 警告。
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic

        # $<...> 是“生成器表达式”，在生成具体构建规则时才求值。
        # 外层格式：$<条件:条件为真时产生的内容>
        # $<BOOL:...> 把 ON/OFF 转成真假；选项为 ON 时添加 -Werror。
        $<$<BOOL:${EDGEVISION_WARNINGS_AS_ERRORS}>:-Werror>
    )
endfunction()

# -----------------------------------------------------------------------------
# 正式库目标
# -----------------------------------------------------------------------------

# add_library 的语法：
#   add_library(<目标名> STATIC|SHARED <源文件...>)
#
# STATIC 创建静态库。这里生成的文件通常叫 libedgevision_log.a。
# “目标名”是 CMake 内部引用名称，不需要写 lib 前缀或 .a 后缀。
add_library(edgevision_log STATIC
    modules/log/async_logger.c
    modules/blocking_queue/blocking_queue.c
)

# target_include_directories 告诉编译器去哪里寻找 #include 头文件：
#   target_include_directories(<目标> PUBLIC|PRIVATE|INTERFACE <目录...>)
#
# PUBLIC 有两层含义：
#   1. 编译 edgevision_log 自己时需要这些目录；
#   2. 链接 edgevision_log 的目标也继承这些目录。
# 这符合 async_logger.h 属于库公开接口的事实。
target_include_directories(edgevision_log PUBLIC
    modules/log
    modules/blocking_queue
)

# target_compile_definitions 相当于给编译器添加 -D宏定义。
# PRIVATE 表示使用者不需要继承这个实现细节。
target_compile_definitions(edgevision_log PRIVATE
    _POSIX_C_SOURCE=200809L
)

# target_link_libraries 声明“目标之间的依赖关系”：
#   target_link_libraries(<目标> PUBLIC|PRIVATE|INTERFACE <依赖...>)
#
# PUBLIC 表示 edgevision_log 自己需要线程库和 zlib，使用它的目标也需要这两个依赖。
# CMake 会自动按照依赖关系决定正确的链接顺序。
target_link_libraries(edgevision_log PUBLIC
    Threads::Threads
    ZLIB::ZLIB
)
edgevision_enable_warnings(edgevision_log)

# graceful shutdown 单独做成库，后续其他程序也可以复用。
add_library(edgevision_runtime STATIC
    modules/runtime/graceful_shutdown.c
)
target_include_directories(edgevision_runtime PUBLIC
    modules/runtime
)
target_compile_definitions(edgevision_runtime PRIVATE
    _POSIX_C_SOURCE=200809L
)
edgevision_enable_warnings(edgevision_runtime)

# modules/net 的正式网络适配层。
add_library(edgevision_net STATIC
    modules/net/address.c
    modules/net/socket.c
)
target_include_directories(edgevision_net PUBLIC
    modules/net
)
target_compile_definitions(edgevision_net PRIVATE
    _POSIX_C_SOURCE=200809L
)
edgevision_enable_warnings(edgevision_net)

# -----------------------------------------------------------------------------
# 主程序目标
# -----------------------------------------------------------------------------

# add_executable 的语法：
#   add_executable(<目标名> <源文件...>)
#
# 它创建可执行目标 gateway；具体输出路径由构建目录和生成器决定。
add_executable(gateway
    apps/gateway/main.c
)
target_compile_definitions(gateway PRIVATE
    _POSIX_C_SOURCE=200809L
)

# gateway 的源码会自动继承三个 PUBLIC 库暴露的头文件目录和传递依赖。
# PRIVATE 表示：gateway 使用这些库，但 gateway 本身不是供其他目标链接的库，
# 因而不需要继续向下传播依赖。
target_link_libraries(gateway PRIVATE
    edgevision_log
    edgevision_runtime
    edgevision_net
)
edgevision_enable_warnings(gateway)

# -----------------------------------------------------------------------------
# 独立网络学习示例
# -----------------------------------------------------------------------------

# CMake 的列表本质上是以分号分隔的字符串。
# set(EDGEVISION_NET_EXAMPLES a b c) 创建列表变量；这里换行只是为了易读。
set(EDGEVISION_NET_EXAMPLES
    tcp_server
    tcp_client
    tcp_sigpipe_server
    tcp_sigpipe_client
    udp_server
    udp_client
)

# foreach 的语法：
#   foreach(<循环变量> IN LISTS <列表变量名>)
#       ...
#   endforeach()
#
# 注意 IN LISTS 后写变量“名称”，不是 ${EDGEVISION_NET_EXAMPLES} 的展开结果。
# 循环体内 ${example} 依次变成 tcp_server、tcp_client 等。
foreach(example IN LISTS EDGEVISION_NET_EXAMPLES)
    # 例如第一次循环展开成：
    #   add_executable(tcp_server examples/net/tcp_server.c)
    add_executable(${example}
        examples/net/${example}.c
    )
    target_include_directories(${example} PRIVATE
        examples/net
    )
    target_compile_definitions(${example} PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    edgevision_enable_warnings(${example})
endforeach()

# -----------------------------------------------------------------------------
# 自动测试（CTest）
# -----------------------------------------------------------------------------

# include(CTest) 加载 CMake 自带的测试支持，并创建 BUILD_TESTING 选项。
# 默认 BUILD_TESTING=ON；如只想构建不生成测试，可配置：
#   cmake -S . -B build -DBUILD_TESTING=OFF
include(CTest)

# if/endif 根据变量真假决定是否处理其中命令。
if(BUILD_TESTING)
    # 测试程序首先也是普通可执行目标，然后才通过 add_test 注册给 CTest。
    add_executable(async_logger_test
        tests/async_logger_test.c
    )
    target_link_libraries(async_logger_test PRIVATE
        edgevision_log
    )
    edgevision_enable_warnings(async_logger_test)

    # add_test 的常用语法：
    #   add_test(NAME <CTest中的名字> COMMAND <程序> <参数...>)
    #
    # COMMAND async_logger_test 可以直接写目标名；CMake 会找到实际生成的程序。
    add_test(NAME async_logger
        COMMAND async_logger_test
    )

    add_executable(net_module_test
        tests/net_module_test.c
    )
    target_link_libraries(net_module_test PRIVATE
        edgevision_net
    )
    edgevision_enable_warnings(net_module_test)
    add_test(NAME net_module
        COMMAND net_module_test
    )

    # ${CMAKE_CURRENT_SOURCE_DIR} 在这里表示当前 CMakeLists.txt 所在源码目录，
    # 即仓库根目录。使用绝对路径可避免 CTest 从 build/ 运行时找不到脚本。
    #
    # $<TARGET_FILE:gateway> 是生成器表达式，会展开为 gateway 可执行文件
    # 的完整路径，例如 /home/.../build/gateway。
    add_test(NAME gateway_smoke
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gateway_smoke.sh
            $<TARGET_FILE:gateway>
    )

    # 同一个脚本通过最后一个参数选择发送 SIGTERM 或 SIGINT。
    add_test(NAME graceful_shutdown_sigterm
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_graceful_shutdown.sh
            $<TARGET_FILE:gateway>
            SIGTERM
    )
    add_test(NAME graceful_shutdown_sigint
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_graceful_shutdown.sh
            $<TARGET_FILE:gateway>
            SIGINT
    )

    # 保存重复使用的脚本路径。变量只在 CMake 配置阶段展开。
    set(NET_TEST_SCRIPT
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_network_examples.sh
    )

    # 以下测试都调用同一脚本：第一个参数表示场景，后面传入所需程序路径。
    add_test(NAME net_tcp_echo
        COMMAND
            ${NET_TEST_SCRIPT}
            tcp_echo
            $<TARGET_FILE:tcp_server>
            $<TARGET_FILE:tcp_client>
    )
    add_test(NAME net_udp_echo
        COMMAND
            ${NET_TEST_SCRIPT}
            udp_echo
            $<TARGET_FILE:udp_server>
            $<TARGET_FILE:udp_client>
    )
    add_test(NAME net_peer_close
        COMMAND
            ${NET_TEST_SCRIPT}
            peer_close
            $<TARGET_FILE:tcp_sigpipe_server>
            $<TARGET_FILE:tcp_sigpipe_client>
    )
    add_test(NAME net_no_service
        COMMAND
            ${NET_TEST_SCRIPT}
            no_service
            $<TARGET_FILE:tcp_client>
    )
    add_test(NAME net_port_in_use
        COMMAND
            ${NET_TEST_SCRIPT}
            port_in_use
            $<TARGET_FILE:tcp_server>
            $<TARGET_FILE:tcp_client>
    )
    add_test(NAME net_udp_timeout
        COMMAND
            ${NET_TEST_SCRIPT}
            udp_timeout
            $<TARGET_FILE:udp_client>
    )
    add_test(NAME net_invalid_address
        COMMAND
            ${NET_TEST_SCRIPT}
            invalid_address
            $<TARGET_FILE:tcp_client>
    )

    # -------------------------------------------------------------------------
    # TCP 大消息与多连接压力测试
    # -------------------------------------------------------------------------

    # 创建一个名为 net_stress_test 的可执行目标。
    #
    #这里只需要列出测试文件本身：
    #   tests/net_stress_test.c
    #
    # address.c 和 socket.c 不需要再次写在这里，因为它们已经被编译进
    # edgevision_net 静态库，后面通过 target_link_libraries 链接。
    add_executable(net_stress_test
        tests/net_stress_test.c
    )

    # 声明 net_stress_test 所依赖的库。
    #
    # edgevision_net：
    #   提供 net_tcp_socket、net_bind、net_listen、net_accept、
    #   net_connect、net_send_all、net_recv 等网络函数。
    #
    # Threads::Threads：
    #   提供 pthread_create、pthread_join 等线程功能。
    #
    # edgevision_net 的头文件目录被声明为 PUBLIC：
    #
    #   target_include_directories(edgevision_net PUBLIC modules/net)
    #
    # 因此，链接 edgevision_net 后，net_stress_test 会自动继承
    # modules/net 头文件搜索路径，不需要再次编写：
    #
    #   target_include_directories(net_stress_test PRIVATE modules/net)
    target_link_libraries(net_stress_test PRIVATE
        edgevision_net
        Threads::Threads
    )

    # 为这个目标启用：
    #   -Wall
    #   -Wextra
    #   -Wpedantic
    #   -Werror（由 EDGEVISION_WARNINGS_AS_ERRORS 控制）
    edgevision_enable_warnings(net_stress_test)

    # 把可执行程序注册为 CTest 测试。
    #
    # NAME net_stress：
    #   是 CTest 中显示的测试名称。
    #
    # COMMAND net_stress_test：
    #   这里写的是 CMake 目标名，不是手工拼接的文件路径。
    #   CMake 会自动找到 build 目录中生成的实际程序。
    add_test(NAME net_stress
        COMMAND net_stress_test
    )

    # 最多允许运行 60 秒。
    #
    # 正常情况下这个测试只需要几秒；
    # 如果线程、accept 或 recv 意外死锁，CTest 会在 60 秒后终止它。
    set_tests_properties(net_stress PROPERTIES
        TIMEOUT 60
    )

    # set_tests_properties 修改已经注册的测试属性：
    #   set_tests_properties(<测试名...> PROPERTIES <属性> <值> ...)
    #
    # UDP 客户端内部等待约 2 秒；CTest 最多给它 5 秒，防止异常时永久卡住。
    set_tests_properties(net_udp_timeout PROPERTIES
        TIMEOUT 5
    )
endif()
