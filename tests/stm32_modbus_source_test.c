#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 600

#include "stm32_modbus_source.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", \
                __LINE__, #condition, errno); \
        goto fail; \
    } \
} while (0)

/* PTY 没有真实 UART 移位寄存器；仅替代 TEMT 查询，不声称覆盖电气时序。 */
int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list arguments;
    va_start(arguments, request);
    void *argument = va_arg(arguments, void *);
    va_end(arguments);
    if (request == TIOCSERGETLSR)
    {
        *(int *)argument = TIOCSER_TEMT;
        return 0;
    }
    return __real_ioctl(fd, request, argument);
}

/* 单元测试不访问主机 GPIO；真实 GPIO22 已由独立板端测试覆盖。 */
int rse_control_open(rse_control_t *control,
                     const char *path,
                     unsigned int offset)
{
    (void)path;
    (void)offset;
    control->chip_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    control->line_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    return control->chip_fd >= 0 && control->line_fd >= 0 ? 0 : -1;
}

int rse_control_set_rx(rse_control_t *control)
{
    (void)control;
    return 0;
}

int rse_control_set_tx(rse_control_t *control)
{
    (void)control;
    return 0;
}

void rse_control_close(rse_control_t *control)
{
    if (control->line_fd >= 0)
        (void)close(control->line_fd);
    if (control->chip_fd >= 0)
        (void)close(control->chip_fd);
    control->line_fd = -1;
    control->chip_fd = -1;
}

typedef struct
{
    int master_fd;
    int result;
} responder_context_t;

static int read_exact(int fd, uint8_t *buffer, size_t length)
{
    size_t received = 0;
    while (received < length)
    {
        ssize_t count = read(fd, buffer + received, length - received);
        if (count > 0)
            received += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 0;
}

static void *respond_once(void *argument)
{
    static const uint8_t expected_request[8] = {
        0x01, 0x04, 0x00, 0x01, 0x00, 0x02, 0x20, 0x0B
    };
    static const uint8_t response[9] = {
        0x01, 0x04, 0x04, 0x01, 0x46, 0x01, 0x5E, 0x9B, 0xC5
    };
    responder_context_t *context = argument;
    uint8_t request[sizeof(expected_request)];

    if (read_exact(context->master_fd, request, sizeof(request)) != 0 ||
        memcmp(request, expected_request, sizeof(request)) != 0 ||
        write(context->master_fd, response, sizeof(response)) !=
            (ssize_t)sizeof(response))
    {
        context->result = -1;
    }
    return NULL;
}

int main(void)
{
    stm32_modbus_source_t stm32 = STM32_MODBUS_SOURCE_INITIALIZER;
    int master_fd = -1;
    pthread_t responder;
    int responder_started = 0;

    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master_fd >= 0);
    CHECK(grantpt(master_fd) == 0 && unlockpt(master_fd) == 0);
    char *slave_path = ptsname(master_fd);
    CHECK(slave_path != NULL);

    const stm32_modbus_source_config_t config = {
        .serial_path = slave_path,
        .gpiochip_path = "mock",
        .line_offset = 22u,
        .timeout_ms = 500,
        .should_stop = NULL,
        .stop_context = NULL
    };
    CHECK(stm32_modbus_source_open(&stm32, &config) == 0);

    responder_context_t context = {.master_fd = master_fd, .result = 0};
    CHECK(pthread_create(&responder, NULL, respond_once, &context) == 0);
    responder_started = 1;

    measurement_source_t source =
        stm32_modbus_source_as_measurement_source(&stm32);
    measurement_t temperature;
    measurement_t humidity;
    CHECK(measurement_source_next(&source, &temperature) ==
          MEASUREMENT_SOURCE_OK);
    CHECK(measurement_source_next(&source, &humidity) ==
          MEASUREMENT_SOURCE_OK);
    CHECK(pthread_join(responder, NULL) == 0);
    responder_started = 0;
    CHECK(context.result == 0);

    CHECK(temperature.sequence == 1u && humidity.sequence == 2u);
    CHECK(temperature.timestamp_ms == humidity.timestamp_ms);
    CHECK(strcmp(temperature.metric, "temperature") == 0);
    CHECK(strcmp(humidity.metric, "humidity") == 0);
    CHECK(temperature.value == 32.6 && humidity.value == 35.0);
    CHECK(temperature.quality == MEASUREMENT_QUALITY_UNCERTAIN);
    CHECK(humidity.quality == MEASUREMENT_QUALITY_UNCERTAIN);

    stm32_modbus_source_close(&stm32);
    CHECK(stm32.serial.port.serial_fd == -1 && !stm32.pending_valid);
    (void)close(master_fd);
    puts("stm32 modbus source: one request -> two cached Measurements PASS");
    return EXIT_SUCCESS;

fail:
    stm32_modbus_source_close(&stm32);
    if (responder_started)
        (void)pthread_join(responder, NULL);
    if (master_fd >= 0)
        (void)close(master_fd);
    return EXIT_FAILURE;
}
