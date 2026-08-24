#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "address.h"
#include "network_client.h"

/**
 * 业务数据回调。NetworkClient 负责收取字节，但协议解析属于上层。
 *
 * @param data 数据首地址，本示例不解析内容。
 * @param size 数据长度。
 * @param user_data 业务上下文，本示例未使用。
 */
static void handle_server_data(
    const void *data, size_t size, void *user_data)
{
    (void)data;
    (void)user_data;
    fprintf(stderr, "[demo] received %zu bytes\n", size);
}

/**
 * 可被信号打断后继续等待的秒级休眠辅助函数。
 *
 * @param seconds 需要等待的秒数。
 * @return 成功返回 0；失败返回 -1，并设置 errno。
 */
static int sleep_seconds(time_t seconds)
{
    struct timespec remaining = { seconds, 0 };

    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

/**
 * NetworkClient 最小示例：连接本地 8080 端口，每秒发送 PING 心跳，
 * 运行 8 秒后请求停止、join 并销毁客户端。
 *
 * @return 成功返回 EXIT_SUCCESS；失败返回 EXIT_FAILURE。
 */
int main(void)
{
    static const char heartbeat[] = "PING\n";
    net_address_t address;

    if (net_address_ipv4(&address, "127.0.0.1", 8080) < 0) {
        fprintf(stderr, "invalid server address\n");
        return EXIT_FAILURE;
    }

    network_client_config_t config = {
        .server_addrlen = net_address_length(&address),
        .connect_timeout_ms = 2000,
        .backoff_base_ms = 500,
        .backoff_max_ms = 2000,
        .heartbeat_interval_ms = 1000,
        .heartbeat_data = heartbeat,
        .heartbeat_size = sizeof(heartbeat) - 1,
        .on_data = handle_server_data,
        .user_data = NULL,
        .enable_logging = true
    };
    memcpy(&config.server_addr,
           net_address_sockaddr(&address),
           config.server_addrlen);

    network_client_t *client = network_client_create(&config);
    if (client == NULL) {
        fprintf(stderr, "create failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (network_client_start(client) < 0) {
        fprintf(stderr, "start failed: %s\n", strerror(errno));
        network_client_destroy(client);
        return EXIT_FAILURE;
    }

    int result = EXIT_SUCCESS;
    if (sleep_seconds(8) < 0) {
        fprintf(stderr, "sleep failed: %s\n", strerror(errno));
        result = EXIT_FAILURE;
    }

    if (network_client_request_stop(client) < 0) {
        fprintf(stderr, "stop failed: %s\n", strerror(errno));
        result = EXIT_FAILURE;
    } else if (network_client_join(client) < 0) {
        fprintf(stderr, "join failed: %s\n", strerror(errno));
        result = EXIT_FAILURE;
    }

    network_client_destroy(client);
    return result;
}
