#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int wait_stdin_once(void)
{
    /* TODO 1：创建 epoll 实例，不使用额外 flag。 */
    int epfd = epoll_create1(0);

    if (epfd == -1) {
        fprintf(stderr, "epoll_create1: %s\n", strerror(errno));
        return -1;
    }

    struct epoll_event interest = {
        .events = EPOLLIN,
        .data.fd = STDIN_FILENO,
    };

    /* 注册关注事件
     * epoll_ctl(epfd, 操作， 要注册的fd, 关注信息)
        EPOLL_CTL_ADD  添加并开始监听
        EPOLL_CTL_MOD  修改已经注册的关注事件
        EPOLL_CTL_DEL  删除，不再监听
     * 把 STDIN_FILENO 添加到 epfd，
     * 关注信息保存在 interest 中。
     */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD ,STDIN_FILENO, &interest) == -1) {
        fprintf(stderr, "epoll_ctl: %s\n", strerror(errno));
        close(epfd);
        return -1;
    }

    printf("waiting for input...\n");

    struct epoll_event ready[1];

    /*
     * epoll_wait 休眠
     * 返回的事件必须写入ready
     * 最多接收 1 个事件；
     * timeout 使用 -1，表示一直等待。
     */
    int count = epoll_wait(epfd, &ready[0], 1, -1);

    if (count == -1) {
        fprintf(stderr, "epoll_wait: %s\n", strerror(errno));
        close(epfd);
        return -1;
    }

    // fd 变为可读
    printf("epoll_wait returned: count=%d\n", count);

    if (ready[0].events & EPOLLIN) {
        printf("fd=%d is readable\n", ready[0].data.fd);

        char buffer[64];

        /*
        * TODO 1：
        * 从 ready[0].data.fd 读取数据。
        * 最多读取 sizeof(buffer) - 1 个字节，
        * 为结尾的 '\0' 留一个位置。
        */
        ssize_t bytes_read = read(ready[0].data.fd, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            /*
            * TODO 2：
            * read() 返回的是原始字节，不会自动添加字符串结尾。
            * 在实际数据之后写入 '\0'。
            */
            /* 你的代码 */
            buffer[bytes_read] = '\0';
            printf("read %zd bytes: %s", bytes_read, buffer);
        } else if (bytes_read == 0) {
            printf("stdin reached EOF\n");
        } else {
            fprintf(stderr, "read: %s\n", strerror(errno));
        }
}

    close(epfd);
    return 0;
}

int main(void)
{
    return wait_stdin_once() == 0 ? 0 : 1;
}