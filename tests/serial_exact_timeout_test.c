#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 600

#include "rs485_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
        goto fail; \
    } \
} while (0)

static volatile sig_atomic_t signal_count;

static void on_alarm(int signo)
{
    (void)signo;
    signal_count++;
}

static int delay_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000,
    };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    int master = -1;
    int slave = -1;
    pid_t writer = -1;
    int handler_installed = 0;
    struct sigaction previous_action;
    const struct itimerval stopped = {0};
    uint8_t buffer[9] = {0};
    const uint8_t input[9] = {1, 3, 0, 0, 0, 2, 0xC4, 0x0B, 0xAA};
    size_t received = 99;
    int status = 0;

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char *name = ptsname(master);
    CHECK(name != NULL);
    slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(slave >= 0 && serial_configure_115200_8n1(slave) == 0);

    CHECK(serial_read_exact_timeout(-1, buffer, 8, 100, &received) == -1);
    CHECK(errno == EINVAL && received == 0);
    CHECK(serial_read_exact_timeout(slave, NULL, 8, 100, &received) == -1);
    CHECK(serial_read_exact_timeout(slave, buffer, 0, 100, &received) == -1);
    CHECK(serial_read_exact_timeout(slave, buffer, 8, 0, &received) == -1);
    CHECK(serial_read_exact_timeout(slave, buffer, 8, 100, NULL) == -1);

    /* 绝对截止时间接口：错误参数、过去的截止时间不消费排队输入。 */
    int64_t deadline = 123;
    CHECK(serial_deadline_after_ms(0, &deadline) == -1 && errno == EINVAL);
    CHECK(deadline == 123);
    CHECK(serial_deadline_after_ms(100, NULL) == -1 && errno == EINVAL);
    CHECK(serial_read_exact_until_stop(slave, buffer, 8, -1,
          &received, NULL, NULL) == -1 && errno == EINVAL);
    CHECK(received == 0);
    CHECK(write(master, input, 9) == 9);
    CHECK(serial_read_exact_until_stop(slave, buffer, 3, 0,
          &received, NULL, NULL) == 0 && received == 0);

    /* 正常两段读取同一帧；第9字节仍留给下一次读取。 */
    CHECK(serial_deadline_after_ms(1000, &deadline) == 0);
    size_t header_received = 99;
    size_t body_received = 99;
    CHECK(serial_read_exact_until_stop(slave, buffer, 3, deadline,
          &header_received, NULL, NULL) == 1);
    CHECK(serial_read_exact_until_stop(slave, buffer + 3, 5, deadline,
          &body_received, NULL, NULL) == 1);
    CHECK(header_received == 3 && body_received == 5);
    CHECK(memcmp(buffer, input, 8) == 0);
    /* 极大截止时间也不能在转换poll毫秒参数时溢出。 */
    CHECK(serial_read_exact_until_stop(slave, buffer, 1, INT64_MAX,
          &received, NULL, NULL) == 1);
    CHECK(received == 1 && buffer[0] == 0xAA);

    /* 总预算300ms：180ms收到头+1字节，360ms才收到其余4字节。
     * 第二段必须在原截止时间超时，不能重新获得300ms预算而收齐。
     */
    CHECK(serial_deadline_after_ms(300, &deadline) == 0);
    writer = fork();
    CHECK(writer >= 0);
    if (writer == 0) {
        (void)close(slave);
        if (delay_ms(180) < 0 || write(master, input, 4) != 4 ||
            delay_ms(180) < 0 || write(master, input + 4, 4) != 4) {
            _exit(1);
        }
        _exit(0);
    }
    CHECK(serial_read_exact_until_stop(slave, buffer, 3, deadline,
          &header_received, NULL, NULL) == 1);
    CHECK(serial_read_exact_until_stop(slave, buffer + 3, 5, deadline,
          &body_received, NULL, NULL) == 0);
    CHECK(header_received == 3 && body_received == 1);
    CHECK(memcmp(buffer, input, 4) == 0);
    CHECK(waitpid(writer, &status, 0) == writer);
    writer = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* 迟到的字节没有被丢弃，也不会污染下面的独立场景。 */
    CHECK(serial_read_exact_timeout(slave, buffer, 4, 100, &received) == 1);
    CHECK(received == 4 && memcmp(buffer, input + 4, 4) == 0);

    /* 两次间隔发送3+5字节，第9字节不能被本次8字节请求吞掉。 */
    writer = fork();
    CHECK(writer >= 0);
    if (writer == 0) {
        (void)close(slave);
        if (delay_ms(20) < 0 || write(master, input, 3) != 3 ||
            delay_ms(80) < 0 || write(master, input + 3, 6) != 6) {
            _exit(1);
        }
        _exit(0);
    }
    CHECK(serial_read_exact_timeout(slave, buffer, 8, 1000, &received) == 1);
    CHECK(received == 8 && memcmp(buffer, input, 8) == 0);
    CHECK(waitpid(writer, &status, 0) == writer);
    writer = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(serial_read_exact_timeout(slave, buffer, 1, 100, &received) == 1);
    CHECK(received == 1 && buffer[0] == 0xAA);

    /* 完全没有输入时超时，不残留上次的received。 */
    CHECK(serial_read_exact_timeout(slave, buffer, 8, 30, &received) == 0);
    CHECK(received == 0);

    /* 先收到3字节；每10ms打断poll，仍须遵守原120ms预算。 */
    struct sigaction action = {0};
    action.sa_handler = on_alarm;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGALRM, &action, &previous_action) == 0);
    handler_installed = 1;
    const struct itimerval timer = {
        .it_interval = {.tv_sec = 0, .tv_usec = 10000},
        .it_value = {.tv_sec = 0, .tv_usec = 10000},
    };
    CHECK(write(master, input, 3) == 3);
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    int64_t start = now_ms();
    CHECK(start >= 0);
    int result = serial_read_exact_timeout(slave, buffer, 8, 120, &received);
    int64_t end = now_ms();
    CHECK(setitimer(ITIMER_REAL, &stopped, NULL) == 0);
    CHECK(result == 0 && received == 3 && memcmp(buffer, input, 3) == 0);
    CHECK(signal_count > 0 && end >= start);
    CHECK(end - start >= 115 && end - start < 1000);

    CHECK(sigaction(SIGALRM, &previous_action, NULL) == 0);
    handler_installed = 0;
    int closed_fd = slave;
    CHECK(close(slave) == 0);
    slave = -1;
    CHECK(serial_read_exact_timeout(closed_fd, buffer, 8, 100, &received) == -1);
    CHECK(received == 0);
    (void)close(master);
    puts("serial exact timeout: shared deadline, split read, remaining input, timeout, EINTR and errors PASS");
    return EXIT_SUCCESS;
fail:
    if (handler_installed) {
        (void)setitimer(ITIMER_REAL, &stopped, NULL);
        (void)sigaction(SIGALRM, &previous_action, NULL);
    }
    if (writer > 0) {
        (void)kill(writer, SIGTERM);
        (void)waitpid(writer, NULL, 0);
    }
    if (slave >= 0) (void)close(slave);
    if (master >= 0) (void)close(master);
    return EXIT_FAILURE;
}
