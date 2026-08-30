#ifndef EDGEVISION_MODULES_SOURCE_REAL_SERIAL_SOURCE_H
#define EDGEVISION_MODULES_SOURCE_REAL_SERIAL_SOURCE_H

#include "measurement_source.h"
#include "rs485_serial.h"

/** 单次原始串口接收允许配置的最大字节数。 */
#define REAL_SERIAL_SOURCE_CAPACITY 256U

/**
 * 真实串口数据源的打开配置。
 *
 * 串口当前固定为 115200/8N1。两个设备路径只在 open 调用期间借用，数据源
 * 不保存其指针；其余配置值会复制到运行时对象。
 */
typedef struct {
    const char *serial_path;   /* TTY 设备路径，例如 /dev/ttyS5。 */
    const char *gpiochip_path; /* GPIO 字符设备路径，例如 /dev/gpiochip0。 */
    unsigned int line_offset;  /* gpiochip 内用于 RSE 的 line 偏移。 */
    size_t receive_length;     /* 每次 read 期望收齐的原始字节数，范围 1..256。 */
    int timeout_ms;            /* 单次 read 的总超时，必须大于 0。 */
} real_serial_source_config_t;

/** 一次原始串口读取的结果；取消与普通 I/O 错误使用不同枚举值。 */
typedef enum {
    REAL_SERIAL_SOURCE_ERROR = -1,
    REAL_SERIAL_SOURCE_CANCELED = -2,
    REAL_SERIAL_SOURCE_TIMEOUT = 0,
    REAL_SERIAL_SOURCE_COMPLETE = 1
} real_serial_source_result_t;

/*
 * 单一调用者拥有port和buffer，不可复制已打开的对象，不允许并发收发/关闭。
 * buffer[0..received)是本次原始字节，不是Measurement或已验证的协议帧。
 * 内容有效至下一次read/open/close；使用者若要长期保留应自行复制。
 */
typedef struct {
    rs485_port_t port;
    uint8_t buffer[REAL_SERIAL_SOURCE_CAPACITY];
    size_t received;
    size_t receive_length;
    int timeout_ms;
} real_serial_source_t;

#define REAL_SERIAL_SOURCE_INITIALIZER { .port = RS485_PORT_INITIALIZER }

/**
 * 打开串口和 RSE GPIO，并复制定长接收配置。
 *
 * source 必须先使用 REAL_SERIAL_SOURCE_INITIALIZER 初始化。失败时回滚本次
 * 获取的资源；重复打开返回 -1 并设置 errno=EBUSY，原对象保持不变。
 */
int real_serial_source_open(real_serial_source_t *source,
                            const real_serial_source_config_t *config);

/**
 * 尽力恢复接收方向，关闭数据源拥有的全部 FD，并清空运行时接收状态。
 *
 * 调用前须停止并发收发；接受 NULL 和已经关闭的已初始化对象。
 */
void real_serial_source_close(real_serial_source_t *source);

/**
 * 发送一段原始测试数据；不生成 Modbus 查询、不自动重试，也不支持取消。
 *
 * @return 完成 TX/write/tcdrain/RX 方向切换返回 0，失败返回 -1 并设置 errno。
 */
int real_serial_source_send(real_serial_source_t *source,
                            const uint8_t *data, size_t length);

/*
 * 每次调用从received=0开始定长接收；超时/取消/错误时保留本次部分数据。
 * 回调及context仅调用期间借用，取消时errno=ECANCELED；不自动关闭资源。
 * 再次调用是新一次原始读取，不自动拼接上次超时数据或重发请求。
 */
real_serial_source_result_t real_serial_source_read(
    real_serial_source_t *source, serial_stop_fn should_stop, void *stop_context);

/*
 * Measurement占位接口保持兼容：始终NO_DATA，不打开设备、不改写output。
 * D38添加协议语义之前，不将原始串口数据接入默认Gateway的Measurement主链。
 */
measurement_source_t real_serial_source_placeholder(void);

#endif
