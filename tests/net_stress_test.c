#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "address.h"
#include "socket.h"


#define PAYLOAD_SIZE (1024U * 1024U)
#define CONNECTION_COUNT 100
#define SEND_CHUNK_SIZE 137U

/*
 * 客户端线程所需的全部输入和输出。
 *
 * port：
 *   服务器实际监听的端口。
 *
 * payload：
 *   指向主线程生成的 1 MiB 测试数据。
 *   客户端线程只读取它，不修改，因此使用 const。
 *
 * result：
 *   客户端线程执行结果：
 *   0  表示连接和发送成功；
 *   -1 表示发生错误。
 */
struct client_context
{
    uint16_t port;
    const unsigned char *payload;
    int result;
};

/*
 * 创建只监听本机回环地址的 TCP 服务器。
 *
 * port：
 *   输出参数。函数成功后，写入系统实际分配的端口。
 *
 * 返回值：
 *   >= 0：监听 Socket 的文件描述符；
 *   -1：创建、绑定、监听或查询端口失败。
 */

int create_listener(uint16_t *port)
{
    net_address_t listen_address;
    struct sockaddr_in bound_address;
    socklen_t bound_address_length = sizeof(bound_address);
    int listen_fd = -1;

    if (port == NULL)
    {
        return -1;
    }

    /*
     * 创建 TCP Socket。
     */
    listen_fd = net_tcp_socket();
    if (listen_fd < 0)
    {
        return -1;
    }

    /*
     * 端口设置为 0，表示不指定固定端口，
     * 由操作系统自动选择一个当前可用的临时端口。
     *
     * 这样可以避免测试与其他程序争抢固定端口。
     */
    if (net_address_ipv4(
            &listen_address,
            "127.0.0.1",
            0) != 0)
    {
        close(listen_fd);
        return -1;
    }

    /*
     * 把 Socket 绑定到 127.0.0.1。
     * bind 成功后，操作系统会为端口 0 分配实际端口。
     */
    if (net_bind(listen_fd, &listen_address) != 0)
    {
        close(listen_fd);
        return -1;
    }

    /*
     * 将 Socket 转换为监听状态。
     *
     * CONNECTION_COUNT 作为等待连接队列的上限。
     * 当前测试将逐个建立连接，不要求真的同时排队 100 个。
     */
    if (net_listen(listen_fd, CONNECTION_COUNT) != 0)
    {
        close(listen_fd);
        return -1;
    }

    /*
     * bind 时传入的是端口 0，因此程序还不知道系统选择了哪个端口。
     * getsockname() 用来查询 Socket 当前绑定的真实地址。
     */
    if (getsockname(
            listen_fd,
            (struct sockaddr *)&bound_address,
            &bound_address_length) != 0)
    {
        close(listen_fd);
        return -1;
    }

    /*
     * sin_port 使用网络字节序。
     * ntohs() 将它转换为本机字节序，交给客户端使用。
     */
    *port = ntohs(bound_address.sin_port);

    return listen_fd;
}

static void *client_thread_main(void *argument)
{
    struct client_context *context = argument;
    int client_fd = -1;

    context->result = -1;

    /*
     * TODO 1：创建 TCP socket
     *
     * client_fd = net_tcp_socket();
     */
    client_fd = net_tcp_socket();
    if (client_fd < 0)
    {
        return NULL;
    }
    /*
    * 把文本形式的 IPv4 地址和端口转换为 Socket 地址。
    *
    * "127.0.0.1" 表示本机回环地址：
    * 数据不会经过物理网卡，只在本机 TCP/IP 协议栈中传输。
    *
    * context->port 由主线程在创建监听 Socket 后填写。
    */
    net_address_t server_address;

    if (net_address_ipv4(&server_address, "127.0.0.1", context->port) != 0)
    {
        close(client_fd);
        return NULL;
    }


    /*
    * 使用刚创建的 TCP Socket 连接服务器。
    *
    * client_fd：
    *   客户端 Socket 的文件描述符。
    *
    * &server_address：
    *   服务器的 IPv4 地址和端口。
    *
    * 返回值：
    *   0  表示 TCP 连接建立成功；
    *  -1  表示连接失败。
    */
    if (net_connect(client_fd, &server_address) != 0)
    {
        /*
        * Socket 已经创建，因此连接失败时也必须关闭，
        * 否则重复执行压力测试会造成文件描述符泄漏。
        */
        close(client_fd);
        return NULL;
    }

    /*
     * 分块发送 PAYLOAD_SIZE 字节
     *
     * 每次最多发送 SEND_CHUNK_SIZE，即 137 字节。
     * 这里故意使用较小且不整除 1 MiB的分块，
     * 用来模拟多次发送，并验证接收端不能假设
     * 一次 recv() 就能拿到完整消息。
     */

    size_t offset = 0;

    while (offset < PAYLOAD_SIZE)
    {
        /*
        * remaining：还有多少字节尚未发送。
        */
        size_t remaining = PAYLOAD_SIZE - offset;

        /*
        * 默认每次发送 137 字节。
        * 最后一块不足 137 字节时，只发送剩余数据。
        */
        size_t chunk_size =
            remaining < SEND_CHUNK_SIZE
                ? remaining
                : SEND_CHUNK_SIZE;

        /*
        * context->payload + offset 指向本次发送数据的起点。
        *
        * net_send_all() 保证：
        * 成功时，本次 chunk_size 个字节已经全部发送；
        * 失败时，返回 -1。
        */
        if (net_send_all(
                client_fd,
                context->payload + offset,
                chunk_size) != 0)
        {
            close(client_fd);
            return NULL;
        }

        /*
        * 只有本次数据全部发送成功后，才能推进偏移量。
        */
        offset += chunk_size;
    }

    context->result = 0;

    if (client_fd >= 0)
    {
        close(client_fd);
    }

    return NULL;
}

static int count_open_fds(void)
{
    DIR *directory;
    struct dirent *entry;
    int count = 0;

    directory = opendir("/proc/self/fd");
    if (directory == NULL)
    {
        return -1;
    }

    while ((entry = readdir(directory)) != NULL)
    {
        /* TODO：忽略 "." 和 ".." */
        /* TODO：其余目录项代表一个当前打开的 FD */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        count++;
    }

    closedir(directory);
    return count;
}

static void fill_payload(unsigned char *buffer, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        /*
        * 根据字节下标生成确定性的测试数据：
        *
        * 1. 31 与 256 互质，因此每 256 个位置可以覆盖全部字节值；
        * 2. 加 7 是为了设置非零起点；
        * 3. & 0xffU 只保留最低 8 位，将结果限制在 0～255；
        * 4. 该规律可在接收端重新计算，用来检测数据丢失、错位或损坏。
        *
        * 注意：这不是随机数或加密算法，只用于生成可重复的测试数据。
        */
        buffer[i] = (unsigned char)((i * 31U + 7U) & 0xffU);
    }
}

static int verify_payload(const unsigned char *buffer, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        unsigned char expected =
            (unsigned char)((i * 31U + 7U) & 0xffU);

        /* TODO：实际值不等于 expected 时返回 -1 */
        if (buffer[i] != expected)
        {
            return -1;
        }

    }

    return 0;
}

/*
 * 执行一次完整的 TCP 连接测试：
 *
 * 1. 启动客户端线程；
 * 2. 接受连接；
 * 3. 接收 1 MiB 数据；
 * 4. 等待客户端线程结束；
 * 5. 校验接收内容；
 * 6. 关闭本次连接。
 *
 * listen_fd 和 payload 由调用者持有，本函数不关闭、不释放。
 */
static int run_one_connection(
    int listen_fd,
    uint16_t port,
    const unsigned char *payload,
    unsigned char *received,
    size_t *recv_calls,
    size_t *short_reads)
{
    struct client_context context = {
        .port = port,
        .payload = payload,
        .result = -1
    };

    pthread_t client_thread;
    int client_thread_started = 0;
    int accepted_fd = -1;
    int result = -1;
    size_t received_bytes = 0;

    if (payload == NULL ||
        received == NULL ||
        recv_calls == NULL ||
        short_reads == NULL)
    {
        return -1;
    }

    if (pthread_create(
            &client_thread,
            NULL,
            client_thread_main,
            &context) != 0)
    {
        return -1;
    }

    client_thread_started = 1;

    accepted_fd = net_accept(listen_fd);
    if (accepted_fd < 0)
    {
        goto cleanup;
    }

    while (received_bytes < PAYLOAD_SIZE)
    {
        /*
         * requested 表示本次最多希望读取多少字节。
         */
        size_t requested = PAYLOAD_SIZE - received_bytes;

        ssize_t received_now = net_recv(
            accepted_fd,
            received + received_bytes,
            requested);

        if (received_now <= 0)
        {
            goto cleanup;
        }

        /*
         * 每调用一次并成功返回 recv，就增加一次统计。
         */
        (*recv_calls)++;

        /*
         * TCP 不保证一次返回 requested 字节。
         * 实际返回值小于请求值，就是一次短读。
         */
        if ((size_t)received_now < requested)
        {
            (*short_reads)++;
        }

        received_bytes += (size_t)received_now;
    }

    if (pthread_join(client_thread, NULL) != 0)
    {
        client_thread_started = 0;
        goto cleanup;
    }

    client_thread_started = 0;

    if (context.result != 0)
    {
        goto cleanup;
    }

    if (verify_payload(received, PAYLOAD_SIZE) != 0)
    {
        goto cleanup;
    }

    result = 0;

cleanup:
    /*
     * 如果接收过程中失败，先关闭连接。
     * 这样仍在发送的客户端线程会退出，避免 pthread_join 永久等待。
     */
    if (accepted_fd >= 0)
    {
        close(accepted_fd);
    }

    if (client_thread_started)
    {
        pthread_join(client_thread, NULL);
    }

    return result;
}

int main(void)
{
    unsigned char *payload = NULL;
    unsigned char *received = NULL;
    int listen_fd = -1;
    int fd_before;
    int fd_after;
    uint16_t port = 0;
    int exit_code = 1;



    /*
     * 在创建测试 Socket 前记录 FD 数量。
     */
    fd_before = count_open_fds();
    if (fd_before < 0)
    {
        fprintf(stderr, "failed to count file descriptors\n");
        goto cleanup;
    }

    /*
     * payload：客户端发送的数据。
     * received：服务器接收的数据。
     *
     * 两块内存必须分开，否则无法发现传输过程中发生的数据错误。
     */
    payload = malloc(PAYLOAD_SIZE);
    received = malloc(PAYLOAD_SIZE);

    if (payload == NULL || received == NULL)
    {
        fprintf(stderr, "failed to allocate payload buffers\n");
        goto cleanup;
    }

    fill_payload(payload, PAYLOAD_SIZE);

    /*
     * 创建服务器，并取得操作系统自动分配的端口。
     */
    listen_fd = create_listener(&port);
    if (listen_fd < 0)
    {
        fprintf(stderr, "failed to create TCP listener\n");
        goto cleanup;
    }

    size_t recv_calls = 0;
    size_t short_reads = 0;

    for (int connection = 0;
        connection < CONNECTION_COUNT;
        ++connection)
    {
        if (run_one_connection(
                listen_fd,
                port,
                payload,
                received,
                &recv_calls,
                &short_reads) != 0)
        {
            fprintf(
                stderr,
                "connection test failed: connection=%d\n",
                connection + 1);
            goto cleanup;
        }

        /*
        * 每 10 次打印一次进度，避免误以为程序卡住。
        */
        if ((connection + 1) % 10 == 0)
        {
            printf(
                "progress=%d/%d\n",
                connection + 1,
                CONNECTION_COUNT);
        }
    }

    printf(
        "connections=%d payload_bytes=%u "
        "total_bytes=%zu recv_calls=%zu "
        "short_reads=%zu verification=PASS\n",
        CONNECTION_COUNT,
        PAYLOAD_SIZE,
        (size_t)CONNECTION_COUNT * PAYLOAD_SIZE,
        recv_calls,
        short_reads);

    exit_code = 0;

cleanup:
    /*
     * 关闭顺序：
     *
     * 1. 已连接 Socket；
     * 2. 监听 Socket；
     * 3. 等待仍在运行的线程；
     * 4. 释放内存。
     */

    if (listen_fd >= 0)
    {
        close(listen_fd);
    }

    free(received);
    free(payload);

    /*
     * 所有测试 Socket 和线程都结束后，再统计 FD。
     */
    fd_after = count_open_fds();
    if (fd_after < 0)
    {
        fprintf(stderr, "failed to count final file descriptors\n");
        return 1;
    }

    printf(
        "open_fds_before=%d open_fds_after=%d\n",
        fd_before,
        fd_after);

    if (fd_before != fd_after)
    {
        fprintf(
            stderr,
            "file descriptor leak detected: before=%d after=%d\n",
            fd_before,
            fd_after);
        return 1;
    }

    return exit_code;
}