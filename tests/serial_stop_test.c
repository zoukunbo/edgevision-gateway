#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 600

#include "rs485_serial.h"
#include "graceful_shutdown.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
        goto fail; \
    } \
} while (0)

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int never_stop(void *context)
{
    (void)context;
    return 0;
}

static int stop_at_time(void *context)
{
    return now_ms() >= *(const int64_t *)context;
}

static int stop_on_signal(void *context)
{
    (void)context;
    return graceful_shutdown_requested();
}

/* 各场景在独立子进程执行，退出标志不会污染另一个信号场景。 */
static int run_case(int signo)
{
    int master = -1;
    int slave = -1;
    pid_t sender = -1;
    int status = 0;
    uint8_t buffer[8] = {0};
    const uint8_t input[8] = {1, 3, 0, 0, 0, 2, 0xc4, 0x0b};
    size_t received = 99;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char *path = ptsname(master);
    CHECK(path != NULL);
    slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(slave >= 0 && serial_configure_115200_8n1(slave) == 0);

    if (signo == 0) {
        CHECK(write(master, input, sizeof(input)) == (ssize_t)sizeof(input));
        CHECK(serial_read_exact_timeout_stop(slave, buffer, 8, 1000,
              &received, never_stop, NULL) == 1);
        CHECK(received == 8 && memcmp(buffer, input, 8) == 0);

        /* 已停止时不读取排队数据，留给后续正常调用。 */
        int64_t stop_time = 0;
        CHECK(write(master, input, 3) == 3);
        CHECK(serial_read_exact_timeout_stop(slave, buffer, 8, 1000,
              &received, stop_at_time, &stop_time) == -1);
        CHECK(errno == ECANCELED && received == 0);
        CHECK(serial_read_exact_timeout(slave, buffer, 3, 500, &received) == 1);
        CHECK(received == 3 && memcmp(buffer, input, 3) == 0);

        /* 无信号打断也须分片检查停止；部分数据保留，不等满10秒。 */
        CHECK(write(master, input, 3) == 3);
        int64_t start = now_ms();
        CHECK(start >= 0);
        stop_time = start + 150;
        int result = serial_read_exact_timeout_stop(slave, buffer, 8, 10000,
                         &received, stop_at_time, &stop_time);
        int saved = errno;
        int64_t elapsed = now_ms() - start;
        CHECK(result == -1 && saved == ECANCELED);
        CHECK(received == 3 && memcmp(buffer, input, 3) == 0);
        CHECK(elapsed >= 150 && elapsed < 1000);

        /* 分片超时不能误报总超时；总预算不能被重置。 */
        start = now_ms();
        CHECK(serial_read_exact_timeout_stop(slave, buffer, 8, 250,
              &received, never_stop, NULL) == 0);
        elapsed = now_ms() - start;
        CHECK(received == 0 && elapsed >= 245 && elapsed < 1000);
    } else {
        CHECK(graceful_shutdown_install() == 0);
        CHECK(write(master, input, 3) == 3);
        const pid_t parent = getpid();
        sender = fork();
        CHECK(sender >= 0);
        if (sender == 0) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 150000000};
            while (nanosleep(&delay, &delay) < 0) {
                if (errno != EINTR) _exit(1);
            }
            _exit(kill(parent, signo) == 0 ? 0 : 1);
        }
        int64_t start = now_ms();
        CHECK(start >= 0);
        int result = serial_read_exact_timeout_stop(slave, buffer, 8, 10000,
                         &received, stop_on_signal, NULL);
        int saved = errno;
        CHECK(result == -1 && saved == ECANCELED);
        CHECK(received == 3 && memcmp(buffer, input, 3) == 0);
        CHECK(now_ms() - start < 1000);
        CHECK(waitpid(sender, &status, 0) == sender);
        sender = -1;
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    /* 取消不转移所有权、不暗中close；调用者仍能正常使用并关闭FD。 */
    CHECK(fcntl(slave, F_GETFD) >= 0);
    CHECK(write(master, input, 8) == 8);
    CHECK(serial_read_exact_timeout(slave, buffer, 8, 500, &received) == 1);
    CHECK(received == 8 && memcmp(buffer, input, 8) == 0);
    (void)close(slave);
    (void)close(master);
    return 0;
fail:
    if (sender > 0) {
        (void)kill(sender, SIGKILL);
        (void)waitpid(sender, NULL, 0);
    }
    if (slave >= 0) (void)close(slave);
    if (master >= 0) (void)close(master);
    return 1;
}

int main(void)
{
    const int cases[] = {0, SIGINT, SIGTERM};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pid_t child = fork();
        if (child < 0) return EXIT_FAILURE;
        if (child == 0) _exit(run_case(cases[i]));
        int status;
        if (waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            return EXIT_FAILURE;
        }
    }
    puts("serial stop: normal/timeout/cancel/partial/SIGINT/SIGTERM/FD ownership PASS (PTY)");
    return EXIT_SUCCESS;
}
