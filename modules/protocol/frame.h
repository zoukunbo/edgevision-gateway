#ifndef PROTOCOL_FRAME_H
#define PROTOCOL_FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * 自定义 TCP 帧格式：
 *   MAGIC(0xA5 0x5A) + LEN(1 byte) + PAYLOAD(LEN bytes) + CRC16(2 bytes)
 *
 * 接收端典型调用顺序：
 *   1. 为每条 TCP 连接保存一个 frame_parser_t，并调用 frame_parser_init()。
 *   2. 每次 recv() 后调用 frame_parser_feed() 追加字节。
 *   3. 循环调用 frame_parser_next()，直到返回 FRAME_NEED_MORE。
 *
 * 发送端调用 frame_encode() 得到完整帧，再交给 net_send_all() 等“完整发送”
 * 函数。CRC16 使用 Modbus 算法，帧尾按低字节、高字节存放。
 */

#define FRAME_MAX_PAYLOAD 240u      /* 单帧 payload 最大字节数，可容纳 Measurement JSON。 */
#define FRAME_BUFFER_CAPACITY 2048u /* 单连接解析器的暂存容量。 */
#define FRAME_MAX_SIZE \
    (2u + 1u + FRAME_MAX_PAYLOAD + 2u) /* 最大完整帧字节数。 */

/** 解析器累计统计；frame_parser_init() 会把所有字段清零。 */
typedef struct
{
    size_t frame_ok;          /* 成功输出的帧数。 */
    size_t bad_crc;           /* CRC 错误帧数。 */
    size_t invalid_lengths;   /* 长度超过上限的帧数。 */
    size_t noise_bytes;       /* 寻找 MAGIC 时丢弃的噪声字节数。 */
    size_t buffer_overflows;  /* 追加数据超过内部缓冲区容量的次数。 */
} frame_parser_stats_t;

/** 公共 API 的三态返回值。 */
typedef enum
{
    FRAME_INVALID = -1, /* 当前候选帧或输入无效；解析器已执行必要重同步。 */
    FRAME_NEED_MORE = 0, /* 暂存数据不足，等待下一次 feed。 */
    FRAME_READY = 1      /* feed/encode 成功，或 next 已输出一个完整 payload。 */
} frame_result_t;

/** 每条 TCP 连接应独占一个实例，不可由多个线程无锁共享。 */
typedef struct
{
    unsigned char buffer[FRAME_BUFFER_CAPACITY]; /* 尚未解析完的字节。 */
    size_t size;                                 /* buffer 中有效字节数。 */
    frame_parser_stats_t stats;                  /* 该连接的累计统计。 */
} frame_parser_t;

/**
 * @brief 初始化或重置增量帧解析器。
 *
 * @param parser 调用方分配的解析器实例，不能为空。
 *
 * @note 新连接建立后必须先调用一次；重新调用会丢弃未完成帧和全部统计。
 */
void frame_parser_init(frame_parser_t *parser);

/**
 * @brief 把一次 recv() 返回的字节追加到解析器暂存区。
 *
 * @param parser 已初始化的解析器，不能为空。
 * @param bytes 输入字节起始地址；bytes_size 大于 0 时不能为空。
 * @param bytes_size 本次追加的字节数，可以是任意 TCP 分段大小。
 * @return FRAME_READY 表示追加成功；FRAME_INVALID 表示剩余容量不足，此时原有
 *         暂存数据保持不变，buffer_overflows 加 1。
 *
 * @note feed 只追加数据，不代表已经得到完整帧；之后必须调用
 *       frame_parser_next()。
 */
frame_result_t frame_parser_feed(
    frame_parser_t *parser,
    const unsigned char *bytes,
    size_t bytes_size);

/**
 * @brief 从暂存区尝试提取下一个通过长度和 CRC 校验的 payload。
 *
 * @param parser 已初始化且可能已经 feed 数据的解析器。
 * @param payload 输出缓冲区，调用方必须提供至少 FRAME_MAX_PAYLOAD 字节。
 * @param payload_size 输出实际 payload 字节数，不能为空。
 * @return FRAME_READY 表示成功输出一帧；FRAME_NEED_MORE 表示需要继续 recv/feed；
 *         FRAME_INVALID 表示发现坏长度或坏 CRC，解析器已丢弃一个字节以便重同步。
 *
 * @note 一次 feed 可能包含多帧。FRAME_READY 后应继续调用本函数，直到返回
 *       FRAME_NEED_MORE。FRAME_INVALID 后也可立即再次调用以寻找后续正常帧。
 */
frame_result_t frame_parser_next(
    frame_parser_t *parser,
    unsigned char *payload,
    size_t *payload_size);

/**
 * @brief 把业务 payload 编码成可直接发送的完整协议帧。
 *
 * @param payload 输入业务数据；payload_size 大于 0 时不能为空。
 * @param payload_size 输入字节数，不能超过 FRAME_MAX_PAYLOAD。
 * @param output 输出帧缓冲区，不能为空。
 * @param output_capacity output 的可用字节数，至少要容纳本次完整帧。
 * @param output_size 输出完整帧字节数，不能为空。
 * @return FRAME_READY 表示编码成功；FRAME_INVALID 表示 payload 超长或输出容量
 *         不足，调用方不应发送 output。
 *
 * @note 成功后通常调用 net_send_all(fd, output, *output_size)。
 */
frame_result_t frame_encode(
    const unsigned char *payload,
    size_t payload_size,
    unsigned char *output,
    size_t output_capacity,
    size_t *output_size);

/**
 * @brief 计算 Modbus 多项式的 CRC16。
 *
 * @param data 待校验字节序列；size 大于 0 时不能为空。
 * @param size data 的字节数。
 * @return 16 位 CRC 数值；标准向量 "123456789" 的结果为 0x4B37。
 *
 * @note 本函数只返回数值，不负责字节序。自定义帧和 Modbus RTU 在线路上都
 *       使用低字节在前；D38 可直接复用此算法。
 */
uint16_t frame_crc16_modbus(
    const unsigned char *data,
    size_t size);

#endif
