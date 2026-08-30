#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 600
#include "real_serial_source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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


static int stop_now(void *context)
{
    return *(const int *)context;
}

int main(void)
{
    real_serial_source_t source = REAL_SERIAL_SOURCE_INITIALIZER;
    int master = -1;
    const uint8_t input[8] = {1, 3, 0, 0, 0, 2, 0xc4, 0x0b};
    uint8_t tx_copy[8];
    int stop = 1;
    CHECK(real_serial_source_read(&source, NULL, NULL) == REAL_SERIAL_SOURCE_ERROR);
    CHECK(errno == ENODEV);
    CHECK(real_serial_source_send(&source, input, 8) == -1 && errno == ENODEV);

    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    char *path = ptsname(master);
    CHECK(path != NULL);
    real_serial_source_config_t config = {
        .serial_path = path, .gpiochip_path = "mock", .line_offset = 22,
        .receive_length = 8, .timeout_ms = 40,
    };
    config.receive_length = REAL_SERIAL_SOURCE_CAPACITY + 1;
    CHECK(real_serial_source_open(&source, &config) == -1 && errno == EINVAL);
    CHECK(source.port.serial_fd == -1);
    config.receive_length = 8;
    fail_gpio_open = 1;
    CHECK(real_serial_source_open(&source, &config) == -1 && errno == EACCES);
    CHECK(source.port.serial_fd == -1 && source.port.rse.chip_fd == -1);
    fail_gpio_open = 0;
    CHECK(real_serial_source_open(&source, &config) == 0);
    int old_fd = source.port.serial_fd;
    CHECK(real_serial_source_open(&source, &config) == -1 && errno == EBUSY);
    CHECK(source.port.serial_fd == old_fd);
    config.receive_length = 1; /* 配置值已复制，不依赖调用方后续修改。 */
    CHECK(source.receive_length == 8);

    CHECK(real_serial_source_send(&source, input, 8) == 0);
    CHECK(read(master, tx_copy, 8) == 8 && memcmp(input, tx_copy, 8) == 0);
    CHECK(rx_attempts == 1);
    CHECK(write(master, input, 8) == 8);
    CHECK(real_serial_source_read(&source, NULL, NULL) == REAL_SERIAL_SOURCE_COMPLETE);
    CHECK(source.received == 8 && memcmp(source.buffer, input, 8) == 0);
    CHECK(write(master, input, 3) == 3);
    CHECK(real_serial_source_read(&source, NULL, NULL) == REAL_SERIAL_SOURCE_TIMEOUT);
    CHECK(source.received == 3 && memcmp(source.buffer, input, 3) == 0);
    CHECK(real_serial_source_read(&source, stop_now, &stop) == REAL_SERIAL_SOURCE_CANCELED);
    CHECK(errno == ECANCELED && source.received == 0);

    measurement_t output;
    memset(&output, 0xa5, sizeof(output));
    unsigned char snapshot[sizeof(output)];
    memcpy(snapshot, &output, sizeof(output));
    measurement_source_t placeholder = real_serial_source_placeholder();
    CHECK(measurement_source_next(&placeholder, &output) == MEASUREMENT_SOURCE_NO_DATA);
    CHECK(memcmp(snapshot, &output, sizeof(output)) == 0);

    real_serial_source_close(&source);
    CHECK(source.port.serial_fd == -1 && source.received == 0);
    CHECK(fcntl(old_fd, F_GETFD) == -1 && errno == EBADF);
    real_serial_source_close(&source);
    real_serial_source_close(NULL);
    config.receive_length = 8;
    CHECK(real_serial_source_open(&source, &config) == 0);
    CHECK(write(master, input, 8) == 8);
    CHECK(real_serial_source_read(&source, NULL, NULL) == REAL_SERIAL_SOURCE_COMPLETE);
    real_serial_source_close(&source);
    (void)close(master);
    puts("real serial source: config/rollback/raw TX-RX/timeout/cancel/reopen/NO_DATA PASS (PTY + GPIO mock)");
    return EXIT_SUCCESS;
fail:
    real_serial_source_close(&source);
    if (master >= 0) (void)close(master);
    return EXIT_FAILURE;
}
