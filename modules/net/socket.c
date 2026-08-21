#include "socket.h"

#include <sys/socket.h>
#include <stddef.h>
#include <errno.h>


int net_tcp_socket(void)
{
    return socket(AF_INET, SOCK_STREAM, 0);
}


int net_udp_socket(void)
{
    return socket(AF_INET, SOCK_DGRAM, 0);
}


int net_bind(
    int fd,
    const net_address_t *address)
{
    if (fd < 0 || address == NULL)
    {
        return -1;
    }

    return bind(fd,
        net_address_sockaddr(address),
         net_address_length(address));

}


int net_listen(
    int fd,
    int backlog)
{
    if (fd < 0 || backlog <= 0)
    {
        return -1;
    }

    return listen(fd, backlog);
}

int net_accept(int listen_fd)
{
    if (listen_fd < 0)
    {
        return -1;
    }

    int client_fd;

    /* accept 可能被已捕获信号中断；EINTR 时重新等待连接。 */
    do
    {
        client_fd = accept(listen_fd, NULL, NULL);
    } while (client_fd < 0 && errno == EINTR);

    return client_fd;
}


int net_connect(
    int fd,
    const net_address_t *address)
{
    if (fd < 0 || address == NULL)
    {
        return -1;
    }

    /*
     * 调用 connect()
     *
     * 注意：
     * connect 本身就是：
     *
     * 成功 → 0
     * 失败 → -1
     *
     * 所以不需要再写一层 if。
     */
    return connect(fd, net_address_sockaddr(address), net_address_length(address));

}

int net_send_all(
    int fd,
    const void *buffer,
    size_t length)
{
    const unsigned char *p;
    size_t total_written;

    if (fd < 0)
    {
        return -1;
    }

    if (length == 0)
    {
        return 0;
    }

    if (buffer == NULL)
    {
        return -1;
    }

    p = buffer;
    total_written = 0;

    while (total_written < length)
    {
        ssize_t n;

        n = send(
            fd,
            p + total_written,
            length - total_written,
            MSG_NOSIGNAL);

        if (n > 0)
        {
            total_written += (size_t)n;
        }
        else if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }
        else
        {
            return -1;
        }
    }

    return 0;
}

ssize_t net_recv(
    int fd,
    void *buffer,
    size_t length)
{
    ssize_t n;

    if (fd < 0)
    {
        return -1;
    }

    if (length == 0)
    {
        return 0;
    }

    if (buffer == NULL)
    {
        return -1;
    }

    while (1)
    {
        n = recv(fd, buffer, length, 0);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        return n;
    }
}

ssize_t net_sendto(
    int fd,
    const void *buffer,
    size_t length,
    const net_address_t *address)
{

    if (fd < 0 || address == NULL)
    {
        return -1;
    }

    if (length == 0)
    {
        return 0;
    }

    if (buffer == NULL)
    {
        return -1;
    }

    while (1)
    {
        ssize_t n;

        n = sendto(fd,
                    buffer,
                    length,
                    0,
                    net_address_sockaddr(address),
                    net_address_length(address));
        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        return n;
    }

}

ssize_t net_recvfrom(
    int fd,
    void *buffer,
    size_t length,
    net_address_t *source)
{
    socklen_t source_length;

    if (fd < 0 || source == NULL)
    {
        return -1;
    }

    if (length == 0)
    {
        return 0;
    }

    if (buffer == NULL)
    {
        return -1;
    }

    source_length = net_address_length(source);

    while (1)
    {
        ssize_t n;

        n = recvfrom(
            fd,
            buffer,
            length,
            0,
            net_address_sockaddr_mut(source),
            &source_length);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        return n;
    }
}