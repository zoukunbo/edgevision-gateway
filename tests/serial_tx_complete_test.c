/*
 * rs485_send_frame的纯主机状态机测试。链接器替换write/ioctl/tcflush，
 * GPIO方向控制也由本文件提供替身，因此无需串口硬件；它验证调用顺序、
 * 超时和清理语义，但不能证明目标板驱动、电气方向或总线波形正确。
 */
#define _DEFAULT_SOURCE 1
#include "rs485_serial.h"
#include <errno.h>
#include <linux/serial.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #c, errno); return 1; } } while (0)
enum { FD = 123 };
/* 每个开关描述一种驱动/方向故障；计数器用于断言外部可观察的调用顺序。 */
static int unsupported, fail_after_write, always_busy, busy_after_write, fail_rx, short_writes;
static int early_rx;
static int tx_calls, rx_calls, write_calls, flush_calls, written, sample, complete;
static unsigned char captured[8];
static void reset(void)
{
    unsupported=fail_after_write=always_busy=busy_after_write=fail_rx=short_writes=early_rx=0;
    tx_calls=rx_calls=write_calls=flush_calls=written=sample=complete=0;
    memset(captured, 0, sizeof(captured));
}
/* GPIO替身在切回RX时记录是否早于模拟硬件发送完成。 */
int rse_control_open(rse_control_t *c, const char *p, unsigned int n)
{ (void)c; (void)p; (void)n; errno=ENOTSUP; return -1; }
void rse_control_close(rse_control_t *c) { (void)c; }
int rse_control_set_tx(rse_control_t *c) { (void)c; ++tx_calls; return 0; }
int rse_control_set_rx(rse_control_t *c)
{
    (void)c; ++rx_calls;
    if (written && !complete) early_rx=1;
    if (fail_rx) { errno=EACCES; return -1; }
    return 0;
}
ssize_t __real_write(int fd, const void *p, size_t n);
ssize_t __wrap_write(int fd, const void *p, size_t n)
{
    if (fd != FD) return __real_write(fd,p,n);
    ++write_calls;
    if (short_writes && write_calls == 1) { errno=EINTR; return -1; }
    if (short_writes && write_calls == 2 && n > 3) n=3;
    if (n > sizeof(captured)-(size_t)written) { errno=EOVERFLOW; return -1; }
    memcpy(captured+written,p,n); written+=(int)n;
    return (ssize_t)n;
}
int __wrap_tcflush(int fd, int selector)
{ (void)fd; if (selector != TCOFLUSH) { errno=EINVAL; return -1; } ++flush_calls; return 0; }
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    /* sample让软件队列空与TEMT就绪出现在不同轮次，覆盖“两者都要满足”。 */
    va_list ap; va_start(ap,request); int *out=va_arg(ap,int *); va_end(ap);
    if (fd != FD) { errno=EBADF; return -1; }
    if (unsupported) { errno=ENOTTY; return -1; }
    if (fail_after_write && written) { errno=EIO; return -1; }
    if (request == TIOCOUTQ) {
        if (written) ++sample;
        *out=always_busy || (written && (busy_after_write || sample == 1)) ? 8 : 0;
        return 0;
    }
    if (request == TIOCSERGETLSR) {
        /* 第一轮硬件空但软件有待发送数据；第二轮软件空但硬件仍发送。 */
        *out=always_busy || (written && (busy_after_write || sample == 2)) ? 0 : TIOCSER_TEMT;
        if (written && sample >= 3 && !always_busy && !busy_after_write) complete=1;
        return 0;
    }
    errno=EINVAL; return -1;
}
int main(void)
{
    /* 依次覆盖正常短写、能力不支持、发送后错误、超时、RX恢复失败和参数校验。 */
    const uint8_t frame[]={1,4,0,1,0,2,0x20,0x0b};
    rse_control_t rse={.chip_fd=-1,.line_fd=-1};
    reset(); short_writes=1;
    CHECK(rs485_send_frame(FD,&rse,frame,sizeof(frame)) == 0);
    CHECK(tx_calls==1 && rx_calls==1 && complete && sample>=3 && !early_rx);
    CHECK(written==8 && write_calls==3 && memcmp(captured,frame,8)==0 && flush_calls==0);

    reset(); unsupported=1;
    CHECK(rs485_send_frame(FD,&rse,frame,8)==-1 && errno==ENOTTY);
    CHECK(tx_calls==0 && written==0 && rx_calls==1 && flush_calls==0);

    reset(); fail_after_write=1; fail_rx=1;
    CHECK(rs485_send_frame(FD,&rse,frame,8)==-1 && errno==EIO);
    CHECK(tx_calls==1 && rx_calls==1 && flush_calls==1);

    reset(); busy_after_write=1;
    CHECK(rs485_send_frame(FD,&rse,frame,8)==-1 && errno==ETIMEDOUT);
    CHECK(tx_calls==1 && rx_calls==1 && flush_calls==1 && !complete);

    reset(); fail_rx=1;
    CHECK(rs485_send_frame(FD,&rse,frame,8)==-1 && errno==EACCES);
    CHECK(complete && rx_calls==1);

    reset(); always_busy=1;
    CHECK(serial_wait_tx_complete(FD,5)==-1 && errno==ETIMEDOUT);
    CHECK(tx_calls==0 && written==0);
    CHECK(serial_wait_tx_complete(-1,5)==-1 && errno==EBADF);
    CHECK(serial_wait_tx_complete(FD,0)==-1 && errno==EINVAL);
    puts("TX completion: software queue + hardware TEMT, short writes, unsupported-before-TX, error cleanup, timeout PASS (mock; no electrical proof)");
    return 0;
}
