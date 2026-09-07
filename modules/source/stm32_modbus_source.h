#ifndef EDGEVISION_MODULES_SOURCE_STM32_MODBUS_SOURCE_H
#define EDGEVISION_MODULES_SOURCE_STM32_MODBUS_SOURCE_H

#include "measurement_source.h"
#include "real_serial_source.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * STM32 DHT11 Modbus 04 正式数据源配置。
 *
 * 设备路径只在 open() 期间借用；串口/GPIO 句柄由数据源独占，直到 close()。
 * should_stop 可为空；非空时用于让阻塞接收响应 SIGINT/SIGTERM。
 */
typedef struct
{
    const char *serial_path;
    const char *gpiochip_path;
    unsigned int line_offset;
    int timeout_ms;
    serial_stop_fn should_stop;
    void *stop_context;
} stm32_modbus_source_config_t;

/**
 * 数据源运行状态不可复制，也不可由多个线程并发调用。
 *
 * 一次 Modbus 响应生成两条 Measurement。next() 先返回温度，并把湿度按值
 * 缓存在 pending 中；下一次 next() 直接交付缓存，避免重复查询同一次采样。
 */
typedef struct
{
    real_serial_source_t serial;
    uint32_t next_sequence;
    measurement_t pending;
    bool pending_valid;
    serial_stop_fn should_stop;
    void *stop_context;
} stm32_modbus_source_t;

#define STM32_MODBUS_SOURCE_INITIALIZER \
    { .serial = REAL_SERIAL_SOURCE_INITIALIZER, .next_sequence = 1u }

/**
 * 打开 UART、申请 RSE GPIO，并初始化序号和双 Measurement 缓存。
 * 失败时不持有任何新资源；source 必须先使用 INITIALIZER 初始化。
 */
int stm32_modbus_source_open(
    stm32_modbus_source_t *source,
    const stm32_modbus_source_config_t *config);

/**
 * 关闭串口/GPIO，清除缓存。允许传入 NULL 或已经关闭的初始化对象。
 */
void stm32_modbus_source_close(stm32_modbus_source_t *source);

/**
 * 返回供 Gateway 使用的统一 MeasurementSource 接口。
 * 返回对象只借用 source 指针，source 必须保持存活且已经成功 open()。
 */
measurement_source_t stm32_modbus_source_as_measurement_source(
    stm32_modbus_source_t *source);

#endif
