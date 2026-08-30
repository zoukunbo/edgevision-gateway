#ifndef EDGEVISION_MODULES_SERIAL_RSE_CONTROL_H
#define EDGEVISION_MODULES_SERIAL_RSE_CONTROL_H

/**
 * SP3485 半双工方向控制器。
 *
 * chip_fd 持有 gpiochip 设备；line_fd 持有被独占请求的 GPIO line。
 * 初始化成功后，GPIO 默认输出低电平，使 SP3485 处于接收模式。
 * 调用者必须先把两个 fd 初始化为 -1；已打开对象须先 close 再 open。
 */
typedef struct {
    int chip_fd;
    int line_fd;
} rse_control_t;

/**
 * 打开 gpiochip 并独占请求一个输出 line。
 *
 * OK1126B-S 当前验证参数：
 *     gpiochip_path = "/dev/gpiochip0"
 *     line_offset   = 22（P16 Pin 7 / GPIO0_C6）
 *
 * @return 成功返回 0，失败返回 -1 并保留 errno。
 */
int rse_control_open(rse_control_t *control,
                     const char *gpiochip_path,
                     unsigned int line_offset);

/** 将 RSE 设置为高电平，使能 SP3485 发送。 */
int rse_control_set_tx(rse_control_t *control);

/** 将 RSE 设置为低电平，使能 SP3485 接收。 */
int rse_control_set_rx(rse_control_t *control);

/**
 * 关闭 line 和 gpiochip 句柄。
 *
 * 可接收 NULL、两个 fd 均初始化为 -1 的对象或已经关闭的对象。
 * 不可传入未初始化的栈内存或 {0, 0}（0 是有效 FD）。
 * 释放 line 后不再保证物理电平；退出前恢复 RX 与硬件默认偏置分别负责。
 */
void rse_control_close(rse_control_t *control);

#endif
