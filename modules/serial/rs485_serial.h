#ifndef EDGEVISION_MODULES_SERIAL_RS485_SERIAL_H
#define EDGEVISION_MODULES_SERIAL_RS485_SERIAL_H

#include "rse_control.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 独占拥有这些FD；打开后不能按值复制，也不能由外部单独关闭其中的FD。 */
typedef struct {
    int serial_fd;
    rse_control_t rse;
} rs485_port_t;

#define RS485_PORT_INITIALIZER { \
    .serial_fd = -1,              \
    .rse = {                     \
        .chip_fd = -1,           \
        .line_fd = -1            \
    }                            \
}

/*
 * port 必须先使用 RS485_PORT_INITIALIZER 初始化。
 * 当前固定配置115200/8N1；路径仅在调用期间借用，不保存指针。
 * 成功返回0且RSE=RX；失败返回-1，释放本次资源并保留errno。
 * 重复打开返回EBUSY，原对象保持不变；先关闭才能重新打开。
 */
int rs485_port_open(rs485_port_t *port,
                    const char *serial_path,
                    const char *gpiochip_path,
                    unsigned int line_offset);

/*
 * 调用前先停止所有收发；不能和其他线程的IO并发调用。
 * 尽力恢复RX，释放全部FD并重置为-1；保留调用前errno。
 * 接受NULL和已关闭对象；不可传入未初始化对象或全零对象。
 * 这是资源清理，不保证GPIO释放后的物理电平或替代发送前的tcdrain。
 */
void rs485_port_close(rs485_port_t *port);

/**
 * 把已经打开的 TTY 配置为 115200、8N1、无流控、原始模式。
 * poll() 负责等待，因此 read() 本身配置为立即返回当前可用数据。
 */
int serial_configure_115200_8n1(int serial_fd);

/**
 * 把 length 个字节完整交给串口发送队列。
 *
 * 必须处理部分写和 EINTR；成功返回 0，失败返回 -1。
 */
int serial_write_full(int serial_fd,
                      const uint8_t *data,
                      size_t length);

/**
 * 完成一次半双工帧发送。
 *
 * 正确顺序：RSE=TX → write_full → tcdrain → RSE=RX。
 * 所有错误路径都必须尽力恢复 RX；成功返回 0，失败返回 -1。
 */
int rs485_send_frame(int serial_fd,
                     rse_control_t *rse,
                     const uint8_t *frame,
                     size_t frame_length);

/**
 * 等待串口可读。
 *
 * timeout_ms >= 0 表示毫秒预算，-1 表示调用者明确要求无限等待。
 * EINTR 返回 -1；后续循环接收器须用剩余预算重试，不能重置总超时。
 *
 * @return 1 表示 POLLIN，0 表示超时，-1 表示错误事件或调用失败。
 */
int serial_wait_readable(int serial_fd, int timeout_ms);

/**
 * 把本次 read() 得到的数据追加到 buffer 已有内容之后。
 *
 * used 指向当前已使用长度；函数必须使用 buffer + *used 和
 * capacity - *used，成功读取后增加 *used。
 *
 * @return read() 的返回值；缓冲区已满时返回 -1 并设置 errno=ENOBUFS。
 */
ssize_t serial_read_append(int serial_fd,
                           uint8_t *buffer,
                           size_t capacity,
                           size_t *used);

/**
 * 在总超时内收齐 length 个字节；buffer 必须至少有 length 字节空间。
 * 要求 length > 0、timeout_ms > 0；入口将 *received 清零。
 * 返回 1=收齐、0=超时、-1=错误；失败时保留已收到的字节及其数量。
 * fd 应已配置 raw + VMIN=0/VTIME=0，或为非阻塞fd，且只能有一个读者。
 * 不丢弃已有输入，不识别协议帧；超过 length 的输入留给后续读取。
 */
int serial_read_exact_timeout(int fd,
                              uint8_t *buffer,
                              size_t length,
                              int timeout_ms,
                              size_t *received);

/*
 * 回调在接收调用线程中执行；非0表示停止，必须快速返回。
 * context仅在本次调用期间借用；跨线程停止标志须由调用者使用原子量等同步。
 */
typedef int (*serial_stop_fn)(void *context);

/*
 * 与serial_read_exact_timeout相同，但可通过回调取消。
 * should_stop为NULL时不检查取消，stop_context被忽略。
 * 取消返回-1/ECANCELED，保留部分数据；不关闭FD。
 * 有回调时poll单次最多等待100ms（不含调度/回调耗时），总截止时间不重置。
 * 停止在循环顶部和可读后检查；检查到停止时优先于超时/新读取。
 * 已完成的读取返回成功；不支持中断发送，也不提供硬实时退出保证。
 */
int serial_read_exact_timeout_stop(int fd,
                                   uint8_t *buffer,
                                   size_t length,
                                   int timeout_ms,
                                   size_t *received,
                                   serial_stop_fn should_stop,
                                   void *stop_context);

/* 基于CLOCK_MONOTONIC生成绝对截止时间，单位为纳秒。
 * 成功返回0；失败返回-1，不修改输出。
 */
int serial_deadline_after_ms(int timeout_ms, int64_t *deadline_ns);


/* deadline_ns必须基于CLOCK_MONOTONIC，单位为纳秒。
 * 返回1=收齐、0=到期、-1=错误或取消。
 * 每次调用清零*received，它只记录本次接收的数量；错误/取消/超时保留部分数据。
 * deadline_ns >= 0；已到期时不消费输入，返回0；检测到停止时优先返回ECANCELED。
 * 使用与timeout_stop相同的raw/非阻塞FD、单读者和缓冲区容量约束。
 * 第二段应传buffer+第一段长度，并单独累计第二段的received。
 */
int serial_read_exact_until_stop(
    int fd,
    uint8_t *buffer,
    size_t length,
    int64_t deadline_ns,
    size_t *received,
    serial_stop_fn should_stop,
    void *stop_context);
#endif
