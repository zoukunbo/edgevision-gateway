#define _DEFAULT_SOURCE 1
#define _XOPEN_SOURCE 600

#include "rs485_serial.h"
#include "real_serial_source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* 单进程基线测试使用统一失败出口，确保 PTY 两端都能被关闭。 */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #condition, errno); \
        goto fail; \
    } \
} while (0)

int main(void)
{
    int master = -1;
    int slave = -1;
    struct termios tty;
    uint8_t buffer[8] = {0};
    const uint8_t input[] = {0x01, 0x03, 0x00, 0x11, 0x13, 0xFF};
    size_t used = 0;

    /* PTY 模拟字节流串口；它不模拟 SP3485 电气方向和 GPIO 时序。 */
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(master >= 0);
    CHECK(grantpt(master) == 0);
    CHECK(unlockpt(master) == 0);
    char *name = ptsname(master);
    CHECK(name != NULL);
    slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    CHECK(slave >= 0);

    /* 先故意打开软件/硬件流控，再验证配置函数能完整清除它们。 */
    CHECK(tcgetattr(slave, &tty) == 0);
    tty.c_iflag |= IXON | IXOFF | IXANY;
    tty.c_cflag |= CRTSCTS;
    CHECK(tcsetattr(slave, TCSANOW, &tty) == 0);
    CHECK(serial_configure_115200_8n1(slave) == 0);
    CHECK(tcgetattr(slave, &tty) == 0);
    CHECK((tty.c_iflag & (IXON | IXOFF | IXANY | ICRNL | ISTRIP)) == 0);
    CHECK((tty.c_cflag & (CRTSCTS | PARENB | CSTOPB)) == 0);
    CHECK((tty.c_cflag & CSIZE) == CS8);
    CHECK((tty.c_lflag & (ICANON | ECHO | ISIG)) == 0);
    CHECK((tty.c_oflag & OPOST) == 0);
    CHECK(tty.c_cc[VMIN] == 0 && tty.c_cc[VTIME] == 0);
    CHECK(cfgetispeed(&tty) == B115200 && cfgetospeed(&tty) == B115200);
    CHECK(serial_wait_readable(slave, 20) == 0);
    CHECK(serial_read_append(slave, buffer, sizeof(buffer), &used) == 0);
    CHECK(used == 0);

    /* 分两批传递二进制字节，验证追加位置，不把一次 read 当作协议帧。 */
    CHECK(write(master, input, 2) == 2);
    CHECK(serial_wait_readable(slave, 200) == 1);
    CHECK(serial_read_append(slave, buffer, sizeof(buffer), &used) == 2);
    CHECK(write(master, input + 2, 4) == 4);
    CHECK(serial_wait_readable(slave, 200) == 1);
    CHECK(serial_read_append(slave, buffer, sizeof(buffer), &used) == 4);
    CHECK(used == sizeof(input) && memcmp(buffer, input, used) == 0);
    CHECK(serial_read_append(slave, buffer, used, &used) == -1 && errno == ENOBUFS);
    CHECK(used == sizeof(input));
    CHECK(serial_read_append(slave, NULL, sizeof(buffer), &used) == -1 && errno == EINVAL);
    CHECK(serial_wait_readable(-1, -1) == -1 && errno == EINVAL);
    CHECK(serial_write_full(slave, NULL, 1) == -1 && errno == EINVAL);
    CHECK(serial_write_full(slave, input, sizeof(input)) == 0);
    CHECK(serial_wait_readable(master, 200) == 1);
    CHECK(read(master, buffer, sizeof(buffer)) == (ssize_t)sizeof(input));
    CHECK(memcmp(buffer, input, sizeof(input)) == 0);

    /* 占位数据源必须报告无数据，并严格保持调用方的输出对象不变。 */
    measurement_source_t source = real_serial_source_placeholder();
    measurement_t output;
    unsigned char before[sizeof(output)];
    memset(&output, 0, sizeof(output));
    memcpy(before, &output, sizeof(output));
    CHECK(measurement_source_next(&source, &output) == MEASUREMENT_SOURCE_NO_DATA);
    CHECK(memcmp(before, &output, sizeof(output)) == 0);
    CHECK(measurement_source_next(&source, NULL) == MEASUREMENT_SOURCE_ERROR);

    (void)close(slave);
    (void)close(master);
    puts("serial baseline: configuration, byte IO, timeout and placeholder PASS");
    return EXIT_SUCCESS;
fail:
    if (slave >= 0) (void)close(slave);
    if (master >= 0) (void)close(master);
    return EXIT_FAILURE;
}
