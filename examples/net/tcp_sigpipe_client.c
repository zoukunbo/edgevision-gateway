#define _POSIX_C_SOURCE 200809L

#include "net_example_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080

static int send_probe(int fd, const char *message, const char *label)
{
    /* MSG_NOSIGNAL 把默认会终止进程的 SIGPIPE 转换为可处理的 EPIPE。 */
    ssize_t n = send(fd, message, strlen(message), MSG_NOSIGNAL);
    if (n >= 0)
    {
        printf("%s returned %zd\n", label, n);
        return 0;
    }
    if (errno == EPIPE || errno == ECONNRESET)
    {
        fprintf(stderr, "peer closed connection: %s\n", strerror(errno));
        return 1;
    }
    perror(label);
    return -1;
}

static void short_pause(void)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
    (void)nanosleep(&delay, NULL);
}

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    const char *message = "hello";
    int observed_peer_close = 0;
    int fd;

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (connect(fd,
                (struct sockaddr *)&endpoint.sockaddr,
                sizeof(endpoint.sockaddr)) != 0)
    {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    short_pause();
    /* 首次发送可能先进入本地缓冲区，因此使用有上限的探测循环。 */
    for (int attempt = 1; attempt <= 3 && !observed_peer_close; ++attempt)
    {
        char label[32];
        snprintf(label, sizeof(label), "send #%d", attempt);
        int result = send_probe(fd, message, label);
        if (result < 0)
        {
            close(fd);
            return EXIT_FAILURE;
        }
        observed_peer_close = result;
        short_pause();
    }

    close(fd);
    /* The exception test only passes after an expected peer-close error. */
    if (!observed_peer_close)
    {
        fprintf(stderr, "peer-close condition was not observed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
