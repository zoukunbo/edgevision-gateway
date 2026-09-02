#define _DEFAULT_SOURCE 1

#include "rs485_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int serial_configure_115200_8n1(int serial_fd)
{
    struct termios tty;

    if (tcgetattr(serial_fd, &tty) < 0) {
        return -1;
    }

    tty.c_iflag &=
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
          IXON | IXOFF | IXANY | INPCK | IGNPAR);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    tty.c_cflag |= CS8 | CLOCAL | CREAD;

    /* 等待职责交给 poll()，read() 只取走当前已经到达的数据。 */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (cfsetispeed(&tty, B115200) < 0 ||
        cfsetospeed(&tty, B115200) < 0) {
        return -1;
    }

    return tcsetattr(serial_fd, TCSANOW, &tty);
}

void rs485_port_close(rs485_port_t *port)
{
    if (port == NULL) {
        return;
    }

    int saved_errno = errno;
    /* 即使恢复RX失败，也必须继续释放所有资源。 */
    if (port->rse.line_fd >= 0) {
        (void)rse_control_set_rx(&port->rse);
    }
    rse_control_close(&port->rse);
    if (port->serial_fd >= 0) {
        (void)close(port->serial_fd);
        port->serial_fd = -1;
    }
    errno = saved_errno;
}

int rs485_port_open(rs485_port_t *port,
                    const char *serial_path,
                    const char *gpiochip_path,
                    unsigned int line_offset)
{
    if (port == NULL || serial_path == NULL || gpiochip_path == NULL ||
        serial_path[0] == '\0' || gpiochip_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (port->serial_fd != -1 || port->rse.chip_fd != -1 ||
        port->rse.line_fd != -1) {
        errno = EBUSY;
        return -1;
    }

    /* 只有全部成功后才把资源所有权交给调用方。 */
    rs485_port_t pending = RS485_PORT_INITIALIZER;
    pending.serial_fd = open(serial_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (pending.serial_fd < 0) {
        return -1;
    }
    if (serial_configure_115200_8n1(pending.serial_fd) < 0 ||
        rse_control_open(&pending.rse, gpiochip_path, line_offset) < 0) {
        int saved_errno = errno;
        rs485_port_close(&pending);
        errno = saved_errno;
        return -1;
    }

    /* 所有权转移，pending离开作用域后不得再次关闭这些FD。 */
    *port = pending;
    return 0;
}

int serial_write_full(int serial_fd,
                      const uint8_t *data,
                      size_t length)
{
    if (serial_fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (data == NULL && length != 0) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    size_t written = 0;

    while(written < length)
    {
        n = write(serial_fd, data + written, length - written);

        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n == 0) {
            errno = EIO;
            return -1;
        } else {
            return -1;
        }

    }

    return 0;
}

/*
 * 等的是“本机最后一位已发送完”，不是“write已经返回”：
 * TIOCOUTQ检查软件发送队列；TEMT还必须表示FIFO和移位寄存器均为空。
 * 已核对OK1126B的8250驱动还排除DMA在途；不能外推到未核对的其他驱动。
 *
 * 2026-08-31实板对照：旧tcdrain路径在跟踪中等待约8.8ms，
 * STM32可在请求结束约1.837ms后安排回复，因此晚释放TX可能覆盖回复。
 * 改为状态轮询后，无需STM32的临时20ms延时即可读取成功；没有波形证据。
 * 50us只是再次检查的间隔，不用固定睡眠时间推断“已发完”。
 * nanosleep和GPIO ioctl均可能受调度影响，这不是硬实时方向控制。
 */
int serial_wait_tx_complete(int serial_fd, int timeout_ms)
{
    int64_t deadline, now;
    if (serial_fd < 0) { errno = EBADF; return -1; }
    if (serial_deadline_after_ms(timeout_ms, &deadline) < 0) return -1;

    for (;;) {
        struct timespec clock_now;
        if (clock_gettime(CLOCK_MONOTONIC, &clock_now) < 0) return -1;
        now = (int64_t)clock_now.tv_sec * INT64_C(1000000000) + clock_now.tv_nsec;
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }

        /* 无其他写入者是调用前提。队列空不代表移位寄存器空，必须两项同时成立。 */
        int queued = 0, status = 0;
        if (ioctl(serial_fd, TIOCOUTQ, &queued) < 0 ||
            ioctl(serial_fd, TIOCSERGETLSR, &status) < 0) {
            if (errno == EINTR) continue;
            return -1; /* 不支持时明确失败，不能把“未知”当成发送完成。 */
        }
        if (queued == 0 && (status & TIOCSER_TEMT) != 0) return 0;

        struct timespec pause = { .tv_sec = 0, .tv_nsec = 50000 };
        if (nanosleep(&pause, NULL) < 0 && errno != EINTR) return -1;
        /* EINTR也回到同一个截止时间；普通Linux调度不提供硬实时保证。 */
    }
}

int rs485_send_frame(int serial_fd,
                     rse_control_t *rse,
                     const uint8_t *frame,
                     size_t frame_length)
{
    int saved_errno;
    int tx_started = 0;

    /*
     * 发送前先验证接口能力并确认本机发送端为空；不支持的设备不会进入TX。
     * 这里不检测其他主站是否占用A/B，总线仍必须保证只有一个请求发起者。
     * 发送前100ms、write后100ms、上层接收1000ms是三个独立预算；
     * write_full仍可能阻塞，不能把它们相加称为端到端总超时。
     */
    if (serial_wait_tx_complete(serial_fd, 100) < 0) {
        saved_errno = errno;
        goto restore_after_error;
    }

    if (rse_control_set_tx(rse) < 0)
    {
        saved_errno = errno;
        goto restore_after_error;
    }

    tx_started = 1;
    if (serial_write_full(serial_fd, frame, frame_length) < 0)
    {
        saved_errno = errno;
        goto restore_after_error;
    }

    /* 不走tcdrain的节拍轮询；必须同时确认软件队列和硬件发送完成。 */
    if (serial_wait_tx_complete(serial_fd, 100) < 0)
    {
        saved_errno = errno;
        goto restore_after_error;
    }

    if (rse_control_set_rx(rse) < 0)
    {
        return -1;
    }

    return 0;

restore_after_error:
    /*
     * 失败帧不在此层重试；尽力清除本机尚未发送的队列并释放方向。
     * 已在线路上发送的字节无法撤回，对端也可能见到残帧；
     * 因而调用者不能把失败当作“完全没发送过”，更不能直接当作有效测量。
     */
    if (tx_started) (void)tcflush(serial_fd, TCOFLUSH);
    /* 保留首个失败原因，恢复 RX 失败不应覆盖真正的发送错误。 */
    (void)rse_control_set_rx(rse);
    errno = saved_errno;
    return -1;
}

int serial_wait_readable(int serial_fd, int timeout_ms)
{
    /* poll 会忽略负 fd，不能让调用错误伪装成超时或永久等待。 */
    if (serial_fd < 0 || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd descriptor = {
        .fd = serial_fd,
        .events = POLLIN,
        .revents = 0,
    };

    int result = poll(&descriptor, 1, timeout_ms);
    if (result == 0) {
        return 0;
    }

    if (result < 0) {
        return -1;
    }

    /* 错误状态优先于 POLLIN，避免把断线时残留的可读状态当成正常数据。 */
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        errno = EIO;
        return -1;
    }

    if ((descriptor.revents & POLLIN) != 0) {
        return 1;
    }

    errno = EIO;
    return -1;
}

static ssize_t serial_read_append_once(int serial_fd,
                           uint8_t *buffer,
                           size_t capacity,
                           size_t *used)
{
    if (buffer == NULL || used == NULL || *used > capacity)
    {
        errno = EINVAL;
        return  -1;
    }

    if (*used == capacity)
    {
        errno = ENOBUFS;
        return -1;
    }

    ssize_t n = read(serial_fd, buffer + *used, capacity - *used);

    if (n > 0)
    {
       *used += (size_t)n;
    }


    return n;
}

/* 保留原接口的 EINTR 重试语义，实际追加逻辑只维护一份。 */
ssize_t serial_read_append(int serial_fd,
                           uint8_t *buffer,
                           size_t capacity,
                           size_t *used)
{
    ssize_t n;
    do {
        n = serial_read_append_once(serial_fd, buffer, capacity, used);
    } while (n < 0 && errno == EINTR);
    return n;
}

static int serial_monotonic_ns(int64_t *now_ns)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    *now_ns = (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
    return 0;
}

/**
 * timeout_ms 是时长，例如1000ms。
 * deadline_ns 是固定的截止时刻，一旦生成，后续分段接收不再重新计算它。
 */
int serial_deadline_after_ms(int timeout_ms, int64_t *deadline_ns)
{
    if (timeout_ms <= 0 || deadline_ns == NULL) {
        errno = EINVAL;
        return -1;
    }

    int64_t now_ns;
    if (serial_monotonic_ns(&now_ns) < 0) {
        return -1;
    }

    int64_t duration_ns =
        (int64_t)timeout_ms * INT64_C(1000000);

    if (now_ns > INT64_MAX - duration_ns) {
        errno = EOVERFLOW;
        return -1;
    }

    *deadline_ns = now_ns + duration_ns;
    return 0;
}


/* 旧接口保持兼容；接收逻辑只维护一份。 */
int serial_read_exact_timeout(int fd,
                              uint8_t *buffer,
                              size_t length,
                              int timeout_ms,
                              size_t *received)
{
    return serial_read_exact_timeout_stop(
        fd, buffer, length, timeout_ms, received, NULL, NULL);
}

int serial_read_exact_timeout_stop(int fd,
                                   uint8_t *buffer,
                                   size_t length,
                                   int timeout_ms,
                                   size_t *received,
                                   serial_stop_fn should_stop,
                                   void *stop_context)
{
    if (received == NULL) {
        errno = EINVAL;
        return -1;
    }
    *received = 0;
    if (fd < 0 || buffer == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }

    int64_t deadline_ns;
    if (serial_deadline_after_ms(timeout_ms, &deadline_ns) < 0) {
        return -1;
    }
    return serial_read_exact_until_stop(
        fd, buffer, length, deadline_ns, received, should_stop, stop_context);
}

int serial_read_exact_until_stop(int fd,
                                 uint8_t *buffer,
                                 size_t length,
                                 int64_t deadline_ns,
                                 size_t *received,
                                 serial_stop_fn should_stop,
                                 void *stop_context)
{
    if (received == NULL) {
        errno = EINVAL;
        return -1;
    }
    *received = 0;
    if (fd < 0 || buffer == NULL || length == 0 || deadline_ns < 0) {
        errno = EINVAL;
        return -1;
    }

    int64_t now_ns;
    /* 截止时间由调用者提供，多次分段接收可共用同一个总预算。 */
    while (*received < length) {
        if (should_stop != NULL && should_stop(stop_context)) {
            errno = ECANCELED;
            return -1;
        }
        if (serial_monotonic_ns(&now_ns) < 0) {
            return -1;
        }
        if (now_ns >= deadline_ns) {
            return 0;
        }
        const int64_t remaining_ns = deadline_ns - now_ns;
        /* 向上取整但不直接加999999，避免极大截止时间溢出。 */
        const int64_t remaining_ms =
            remaining_ns / INT64_C(1000000) +
            (remaining_ns % INT64_C(1000000) != 0);
        /* poll参数为int；分片不改变总截止时间。 */
        int wait_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
        if (should_stop != NULL && wait_ms > 100) {
            wait_ms = 100;
        }
        int ready = serial_wait_readable(fd, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ready == 0) {
            /* 重新检查统一的截止时间，而不是重新开始计时。 */
            continue;
        }

        if (should_stop != NULL && should_stop(stop_context)) {
            errno = ECANCELED;
            return -1;
        }

        /* 本次预算已耗尽时，不再开始新的读取。 */
        if (serial_monotonic_ns(&now_ns) < 0) {
            return -1;
        }
        if (now_ns >= deadline_ns) {
            return 0;
        }

        /* 复用追加逻辑，但 EINTR 交回外层以重新检查 deadline。 */
        ssize_t n = serial_read_append_once(fd, buffer, length, received);
        if (n < 0 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
            return -1;
        }
        /* 正数：累计；0或可重试错误：回到 poll，不把空读当成成功。 */
    }
    return 1;
}
