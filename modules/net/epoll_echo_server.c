#define _POSIX_C_SOURCE 200809L

#include "epoll_echo_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define MAX_EVENTS 16
#define MAX_CONNECTIONS 128

typedef struct
{
    int active;
    int fd;                    /* 该状态属于哪个客户端。 */
    unsigned char output[BUFFER_SIZE];
    size_t output_length;      /* 本轮总共需要发送的字节数。 */
    size_t output_sent;        /* 已发送字节数，用于部分发送后续传。 */
} connection_t;

typedef struct
{
    int epoll_fd;
    int listen_fd;
    connection_t connections[MAX_CONNECTIONS];
} server_t;

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static connection_t *find_connection(server_t *server, int fd)
{
    size_t i;
    for (i = 0; i < MAX_CONNECTIONS; ++i)
        if (server->connections[i].active && server->connections[i].fd == fd)
            return &server->connections[i];
    return NULL;
}

static connection_t *add_connection(server_t *server, int fd)
{
    size_t i;
    for (i = 0; i < MAX_CONNECTIONS; ++i)
    {
        connection_t *connection = &server->connections[i];
        if (!connection->active)
        {
            connection->active = 1;
            connection->fd = fd;
            connection->output_length = 0;
            connection->output_sent = 0;
            return connection;
        }
    }
    return NULL;
}

static void remove_connection(connection_t *connection)
{
    if (connection == NULL)
        return;
    connection->active = 0;
    connection->fd = -1;
    connection->output_length = 0;
    connection->output_sent = 0;
}

static void close_connection(server_t *server, int fd)
{
    connection_t *connection = find_connection(server, fd);

    /* DEL 失败也必须 close；close 才真正释放进程持有的 FD。 */
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0 &&
        errno != ENOENT)
        perror("epoll_ctl DEL client");
    remove_connection(connection);
    close(fd);
}

static int change_interest(server_t *server, int fd, uint32_t events)
{
    struct epoll_event interest = {.events = events, .data.fd = fd};
    return epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, fd, &interest);
}

static void accept_ready_clients(server_t *server)
{
    for (;;)
    {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd >= 0)
        {
            connection_t *connection;
            struct epoll_event interest;

            if (set_nonblocking(client_fd) < 0)
            {
                close(client_fd);
                continue;
            }
            connection = add_connection(server, client_fd);
            if (connection == NULL)
            {
                close(client_fd);
                continue;
            }
            interest.events = EPOLLIN;
            interest.data.fd = client_fd;
            if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD,
                          client_fd, &interest) < 0)
            {
                remove_connection(connection);
                close(client_fd);
            }
            continue;
        }
        if (errno == EINTR)
            continue;

        /*
         * listen_fd 的一次 EPOLLIN 只表示 accept 队列非空，不代表只有
         * 一个新客户端。因此必须 accept 到 EAGAIN，把 A、B、C 全部取出。
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        perror("accept");
        return;
    }
}

static void read_client(server_t *server, int fd)
{
    connection_t *connection = find_connection(server, fd);
    if (connection == NULL)
    {
        close_connection(server, fd);
        return;
    }

    for (;;)
    {
        ssize_t n = recv(fd, connection->output, sizeof(connection->output), 0);
        if (n > 0)
        {
            /* 保存待发数据，并只在确有数据时才关注 EPOLLOUT。 */
            connection->output_length = (size_t)n;
            connection->output_sent = 0;
            if (change_interest(server, fd, EPOLLOUT) < 0)
                close_connection(server, fd);
            return;
        }
        if (n == 0)
        {
            /* recv 返回 0 表示对端正常关闭。 */
            close_connection(server, fd);
            return;
        }
        if (errno == EINTR)
            continue;
        /* EAGAIN 表示当前已读空，不是失败，也不是断线。 */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        close_connection(server, fd);
        return;
    }
}

static void write_client(server_t *server, int fd)
{
    connection_t *connection = find_connection(server, fd);
    if (connection == NULL)
    {
        close_connection(server, fd);
        return;
    }

    while (connection->output_sent < connection->output_length)
    {
        size_t remaining = connection->output_length - connection->output_sent;
        ssize_t n = send(fd, connection->output + connection->output_sent,
                         remaining, MSG_NOSIGNAL);
        if (n > 0)
        {
            connection->output_sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        /* 保留发送进度，等 ready[] 再次报告 EPOLLOUT 后续传。 */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        close_connection(server, fd);
        return;
    }

    connection->output_length = 0;
    connection->output_sent = 0;
    if (change_interest(server, fd, EPOLLIN) < 0)
        close_connection(server, fd);
}

static int run_event_loop(server_t *server)
{
    struct epoll_event ready[MAX_EVENTS];

    for (;;)
    {
        int count = epoll_wait(server->epoll_fd, ready, MAX_EVENTS, -1);
        int i;
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }

        /*
         * ready[] 不是所有连接，也不是 accept 后得到的新客户端数组。
         * 它是“本次已经发生所关注事件”的快照；count 是有效元素数量，
         * 所以本轮只访问 ready[0] 到 ready[count - 1]。
         */
        for (i = 0; i < count; ++i)
        {
            int event_fd = ready[i].data.fd;
            uint32_t events = ready[i].events;

            if (event_fd == server->listen_fd)
            {
                /* 一个 listen_fd 元素背后可能积压多个待 accept 连接。 */
                if (events & EPOLLIN)
                    accept_ready_clients(server);
                continue;
            }

            /* HUP 与 IN 同时出现时，先让下面的读逻辑取走剩余数据。 */
            if ((events & EPOLLERR) ||
                ((events & EPOLLHUP) && !(events & EPOLLIN)))
            {
                close_connection(server, event_fd);
                continue;
            }
            if (events & EPOLLIN)
                read_client(server, event_fd);

            /* 读分支可能已经关闭 FD，写前重新确认连接仍存在。 */
            if ((events & EPOLLOUT) && find_connection(server, event_fd) != NULL)
                write_client(server, event_fd);
        }
    }
}

int net_epoll_echo_server_run(const net_address_t *address, int backlog)
{
    server_t server;
    struct epoll_event interest;
    int result;

    if (address == NULL || backlog <= 0)
        return -1;
    memset(&server, 0, sizeof(server));
    server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server.listen_fd < 0 ||
        bind(server.listen_fd, net_address_sockaddr(address),
             net_address_length(address)) < 0 ||
        listen(server.listen_fd, backlog) < 0 ||
        set_nonblocking(server.listen_fd) < 0)
    {
        if (server.listen_fd >= 0)
            close(server.listen_fd);
        return -1;
    }

    server.epoll_fd = epoll_create1(0);
    if (server.epoll_fd < 0)
    {
        close(server.listen_fd);
        return -1;
    }
    interest.events = EPOLLIN;
    interest.data.fd = server.listen_fd;
    if (epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD,
                  server.listen_fd, &interest) < 0)
    {
        close(server.epoll_fd);
        close(server.listen_fd);
        return -1;
    }

    result = run_event_loop(&server);
    close(server.epoll_fd);
    close(server.listen_fd);
    return result;
}
