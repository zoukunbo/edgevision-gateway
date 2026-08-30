#include "real_serial_source.h"
#include "graceful_shutdown.h"


#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* OK1126B-S P16 接口的 UART5 与 SP3485 RSE 控制参数。 */
#define RS485_SERIAL_DEVICE "/dev/ttyS5"
#define RS485_GPIOCHIP_DEVICE "/dev/gpiochip0"
#define RS485_RSE_LINE_OFFSET 22U

/* 串口模块不依赖全局退出机制，由demo负责适配。 */
static int demo_should_stop(void *context)
{
    (void)context;
    return graceful_shutdown_requested();
}

int main(void)
{
    real_serial_source_t source = REAL_SERIAL_SOURCE_INITIALIZER;
    const real_serial_source_config_t config = {
        .serial_path = RS485_SERIAL_DEVICE,
        .gpiochip_path = RS485_GPIOCHIP_DEVICE,
        .line_offset = RS485_RSE_LINE_OFFSET,
        .receive_length = 8,
        .timeout_ms = 10000,
    };

    if (graceful_shutdown_install() < 0) {
        perror("graceful_shutdown_install");
        return EXIT_FAILURE;
    }

    if (real_serial_source_open(&source, &config) < 0) {
        perror("real_serial_source_open");
        return EXIT_FAILURE;
    }

    puts("RS485 initialized: /dev/ttyS5, RSE=RX");

    /* 仅验证原始字节收发和方向切换，不赋予这些字节任何业务协议语义。 */
    static const uint8_t test_frame[] = {
        0x11, 0x22, 0x33, 0x44, 0x55
    };

    int exit_code = EXIT_SUCCESS;

    if (graceful_shutdown_requested()) {
        goto cleanup;
    }

    if (real_serial_source_send(&source, test_frame, sizeof(test_frame)) < 0) {
        perror("real_serial_source_send");
        exit_code = EXIT_FAILURE;
    } else {
        puts("RS485 TX complete");
    }


    /* 总预算内收齐8字节；这里只验证定长收齐，不校验协议语义。 */
    if (exit_code == EXIT_SUCCESS)
    {
        puts("Waiting for 8 bytes, total timeout=10s... (Ctrl+C to stop)");
        fflush(stdout);

        real_serial_source_result_t result =
            real_serial_source_read(&source, demo_should_stop, NULL);

        if (result == REAL_SERIAL_SOURCE_CANCELED) {
            printf("RX canceled: received %zu/8 bytes\n", source.received);
        } else if (result < 0) {
            perror("real_serial_source_read");
            exit_code = EXIT_FAILURE;
        } else if (result == 0) {
            printf("RX timeout: received %zu/8 bytes\n", source.received);
            exit_code = EXIT_FAILURE;
        } else {
            puts("RX complete");
        }

        printf("RX %zu bytes:", source.received);
        for (size_t i = 0; i < source.received; ++i) {
            printf(" %02X", (unsigned int)source.buffer[i]);
        }
        putchar('\n');
    }

cleanup:
    real_serial_source_close(&source);
    puts("RS485 closed");
    return exit_code;
}
