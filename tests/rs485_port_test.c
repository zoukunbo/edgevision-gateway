#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 600

#include "rs485_serial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
        goto fail; \
    } \
} while (0)

/* 仅用于本测试：不访问实际GPIO，不能据此声称电气/时序验证通过。 */
static int fail_gpio_open;
static int fail_rx;
static int rx_attempts;
static int gpio_opens;

int rse_control_open(rse_control_t *control, const char *path,
                     unsigned int offset)
{
    (void)path;
    (void)offset;
    gpio_opens++;
    control->chip_fd = -1;
    control->line_fd = -1;
    if (fail_gpio_open) {
        errno = EACCES;
        return -1;
    }
    control->chip_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (control->chip_fd < 0) return -1;
    control->line_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (control->line_fd < 0) {
        int saved = errno;
        (void)close(control->chip_fd);
        control->chip_fd = -1;
        errno = saved;
        return -1;
    }
    return 0;
}

int rse_control_set_rx(rse_control_t *control)
{
    (void)control;
    rx_attempts++;
    if (fail_rx) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int rse_control_set_tx(rse_control_t *control)
{
    (void)control;
    return 0;
}

void rse_control_close(rse_control_t *control)
{
    if (control->line_fd >= 0) (void)close(control->line_fd);
    if (control->chip_fd >= 0) (void)close(control->chip_fd);
    control->line_fd = -1;
    control->chip_fd = -1;
}

static int fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (dir == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') count++;
    }
    (void)closedir(dir);
    return count;
}

static int is_closed(const rs485_port_t *port)
{
    return port->serial_fd == -1 &&
        port->rse.chip_fd == -1 && port->rse.line_fd == -1;
}

static int stop_now(void *context)
{
    (void)context;
    return 1;
}

int main(void)
{
    rs485_port_t port = RS485_PORT_INITIALIZER;
    int master = -1;
    int reused = -1;
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char *path = ptsname(master);
    CHECK(path != NULL);
    const int baseline = fd_count();
    CHECK(baseline >= 0);

    CHECK(rs485_port_open(NULL, path, "mock", 22) == -1 && errno == EINVAL);
    CHECK(rs485_port_open(&port, NULL, "mock", 22) == -1 && errno == EINVAL);
    CHECK(rs485_port_open(&port, "/dev/null/no-such-tty", "mock", 22) == -1);
    CHECK(is_closed(&port) && fd_count() == baseline);
    CHECK(rs485_port_open(&port, "/dev/null", "mock", 22) == -1 && errno == ENOTTY);
    CHECK(gpio_opens == 0 && is_closed(&port) && fd_count() == baseline);

    fail_gpio_open = 1;
    CHECK(rs485_port_open(&port, path, "mock", 22) == -1 && errno == EACCES);
    CHECK(is_closed(&port) && fd_count() == baseline);
    fail_gpio_open = 0;

    CHECK(rs485_port_open(&port, path, "mock", 22) == 0);
    CHECK(fd_count() == baseline + 3);
    int serial_fd = port.serial_fd;
    int chip_fd = port.rse.chip_fd;
    int line_fd = port.rse.line_fd;
    CHECK((fcntl(serial_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(rs485_port_open(&port, path, "mock", 22) == -1 && errno == EBUSY);
    CHECK(port.serial_fd == serial_fd && port.rse.chip_fd == chip_fd &&
          port.rse.line_fd == line_fd && fd_count() == baseline + 3);

    uint8_t buffer[8];
    size_t received = 99;
    CHECK(serial_read_exact_timeout_stop(port.serial_fd, buffer, sizeof(buffer),
          10000, &received, stop_now, NULL) == -1);
    CHECK(errno == ECANCELED && received == 0);
    CHECK(fd_count() == baseline + 3);

    fail_rx = 1;
    errno = ERANGE;
    rs485_port_close(&port);
    CHECK(errno == ERANGE && rx_attempts == 1);
    CHECK(is_closed(&port) && fd_count() == baseline);
    CHECK(fcntl(serial_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(chip_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(line_fd, F_GETFD) == -1 && errno == EBADF);

    /* 旧编号被另一个文件复用后，重复close也不能误关它。 */
    reused = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(reused >= 0);
    rs485_port_close(&port);
    rs485_port_close(NULL);
    CHECK(fcntl(reused, F_GETFD) >= 0 && rx_attempts == 1);
    (void)close(reused);
    reused = -1;

    fail_rx = 0;
    CHECK(rs485_port_open(&port, path, "mock", 22) == 0);
    rs485_port_close(&port);
    CHECK(is_closed(&port) && fd_count() == baseline && rx_attempts == 2);
    (void)close(master);
    puts("rs485 port: open/rollback/busy/close/reopen/fd baseline PASS (GPIO mock)");
    return EXIT_SUCCESS;
fail:
    rs485_port_close(&port);
    if (reused >= 0) (void)close(reused);
    if (master >= 0) (void)close(master);
    return EXIT_FAILURE;
}
