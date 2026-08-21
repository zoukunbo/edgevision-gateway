#!/usr/bin/env bash

set -euo pipefail

mode=${1:?test mode is required}
shift
test_dir=$(mktemp -d)
server_pid=

cleanup()
{
    # 任一断言失败都要清理后台服务，防止占用端口影响后续用例。
    if [[ -n ${server_pid:-} ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
        sleep 0.05
        if kill -0 "$server_pid" 2>/dev/null; then
            kill -KILL "$server_pid" 2>/dev/null || true
        fi
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT

# 使用进程号派生高位端口，降低并行测试与本机服务冲突的概率。
port=$((30000 + ($$ % 20000)))
address=127.0.0.1

wait_until_ready()
{
    # 等待服务端明确打印“已就绪”，比固定 sleep 更稳定。
    local output_file=$1
    local marker=$2

    for _ in {1..100}; do
        grep -q "$marker" "$output_file" 2>/dev/null && return 0
        if [[ -n ${server_pid:-} ]] && ! kill -0 "$server_pid" 2>/dev/null; then
            echo "server exited before becoming ready" >&2
            return 1
        fi
        sleep 0.02
    done
    echo "server readiness timeout" >&2
    return 1
}

case "$mode" in
    tcp_echo)
        server=${1:?server is required}
        client=${2:?client is required}
        "$server" "$address" "$port" >"$test_dir/server.out" 2>&1 &
        server_pid=$!
        wait_until_ready "$test_dir/server.out" "server listening"
        "$client" "$address" "$port" >"$test_dir/client.out" 2>&1
        wait "$server_pid"
        server_pid=
        grep -q "echo: hello" "$test_dir/client.out"
        ;;
    udp_echo)
        server=${1:?server is required}
        client=${2:?client is required}
        "$server" "$address" "$port" >"$test_dir/server.out" 2>&1 &
        server_pid=$!
        wait_until_ready "$test_dir/server.out" "UDP server bound"
        "$client" "$address" "$port" >"$test_dir/client.out" 2>&1
        wait "$server_pid"
        server_pid=
        grep -q "echo: hello udp" "$test_dir/client.out"
        ;;
    peer_close)
        server=${1:?server is required}
        client=${2:?client is required}
        "$server" "$address" "$port" >"$test_dir/server.out" 2>&1 &
        server_pid=$!
        wait_until_ready "$test_dir/server.out" "waiting on"
        "$client" "$address" "$port" >"$test_dir/client.out" 2>&1
        wait "$server_pid"
        server_pid=
        grep -Eq "Broken pipe|Connection reset by peer" "$test_dir/client.out"
        ;;
    no_service)
        # 不启动服务端，客户端必须在 connect 阶段失败。
        client=${1:?client is required}
        if "$client" "$address" "$port" >"$test_dir/client.out" 2>&1; then
            echo "client unexpectedly connected without a server" >&2
            exit 1
        fi
        grep -q "connect:" "$test_dir/client.out"
        ;;
    port_in_use)
        # 保持第一个监听者运行，验证第二次 bind 会因端口占用失败。
        server=${1:?server is required}
        client=${2:?client is required}
        "$server" "$address" "$port" >"$test_dir/server.out" 2>&1 &
        server_pid=$!
        wait_until_ready "$test_dir/server.out" "server listening"
        if "$server" "$address" "$port" >"$test_dir/second.out" 2>&1; then
            echo "second server unexpectedly bound the occupied port" >&2
            exit 1
        fi
        grep -q "bind:" "$test_dir/second.out"
        "$client" "$address" "$port" >"$test_dir/client.out" 2>&1
        wait "$server_pid"
        server_pid=
        ;;
    udp_timeout)
        # UDP 没有握手；无服务端时依靠 SO_RCVTIMEO 有界退出。
        client=${1:?client is required}
        if "$client" "$address" "$port" >"$test_dir/client.out" 2>&1; then
            echo "UDP client unexpectedly received a response" >&2
            exit 1
        fi
        grep -q "recvfrom timeout" "$test_dir/client.out"
        ;;
    invalid_address)
        client=${1:?client is required}
        if "$client" not-an-ip "$port" >"$test_dir/client.out" 2>&1; then
            echo "invalid address unexpectedly accepted" >&2
            exit 1
        fi
        grep -q "invalid IPv4 address" "$test_dir/client.out"
        ;;
    *)
        echo "unknown test mode: $mode" >&2
        exit 2
        ;;
esac
