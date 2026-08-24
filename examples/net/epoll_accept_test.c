#define _POSIX_C_SOURCE 200809L

#include "net_example_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <string.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE 1024
#define MAX_EVENTS 16
#define MAX_CONNECTIONS 128

typedef struct
{   int active;
    int fd; // 这个状态属于那个客户端

    unsigned char output[BUFFER_SIZE]; // 需要发送的数据
    size_t output_length; // 总共需要发送多少字节
    size_t output_sent; //
}connection_t;




static connection_t connections[MAX_CONNECTIONS];

static connection_t *connection_add(int fd)
{
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (!connections[i].active) {
            connections[i].active = 1;
            connections[i].fd = fd;
            connections[i].output_length = 0;
            connections[i].output_sent = 0;
            return &connections[i];
        }
    }

    return NULL;
}

static connection_t *connection_find(int fd)
{
    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].active &&
            connections[i].fd == fd) {
            return &connections[i];
        }
    }

    return NULL;
}

static void connection_remove(connection_t *connection)
{
    if (connection == NULL) {
        return;
    }

    connection->active = 0;
    connection->fd = -1;
    connection->output_length = 0;
    connection->output_sent = 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        return -1;
    }

    return fcntl(fd,
                 F_SETFL,
                 flags | O_NONBLOCK);
}

static void close_connection(int epfd, int fd)
{
    connection_t *connection =
        connection_find(fd);

    if (epoll_ctl(epfd,
                  EPOLL_CTL_DEL,
                  fd,
                  NULL) == -1) {
        /*
         * 当前实验只记录。
         * FD可能已经被移除。
         */
        perror("epoll_ctl del");
    }

    connection_remove(connection);
    close(fd);

    printf("connection fd=%d closed\n", fd);
}

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    int listen_fd = -1;
    unsigned char buffer[BUFFER_SIZE];

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

    /* socket 只创建端点；随后还需要 bind 和 listen 才能接收连接。 */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (bind(listen_fd,
             (struct sockaddr *)&endpoint.sockaddr,
             sizeof(endpoint.sockaddr)) != 0)
    {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }


    
    if (listen(listen_fd, 16) != 0)
    {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("server listening on %s:%u\n", endpoint.address, endpoint.port);
    fflush(stdout);

    if (set_nonblocking(listen_fd) == -1) {
        perror("set_nonblocking listen_fd");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event interest = {
        .events = EPOLLIN,
        .data.fd = listen_fd,
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &interest) == -1) {
        perror("epoll_ctl");
        close(epfd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("waiting for a new connection...\n");

    struct epoll_event ready[MAX_EVENTS];

    while(1)
    {
        int count = epoll_wait(epfd, ready, MAX_EVENTS, -1);

        if (count == -1) {

            if (errno == EINTR)
            {
                continue;
            }
            

            perror("epoll_wait");
            close(epfd);
            close(listen_fd);
            return EXIT_FAILURE;
        }
        for (int i = 0; i < count; i++)
        {
            int event_fd = ready[i].data.fd;
            uint32_t events = ready[i].events;

            if ((events & EPOLLIN) && event_fd == listen_fd)
            {
                while(1)
                {
                    int new_client_fd = accept(listen_fd, NULL, NULL);
                    if (new_client_fd >= 0)
                    {
                    
                        printf("accepted client_fd=%d\n", new_client_fd);

                        if (set_nonblocking(new_client_fd) == -1)
                        {
                            perror("set_nonblocking");
                            close(new_client_fd);
                            continue;
                        }
                        
                        connection_t *new_connection = connection_add(new_client_fd);

                        if (new_connection == NULL) {
                            fprintf(stderr, "connection table full\n");
                            close(new_client_fd);
                            continue;
                        }

                        struct epoll_event client_insterest = {
                            .events = EPOLLIN,
                            .data.fd = new_client_fd
                        };

                        if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_client_fd, &client_insterest) == - 1)
                        {
                            perror("epoll_ctl client");
                            connection_remove(new_connection);
                            close(new_client_fd);
                            continue;
                        }
                        
                        printf("new_client_fd=%d registered, waiting for data...\n",new_client_fd);
                        /*
                        * 注册成功后继续accept，
                        * 尝试取出队列中的下一个连接。
                        */
                        continue;
                    }

                    if (errno == EINTR)
                    {
                        continue;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        /* 队列连接已经空 */
                        break;
                    }
                    
                    perror("accept");
                    break;
                }

                 /*
                 * listen_fd处理结束，
                 * 不进入后面的客户端read/send逻辑。
                 */
                continue;
            }


            /*
            * EPOLLERR：发生连接错误，直接清理。
            *
            * EPOLLHUP但没有EPOLLIN：
            * 已经挂断并且没有可读数据，直接清理。
            *
            * 如果EPOLLHUP和EPOLLIN同时出现，
            * 暂时让下面的read逻辑读取剩余数据。
            */

            if ((events & EPOLLERR) || ((events & EPOLLHUP) && !(events & EPOLLIN)))
            {
                fprintf(stderr,
                        "connection fd=%d error/hangup, events=0x%x\n",
                        event_fd,
                        events);

                close_connection(epfd, event_fd);
                continue;
            }
            

            if (events & EPOLLIN){

                connection_t *connection =
                    connection_find(event_fd);

                if (connection == NULL) {
                    fprintf(stderr,
                            "missing connection for fd=%d\n",
                            event_fd);

                    epoll_ctl(epfd,
                            EPOLL_CTL_DEL,
                            event_fd,
                            NULL);

                    close(event_fd);
                    continue;
                }
                
                while(1) {

                    ssize_t n = read(event_fd, buffer, sizeof(buffer));
                    
                    if (n > 0) {
                        printf("event_fd=%d read %zd bytes\n",event_fd, n);

                        /* 保存以后需要发送的数据*/
                        memcpy(connection->output, buffer, (size_t)n);
                        
                        connection->output_length = (size_t)n;
                        connection->output_sent = 0;

                        struct epoll_event write_interest = {
                            .events = EPOLLOUT,
                            .data.fd = event_fd,
                        };

                        if (epoll_ctl(epfd, EPOLL_CTL_MOD, event_fd, &write_interest) == -1)
                        {
                            perror("epoll_ctl enable EPOLLOUT");
                            connection_remove(connection);
                            close(event_fd);
                        }
                        break;
                    } 
                    else if (n == 0)
                    {
                        if (epoll_ctl(epfd,
                                    EPOLL_CTL_DEL,
                                    event_fd,
                                    NULL) == -1) {
                            perror("epoll_ctl del");
                        }

                        connection_remove(connection);
                        close(event_fd);

                        printf("event_fd=%d closed by peer\n", event_fd);

                        break;
                    } 
                    else if(errno == EINTR)
                    {
                        continue;
                    }
                    else if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        break;
                    } 
                    else
                    {
                        perror("read");
                        epoll_ctl(epfd, EPOLL_CTL_DEL, event_fd, NULL);
                        connection_remove(connection);
                        close(event_fd);
                        break;
                    }
                }
            }
            if (events & EPOLLOUT)
            {
                connection_t *connection = connection_find(event_fd);

                if (connection == NULL)
                {
                    fprintf(stderr,
                            "missing writable connection fd=%d\n",
                            event_fd);
                    continue;
                }
                
                size_t remaining = connection->output_length - connection->output_sent;

                ssize_t n = send(event_fd, connection->output + connection->output_sent, remaining, MSG_NOSIGNAL);

                if (n > 0)
                {
                    connection->output_sent += (size_t)n;

                    printf("event_fd=%d sent %zd bytes, "
                            "progress=%zu/%zu\n",
                            event_fd,
                            n,
                            connection->output_sent,
                            connection->output_length);

                    if (connection->output_sent == connection->output_length)
                    {
                        connection->output_length = 0;
                        connection->output_sent = 0;

                        /* 已经没有待发数据*/

                        struct epoll_event read_intersted = {
                            .events = EPOLLIN,
                            .data.fd = event_fd,
                        };

                        if (epoll_ctl(epfd, EPOLL_CTL_MOD, event_fd, &read_intersted) == -1)
                        {
                             perror("epoll_ctl disable EPOLLOUT");

                            connection_remove(connection);
                            close(event_fd);
                        }
                        
                    }
                }
                else if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    /* 仍不能发送，保留output_sent,继续等待EPOLLOUT*/
                }
                else if (n < 0 && errno == EINTR) 
                {
                    /* 状态没有发生改变，继续等待下一次事件*/
                }
                else
                {
                    perror("send");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, event_fd, NULL);
                    connection_remove(connection);
                    close(event_fd);
                }

            }
            
        }
    }
    

    close(epfd);
    close(listen_fd);
    return EXIT_SUCCESS;
}
