#include "frame.h"

#include <string.h>

#define FRAME_MAGIC_SIZE  2u
#define FRAME_HEADER_SIZE 3u
#define FRAME_CRC_SIZE    2u
#define FRAME_MAGIC_0     0xA5u
#define FRAME_MAGIC_1     0x5Au



/**
 * @brief 校验一帧末尾的 CRC16-Modbus。
 *
 * @param buffer 已经确认长度完整的候选帧起始地址。
 * @param frame_size 候选帧总字节数，至少包含 MAGIC、LEN 和 CRC。
 * @return CRC 一致时返回 FRAME_READY，否则返回 FRAME_INVALID。
 *
 * @note 本函数只负责比较，不修改 buffer；调用前必须先由 frame_check_size()
 *       确认 frame_size 有效，CRC 覆盖范围是 LEN + PAYLOAD。
 */
static frame_result_t frame_check_crc(const unsigned char *buffer, size_t frame_size)
{
    size_t crc_offset;
    size_t crc_data_size;
    uint16_t calculated_crc;
    uint16_t received_crc;

    /* 1. 计算CRC字段的起始下标 */
    crc_offset = frame_size - FRAME_CRC_SIZE;
    /* 2. 计算LEN + PAYLOAD的长度，并调用frame_crc16_modbus()
       CRC数据起点是buffer + 2 */
    crc_data_size = frame_size - FRAME_CRC_SIZE - FRAME_HEADER_SIZE + 1;
    calculated_crc = frame_crc16_modbus(buffer + 2, crc_data_size);
    /* 3. 按低字节在前的规则还原received_crc */
    received_crc = (uint16_t)buffer[crc_offset] | ((uint16_t)buffer[crc_offset + 1] << 8);
    /* 4. 两个CRC不同则返回FRAME_INVALID */
    if (calculated_crc != received_crc)
    {
        return FRAME_INVALID;
    }


    return FRAME_READY;
}



/**
 * @brief 在暂存字节中查找帧 MAGIC 0xA5 0x5A。
 *
 * @param buffer 待搜索的暂存区。
 * @param buffer_size 暂存区有效字节数。
 * @param discard_size 输出 MAGIC 前可安全丢弃的字节数。
 * @return 找到完整 MAGIC 返回 1；未找到返回 0。
 *
 * @note 如果最后一个字节恰好是 0xA5，会保留它等待下一次 feed 的 0x5A。
 */
static int frame_find_magic(const unsigned char *buffer,
                            size_t buffer_size,
                            size_t *discard_size)
{
    *discard_size = 0;

    if (buffer_size == 0)
        return 0;

    for (size_t i = 0; i + 1 < buffer_size; ++i)
    {
        if (buffer[i] == FRAME_MAGIC_0 &&
            buffer[i + 1] == FRAME_MAGIC_1)
        {
            /* MAGIC 前的字节都是噪声，可以统一丢弃。 */
            *discard_size = i;
            return 1;
        }
    }

    if (buffer[buffer_size - 1] == FRAME_MAGIC_0)
    {
        /* 保留可能属于下一次 MAGIC 的末尾 0xA5。 */
        *discard_size = buffer_size - 1;
    }
    else
    {
        /* 没有 MAGIC 前缀，全部字节都可视为噪声。 */
        *discard_size = buffer_size;
    }

    return 0;
}


/**
 * @brief 根据 LEN 字段判断候选帧是否完整且长度合法。
 *
 * @param buffer 从 MAGIC 开始的候选帧。
 * @param buffer_size 当前可用字节数。
 * @param frame_size 输出根据 LEN 计算出的完整帧总字节数。
 * @return FRAME_READY 表示完整；FRAME_NEED_MORE 表示数据不足；
 *         FRAME_INVALID 表示 LEN 超过 FRAME_MAX_PAYLOAD。
 *
 * @note 长度上限在计算和复制 payload 前检查，用于阻止超长帧造成越界。
 */
static frame_result_t frame_check_size(const unsigned char *buffer,
                                       size_t buffer_size,
                                       size_t *frame_size)
{
    size_t payload_size;

    /* 1. 不足以读取LEN时，返回FRAME_NEED_MORE */
    if (buffer_size < FRAME_HEADER_SIZE)
    {
        return FRAME_NEED_MORE;
    }

    /* 2. LEN位于buffer[2]，读取payload_size */
    payload_size = (size_t) buffer[2];
    /* 3. payload_size超过FRAME_MAX_PAYLOAD时，返回FRAME_INVALID */
    if (payload_size > FRAME_MAX_PAYLOAD)
    {
        return FRAME_INVALID;
    }

    /* 4. 计算完整frame_size */
    *frame_size = FRAME_HEADER_SIZE + payload_size + FRAME_CRC_SIZE;

    /* 5. 数据尚未收完整时，返回FRAME_NEED_MORE */

    if (buffer_size < *frame_size)
    {
        return FRAME_NEED_MORE;
    }


    return FRAME_READY;
}

/**
 * @brief 从暂存区头部删除指定字节，并保持剩余数据连续。
 *
 * @param buffer 解析器内部暂存区。
 * @param buffer_size 输入当前有效字节数，输出删除后的有效字节数。
 * @param discard_size 要删除的前缀字节数，必须不大于 *buffer_size。
 *
 * @note 使用 memmove() 是因为源区和目标区会重叠；删除 0 字节时不改变数据。
 */
static void frame_discard_prefix(unsigned char *buffer,
                                 size_t *buffer_size,
                                 size_t discard_size)
{
    size_t remaining;

    /* 1. 计算剩余字节数 */
    remaining = *buffer_size - discard_size;

    /* 2. 使用memmove把剩余字节移到开头 */
    memmove(buffer, buffer + discard_size, remaining);

    /* 3. 更新buffer_size */
    *buffer_size = remaining;
}

/**
 * @brief 清空暂存区大小和全部统计计数。
 *
 * @param parser 调用方持有的解析器实例。
 * @note 公共使用约束和调用时机见 frame.h。
 */
void frame_parser_init(frame_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

/**
 * @brief 把新收到的 TCP 字节追加到解析器尾部。
 *
 * @param parser 已初始化的解析器。
 * @param bytes 本次输入字节。
 * @param bytes_size 本次输入长度。
 * @return 追加成功返回 FRAME_READY；空间不足返回 FRAME_INVALID 并累计
 *         buffer_overflows。
 *
 * @note 本函数不解析数据；调用方随后应循环调用 frame_parser_next()。
 */
frame_result_t frame_parser_feed(frame_parser_t *parser,
                                          const unsigned char *bytes,
                                          size_t bytes_size)
{
    /* 1. 判断剩余空间是否足够 */
    if (bytes_size > FRAME_BUFFER_CAPACITY - parser->size) {
        ++parser->stats.buffer_overflows;
        return FRAME_INVALID;
    }

    /* 2. 将新字节复制到buffer有效数据的末尾 */
    memcpy(parser->buffer + parser->size, bytes, bytes_size);

    /* 3. 更新parser->size */
    parser->size += bytes_size;

    return FRAME_READY;
}



/**
 * @brief 逐字节计算 CRC16-Modbus，初值为 0xFFFF，多项式为 0xA001。
 *
 * @param data 输入字节序列。
 * @param size 输入字节数。
 * @return 计算出的 16 位 CRC 数值。
 *
 * @note 这里只计算数值；写入线路时由 frame_encode() 按低字节在前存放。
 */
uint16_t frame_crc16_modbus(const unsigned char *data,size_t size)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < size; ++i)
    {
        /* 1. 将当前字节异或进crc */
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit)
        {
            /* 2. 判断crc最低位 */
            if ((crc & 1u) != 0u)
            {
                /* 3. 最低位为1：右移后异或0xA001 */
                crc = (crc >> 1) ^ 0xA001u;
            }
            else
            {
                /* 4. 最低位为0：只右移 */
                crc = crc >> 1;
            }
        }
    }

    return crc;
}


/**
 * @brief 从解析器暂存区恢复一个完整 payload。
 *
 * @param parser 已初始化并通过 frame_parser_feed() 接收过数据的解析器。
 * @param payload 输出 payload 的缓冲区，至少 FRAME_MAX_PAYLOAD 字节。
 * @param payload_size 输出实际 payload 长度。
 * @return FRAME_READY 表示输出成功；FRAME_NEED_MORE 表示数据不足；
 *         FRAME_INVALID 表示本次候选帧无效且已开始重同步。
 *
 * @note 调用流程依次为：搜索 MAGIC、检查 LEN、检查 CRC、复制 payload。
 *       一次 feed 可能产生多个 FRAME_READY，调用方必须持续排空。
 */
frame_result_t frame_parser_next(frame_parser_t *parser,
                                        unsigned char *payload,
                                        size_t *payload_size)
{
    size_t discard_size = 0;
    size_t frame_size = 0;
    frame_result_t result;

    /*
     * 1. 调用frame_find_magic()
     *
     * 没找到：
     *   根据discard_size清理确定无用的噪声
     *   返回FRAME_NEED_MORE
     *
     * 找到：
     *   丢弃MAGIC前面的噪声
     */
    result = frame_find_magic(parser->buffer, parser->size,  &discard_size);

    parser->stats.noise_bytes += discard_size;

    frame_discard_prefix(parser->buffer, &parser->size, discard_size);

    if (result == 0)
    {
        return FRAME_NEED_MORE;
    }

    /*
     * 2. 调用frame_check_size()
     *
     * FRAME_NEED_MORE：
     *   保留数据并直接返回
     *
     * FRAME_INVALID：
     *   丢弃1字节并返回FRAME_INVALID
     */
    result = frame_check_size(parser->buffer,parser->size, &frame_size);
    if (result == FRAME_INVALID)
    {
        ++parser->stats.invalid_lengths;
        frame_discard_prefix(parser->buffer, &parser->size, 1);
        return FRAME_INVALID;
    }
    else if (result == FRAME_NEED_MORE)
    {
        return 0;
    }

    /*
     * 3. 调用frame_check_crc()
     *
     * CRC错误：
     *   丢弃1字节并返回FRAME_INVALID
     */
    result = frame_check_crc(parser->buffer, frame_size);
    if (result == FRAME_INVALID)
    {
        ++parser->stats.bad_crc;
        frame_discard_prefix(parser->buffer, &parser->size, 1);
        return  FRAME_INVALID;
    }

    /*
     * 4. CRC正确：
     *   payload长度位于buffer[2]
     *   payload起点是buffer + FRAME_HEADER_SIZE
     *   复制payload memcpy(目标, 源, 长度);
     *   删除整个frame_size
     *   返回FRAME_READY
     */
    *payload_size = parser->buffer[2];
    memcpy(payload, parser->buffer + FRAME_HEADER_SIZE, *payload_size);
    ++parser->stats.frame_ok;
    frame_discard_prefix(parser->buffer, &parser->size, frame_size);
    return FRAME_READY;
}



/**
 * @brief 生成 MAGIC + LEN + PAYLOAD + CRC16 格式的完整帧。
 *
 * @param payload 输入业务数据。
 * @param payload_size 输入业务数据字节数。
 * @param output 输出完整帧的缓冲区。
 * @param output_capacity 输出缓冲区容量。
 * @param output_size 输出最终完整帧字节数。
 * @return 编码成功返回 FRAME_READY；payload 超长或 output 容量不足返回
 *         FRAME_INVALID。
 *
 * @note CRC 覆盖 LEN + PAYLOAD，输出末尾依次写 CRC 低字节和高字节。
 */
frame_result_t frame_encode(const unsigned char *payload,
                                   size_t payload_size,
                                   unsigned char *output,
                                   size_t output_capacity,
                                   size_t *output_size)
{
    size_t frame_size;
    uint16_t crc;

    /* 1. 检查payload_size没有超过FRAME_MAX_PAYLOAD */
    if (payload_size > FRAME_MAX_PAYLOAD)
    {
        return FRAME_INVALID;
    }

    frame_size = FRAME_HEADER_SIZE + payload_size + FRAME_CRC_SIZE;

    /* 2. 计算frame_size并检查output_capacity */
    if (frame_size > output_capacity)
    {
        return FRAME_INVALID;
    }

    *output_size = frame_size;

    /* 3. 写入MAGIC、LEN和PAYLOAD */
    output[0] = FRAME_MAGIC_0;
    output[1] = FRAME_MAGIC_1;
    output[2] = payload_size;
    memcpy(output + FRAME_HEADER_SIZE, payload, payload_size);

    /* 4. 对LEN + PAYLOAD计算CRC */
    crc = frame_crc16_modbus(output + 2, payload_size + 1);

    /* 5. 按低字节在前写入CRC，并设置output_size */
    output[frame_size - 2] = (uint8_t)crc;
    output[frame_size - 1] = (uint8_t)(crc >> 8);

    return FRAME_READY;
}