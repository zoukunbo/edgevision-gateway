#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define FRAME_HEADER_SIZE  3u  /* MAGIC 2字节 + LEN 1字节 */
#define FRAME_CRC_SIZE     2u  /* CRC16 */
#define FRAME_MAX_PAYLOAD 64u
#define FRAME_MAGIC_0 0xA5u
#define FRAME_MAGIC_1 0x5Au
#define FRAME_BUFFER_CAPACITY 2048u

typedef struct
{
    unsigned char buffer[FRAME_BUFFER_CAPACITY];
    size_t size;
} frame_parser_t;

typedef enum
{
    FRAME_INVALID = -1,
    FRAME_NEED_MORE = 0,
    FRAME_READY = 1
}frame_result_t;

static uint16_t frame_crc16_modbus(const unsigned char *data,size_t size)
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

static frame_result_t frame_check_crc(const unsigned char *buffer,
                                      size_t frame_size)
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

static frame_result_t frame_parser_append(frame_parser_t *parser,
                                          const unsigned char *bytes,
                                          size_t bytes_size)
{
    /* 1. 判断剩余空间是否足够 */
    if (bytes_size > FRAME_BUFFER_CAPACITY - parser->size) {
        return FRAME_INVALID;
    }

    /* 2. 将新字节复制到buffer有效数据的末尾 */
    memcpy(parser->buffer + parser->size, bytes, bytes_size);

    /* 3. 更新parser->size */
    parser->size += bytes_size;

    return FRAME_READY;
}

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
            /* TODO 1：记录MAGIC前可丢弃的字节数 */
            *discard_size = i;
            /* TODO 2：立即返回“已经找到” */
            return 1;
        }
    }

    if (buffer[buffer_size - 1] == FRAME_MAGIC_0)
    {
        /* TODO 3：只保留末尾这个A5 */
        *discard_size = buffer_size - 1;
    }
    else
    {
        /* TODO 4：全部字节都可以丢弃 */
        *discard_size = buffer_size;
    }

    return 0;
}


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

static frame_result_t frame_parser_next(frame_parser_t *parser,
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
    frame_discard_prefix(parser->buffer, &parser->size, frame_size);
    return FRAME_READY;
}

static frame_result_t frame_encode(const unsigned char *payload,
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

#ifdef TCP_FRAME_SELF_TEST

#include <assert.h>
#include <stdio.h>

int main(void)
{
    size_t discard_size = 0;
    size_t frame_size = 0;

    const unsigned char partial[] = {
    0xA5, 0x5A, 0x03, 'A', 'B'
    };
    assert(frame_check_size(partial, sizeof(partial), &frame_size)
        == FRAME_NEED_MORE);
    assert(frame_size == 8);

    const unsigned char complete[] = {
        0xA5, 0x5A, 0x02, 'X', 'Y', 0xAA, 0xBB
    };
    assert(frame_check_size(complete, sizeof(complete), &frame_size)
        == FRAME_READY);
    assert(frame_size == 7);

    const unsigned char oversized[] = {
        0xA5, 0x5A, FRAME_MAX_PAYLOAD + 1
    };
    assert(frame_check_size(oversized, sizeof(oversized), &frame_size)
        == FRAME_INVALID);

    puts("frame size and magic tests passed");

    const unsigned char magic_at_start[] = {
        0xA5, 0x5A, 0x03
    };
    assert(frame_find_magic(magic_at_start,
                            sizeof(magic_at_start),
                            &discard_size) == 1);
    assert(discard_size == 0);

    const unsigned char noise_before_magic[] = {
        0x00, 0xFF, 0xA5, 0x5A, 0x03
    };
    assert(frame_find_magic(noise_before_magic,
                            sizeof(noise_before_magic),
                            &discard_size) == 1);
    assert(discard_size == 2);

    const unsigned char trailing_half_magic[] = {
        0x11, 0xA5, 0x22, 0xA5
    };
    assert(frame_find_magic(trailing_half_magic,
                            sizeof(trailing_half_magic),
                            &discard_size) == 0);
    assert(discard_size == 3);

    const unsigned char only_noise[] = {
        0x11, 0x22, 0x33
    };
    assert(frame_find_magic(only_noise,
                            sizeof(only_noise),
                            &discard_size) == 0);
    assert(discard_size == 3);


    unsigned char discard_test[] = {
    0x00, 0xFF, 0xA5, 0x5A, 0x03, 'A', 'B', 'C'
    };
    const unsigned char expected_after_discard[] = {
        0xA5, 0x5A, 0x03, 'A', 'B', 'C'
    };
    size_t discard_test_size = sizeof(discard_test);

    frame_discard_prefix(discard_test, &discard_test_size, 2);

    assert(discard_test_size == sizeof(expected_after_discard));
    assert(memcmp(discard_test,
                expected_after_discard,
                sizeof(expected_after_discard)) == 0);
    

    frame_parser_t parser = {0};

    const unsigned char append_part_1[] = {
        0xA5, 0x5A, 0x03, 'A'
    };
    const unsigned char append_part_2[] = {
        'B', 'C', 0x12, 0x34
    };
    const unsigned char expected_appended[] = {
        0xA5, 0x5A, 0x03, 'A', 'B', 'C', 0x12, 0x34
    };

    assert(frame_parser_append(&parser,
                            append_part_1,
                            sizeof(append_part_1)) == FRAME_READY);
    assert(frame_parser_append(&parser,
                            append_part_2,
                            sizeof(append_part_2)) == FRAME_READY);
    assert(parser.size == sizeof(expected_appended));
    assert(memcmp(parser.buffer,
                expected_appended,
                sizeof(expected_appended)) == 0);

    /* 缓冲区已满时，再追加1字节必须被拒绝。 */
    parser.size = FRAME_BUFFER_CAPACITY;

    const unsigned char extra_byte = 0xFF;

    assert(frame_parser_append(&parser,
                            &extra_byte,
                            1) == FRAME_INVALID);
    assert(parser.size == FRAME_BUFFER_CAPACITY);

    puts("parser append tests passed");

    puts("all frame parser helper tests passed");

    const unsigned char crc_test[] = "123456789";

    assert(frame_crc16_modbus(crc_test,
                            sizeof(crc_test) - 1) == 0x4B37u);

    puts("CRC16-Modbus test passed");

    unsigned char crc_frame[] = {
    0xA5, 0x5A, 0x03, 'A', 'B', 'C', 0x00, 0x00
    };

    uint16_t crc_value =
        frame_crc16_modbus(crc_frame + 2, 4);

    /* 按低字节在前写入帧尾。 */
    crc_frame[6] = (unsigned char)(crc_value & 0xFFu);
    crc_frame[7] = (unsigned char)(crc_value >> 8);

    assert(frame_check_crc(crc_frame, sizeof(crc_frame))
        == FRAME_READY);

    /* 修改payload但保留旧CRC，必须被拒绝。 */
    crc_frame[3] ^= 0x01u;

    assert(frame_check_crc(crc_frame, sizeof(crc_frame))
        == FRAME_INVALID);

    puts("frame CRC check tests passed");

    /* 恢复前面被故意破坏的payload，使旧CRC重新有效。 */
    crc_frame[3] ^= 0x01u;

    frame_parser_t next_parser = {0};
    unsigned char output_payload[FRAME_MAX_PAYLOAD] = {0};
    size_t output_payload_size = 0;

    assert(frame_parser_append(&next_parser,
                            crc_frame,
                            sizeof(crc_frame)) == FRAME_READY);

    assert(frame_parser_next(&next_parser,
                            output_payload,
                            &output_payload_size) == FRAME_READY);

    assert(output_payload_size == 3);
    assert(memcmp(output_payload, "ABC", 3) == 0);
    assert(next_parser.size == 0);

    puts("frame_parser_next normal test passed");

    unsigned char bad_crc_frame[sizeof(crc_frame)];
    unsigned char combined_frames[sizeof(crc_frame) * 2];

    memcpy(bad_crc_frame, crc_frame, sizeof(crc_frame));

    /* 破坏payload，但保留原CRC。 */
    bad_crc_frame[3] ^= 0x01u;

    /* 坏帧后面紧跟正常帧。 */
    memcpy(combined_frames,
        bad_crc_frame,
        sizeof(bad_crc_frame));
    memcpy(combined_frames + sizeof(bad_crc_frame),
        crc_frame,
        sizeof(crc_frame));

    frame_parser_t resync_parser = {0};
    memset(output_payload, 0, sizeof(output_payload));
    output_payload_size = 0;

    assert(frame_parser_append(&resync_parser,
                            combined_frames,
                            sizeof(combined_frames)) == FRAME_READY);

    /* 第一次发现坏CRC，只丢弃一个字节。 */
    assert(frame_parser_next(&resync_parser,
                            output_payload,
                            &output_payload_size) == FRAME_INVALID);

    /* 第二次重新寻找MAGIC，应恢复后面的正常帧。 */
    assert(frame_parser_next(&resync_parser,
                            output_payload,
                            &output_payload_size) == FRAME_READY);

    assert(output_payload_size == 3);
    assert(memcmp(output_payload, "ABC", 3) == 0);
    assert(resync_parser.size == 0);

    puts("bad CRC resynchronization test passed");

    /* 场景1：一个完整帧被拆成两次到达。 */
    frame_parser_t split_parser = {0};

    assert(frame_parser_append(&split_parser,
                            crc_frame,
                            4) == FRAME_READY);

    assert(frame_parser_next(&split_parser,
                            output_payload,
                            &output_payload_size) == FRAME_NEED_MORE);

    /* 半帧必须完整保留。 */
    assert(split_parser.size == 4);

    assert(frame_parser_append(&split_parser,
                            crc_frame + 4,
                            sizeof(crc_frame) - 4) == FRAME_READY);

    assert(frame_parser_next(&split_parser,
                            output_payload,
                            &output_payload_size) == FRAME_READY);

    assert(output_payload_size == 3);
    assert(memcmp(output_payload, "ABC", 3) == 0);
    assert(split_parser.size == 0);

    puts("split frame test passed");

    /* 场景2：两个完整帧一次到达。 */
    frame_parser_t sticky_parser = {0};
    unsigned char two_valid_frames[sizeof(crc_frame) * 2];

    memcpy(two_valid_frames,
        crc_frame,
        sizeof(crc_frame));
    memcpy(two_valid_frames + sizeof(crc_frame),
        crc_frame,
        sizeof(crc_frame));

    assert(frame_parser_append(&sticky_parser,
                            two_valid_frames,
                            sizeof(two_valid_frames)) == FRAME_READY);

    /* 第一次只取第一帧，第二帧仍保留在暂存区。 */
    assert(frame_parser_next(&sticky_parser,
                            output_payload,
                            &output_payload_size) == FRAME_READY);
    assert(memcmp(output_payload, "ABC", 3) == 0);
    assert(sticky_parser.size == sizeof(crc_frame));

    /* 再调用一次，取出第二帧。 */
    assert(frame_parser_next(&sticky_parser,
                            output_payload,
                            &output_payload_size) == FRAME_READY);
    assert(memcmp(output_payload, "ABC", 3) == 0);
    assert(sticky_parser.size == 0);

    puts("split and sticky frame tests passed");

    unsigned char encoded_frame[
        FRAME_HEADER_SIZE + FRAME_MAX_PAYLOAD + FRAME_CRC_SIZE
    ] = {0};
    size_t encoded_size = 0;

    assert(frame_encode((const unsigned char *)"ABC",
                        3,
                        encoded_frame,
                        sizeof(encoded_frame),
                        &encoded_size) == FRAME_READY);

    assert(encoded_size == 8);
    assert(encoded_frame[0] == FRAME_MAGIC_0);
    assert(encoded_frame[1] == FRAME_MAGIC_1);
    assert(encoded_frame[2] == 3);

    frame_parser_t roundtrip_parser = {0};
    memset(output_payload, 0, sizeof(output_payload));
    output_payload_size = 0;

    assert(frame_parser_append(&roundtrip_parser,
                            encoded_frame,
                            encoded_size) == FRAME_READY);

    assert(frame_parser_next(&roundtrip_parser,
                            output_payload,
                            &output_payload_size) == FRAME_READY);

    assert(output_payload_size == 3);
    assert(memcmp(output_payload, "ABC", 3) == 0);
    assert(roundtrip_parser.size == 0);

    puts("frame encode/decode roundtrip passed");
    return 0;
}

#endif