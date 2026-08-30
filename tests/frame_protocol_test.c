#include "frame.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 运行方式：
 *   ./build/frame_protocol_test
 *   ctest --test-dir build -R '^frame_protocol$' --output-on-failure
 *
 * CMake 会为该目标取消 NDEBUG，确保 Release/交叉构建中 assert() 仍然执行。
 */

static void test_random_split_and_merge(void);

/**
 * @brief 协议模块测试入口，依次验证 CRC、编码、拆包、粘包和异常重同步。
 *
 * @return 所有 assert() 通过时返回 0；任何断言失败时进程立即终止并由 CTest
 *         标记为失败。
 *
 * @note 基础场景结束后调用 test_random_split_and_merge() 完成 1000 帧量化
 *       验证。
 */
int main(void)
{
    static const unsigned char payload[] = "ABC";

    unsigned char frame[FRAME_MAX_SIZE];
    unsigned char output[FRAME_MAX_PAYLOAD];
    unsigned char combined[FRAME_MAX_SIZE * 2];

    frame_parser_t parser;

    size_t frame_size = 0;
    size_t output_size = 0;

    /* CRC16-Modbus标准向量。 */
    assert(frame_crc16_modbus(
               (const unsigned char *)"123456789",
               9) == 0x4B37u);

    /* Modbus RTU：读取1号从站，从地址0开始的2个保持寄存器。 */
    {
        uint8_t request[8] = {0};
        const uint8_t expected[8] = {
            0x01, 0x03, 0x00, 0x00,
            0x00, 0x02, 0xC4, 0x0B
        };

        assert(modbus_rtu_build_read_holding(
            1, 0, 2, request, sizeof(request)) == 8);

        assert(memcmp(request, expected, sizeof(expected)) == 0);

        /* 非法数量和容量不足。 */
        assert(modbus_rtu_build_read_holding(
            1, 0, 0, request, sizeof(request)) == -1);

        assert(modbus_rtu_build_read_holding(
            1, 0, 126, request, sizeof(request)) == -1);

        assert(modbus_rtu_build_read_holding(
            1, 0, 2, request, 7) == -1);

        /* 地址边界：最后一个寄存器可读，但不能跨越地址空间。 */
        assert(modbus_rtu_build_read_holding(
            1, 0xFFFF, 1, request, sizeof(request)) == 8);

        assert(modbus_rtu_build_read_holding(
            1, 0xFFFF, 2, request, sizeof(request)) == -1);
    }

    /* Modbus正常响应、异常响应及错误帧。 */
    {
        uint8_t normal[] = {
            0x01, 0x03, 0x04, 0x00, 0x19,
            0x00, 0x32, 0xAA, 0x21
        };
        const uint8_t exception[] = {
            0x01, 0x83, 0x02, 0xC0, 0xF1
        };
        uint8_t code = 0xFF;

        /* 正常响应：清零异常码。 */
        assert(modbus_rtu_check_read_holding(
            normal, sizeof(normal), 1, 2, &code)
            == MODBUS_RESPONSE_NORMAL);
        assert(code == 0);

        /* 有效异常响应不是坏帧。 */
        assert(modbus_rtu_check_read_holding(
            exception, sizeof(exception), 1, 2, &code)
            == MODBUS_RESPONSE_EXCEPTION);
        assert(code == 0x02);

        /* 站号不匹配；不能保留上次的异常码。 */
        assert(modbus_rtu_check_read_holding(
            normal, sizeof(normal), 2, 2, &code)
            == MODBUS_RESPONSE_INVALID);
        assert(code == 0);

        /* 已确认正常响应包含2个寄存器，才允许读取数据区。 */
        uint16_t registers[2] = {0};

        for (size_t i = 0; i < 2; ++i) {
            size_t offset = 3U + i * 2U;

            registers[i] =
                ((uint16_t)normal[offset] << 8) |
                (uint16_t)normal[offset + 1];
        }

        assert(registers[0] == 25);
        assert(registers[1] == 50);

        /* 数量不匹配、截断响应。 */
        assert(modbus_rtu_check_read_holding(
            normal, sizeof(normal), 1, 1, &code)
            == MODBUS_RESPONSE_INVALID);

        assert(modbus_rtu_check_read_holding(
            normal, sizeof(normal) - 1, 1, 2, &code)
            == MODBUS_RESPONSE_INVALID);

        /* 修改数据但不更新CRC，必须拒绝。 */
        normal[3] ^= 0x01;
        assert(modbus_rtu_check_read_holding(
            normal, sizeof(normal), 1, 2, &code)
            == MODBUS_RESPONSE_INVALID);
    }

    /* 编码ABC。 */
    assert(frame_encode(payload,
                        3,
                        frame,
                        sizeof(frame),
                        &frame_size) == FRAME_READY);
    assert(frame_size == 8);

    /* 半帧：分两次feed。 */
    frame_parser_init(&parser);

    assert(frame_parser_feed(&parser,
                             frame,
                             4) == FRAME_READY);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_NEED_MORE);

    assert(frame_parser_feed(&parser,
                             frame + 4,
                             frame_size - 4) == FRAME_READY);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_READY);

    assert(output_size == 3);
    assert(memcmp(output, payload, 3) == 0);
    assert(parser.stats.frame_ok == 1);

    /* 粘包：一次feed两个完整帧。 */
    memcpy(combined, frame, frame_size);
    memcpy(combined + frame_size, frame, frame_size);

    frame_parser_init(&parser);

    assert(frame_parser_feed(&parser,
                             combined,
                             frame_size * 2) == FRAME_READY);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_READY);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_READY);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_NEED_MORE);
    assert(parser.stats.frame_ok == 2);

    /* 坏CRC帧后面紧跟正常帧。 */
    combined[3] ^= 0x01u;

    frame_parser_init(&parser);

    assert(frame_parser_feed(&parser,
                             combined,
                             frame_size * 2) == FRAME_READY);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_INVALID);

    assert(frame_parser_next(&parser,
                             output,
                             &output_size) == FRAME_READY);

    assert(output_size == 3);
    assert(memcmp(output, payload, 3) == 0);
    assert(parser.stats.bad_crc == 1);
    assert(parser.stats.frame_ok == 1);

    /* 超长LEN必须被拒绝并计数。 */
    {
        const unsigned char invalid_length[] = {
            0xA5u, 0x5Au, (unsigned char)(FRAME_MAX_PAYLOAD + 1u)};

        frame_parser_init(&parser);
        assert(frame_parser_feed(&parser,
                                 invalid_length,
                                 sizeof(invalid_length)) == FRAME_READY);
        assert(frame_parser_next(&parser,
                                 output,
                                 &output_size) == FRAME_INVALID);
        assert(parser.stats.invalid_lengths == 1);
    }

    /* MAGIC前的噪声必须丢弃并精确计数。 */
    {
        unsigned char noisy_frame[FRAME_MAX_SIZE + 2u];

        noisy_frame[0] = 0x00u;
        noisy_frame[1] = 0x11u;
        memcpy(noisy_frame + 2, frame, frame_size);
        frame_parser_init(&parser);
        assert(frame_parser_feed(&parser,
                                 noisy_frame,
                                 frame_size + 2u) == FRAME_READY);
        assert(frame_parser_next(&parser,
                                 output,
                                 &output_size) == FRAME_READY);
        assert(parser.stats.noise_bytes == 2);
        assert(parser.stats.frame_ok == 1);
    }

    /* 内部缓冲区溢出必须拒绝且保持原有数据不变。 */
    {
        unsigned char overflow[FRAME_BUFFER_CAPACITY + 1u] = {0};

        frame_parser_init(&parser);
        assert(frame_parser_feed(&parser,
                                 overflow,
                                 sizeof(overflow)) == FRAME_INVALID);
        assert(parser.size == 0);
        assert(parser.stats.buffer_overflows == 1);
    }

    test_random_split_and_merge();

    puts("frame protocol module tests passed");
    return 0;
}

#define RANDOM_FRAME_COUNT 1000u /* 随机分片/合并测试生成的帧总数。 */

/**
 * @brief 为指定帧序号生成确定性的 payload 长度。
 *
 * @param frame_index 帧序号，范围为 0 到 RANDOM_FRAME_COUNT - 1。
 * @return 0 到 FRAME_MAX_PAYLOAD 之间的长度。
 *
 * @note 使用确定性公式便于接收端不保存副本也能重建期望值。
 */
static size_t expected_payload_size(size_t frame_index)
{
    return (frame_index * 17u + 3u) % (FRAME_MAX_PAYLOAD + 1u);
}

/**
 * @brief 生成某一帧中指定位置的确定性 payload 字节。
 *
 * @param frame_index 帧序号。
 * @param byte_index payload 内的字节下标。
 * @return 对应位置的期望字节。
 *
 * @note 必须与 test_random_split_and_merge() 写入 payload 的规则保持一致。
 */
static unsigned char expected_payload_byte(size_t frame_index,
                                           size_t byte_index)
{
    return (unsigned char)((frame_index + byte_index * 31u) & 0xFFu);
}

/**
 * @brief 排空解析器中当前所有完整帧，并逐帧校验内容和顺序。
 *
 * @param parser 已初始化且已经 feed 部分字节流的解析器。
 * @param next_frame 输入下一帧期望序号，输出本次成功消费后的新序号。
 *
 * @note FRAME_NEED_MORE 是正常退出条件；出现 FRAME_INVALID 或内容不匹配会
 *       触发 assert()。调用方可在每次 feed 后调用本函数。
 */
static void drain_and_verify(frame_parser_t *parser, size_t *next_frame)
{
    unsigned char payload[FRAME_MAX_PAYLOAD];
    size_t payload_size = 0;

    for (;;)
    {
        frame_result_t result = frame_parser_next(parser,
                                                  payload,
                                                  &payload_size);
        size_t byte_index;

        if (result == FRAME_NEED_MORE)
            return;
        assert(result == FRAME_READY);
        assert(*next_frame < RANDOM_FRAME_COUNT);
        assert(payload_size == expected_payload_size(*next_frame));
        for (byte_index = 0; byte_index < payload_size; ++byte_index)
            assert(payload[byte_index] ==
                   expected_payload_byte(*next_frame, byte_index));
        ++(*next_frame);
    }
}

/**
 * @brief 验证 1000 帧经过伪随机 TCP 分段和合并后仍能无损还原。
 *
 * 先编码连续的完整帧字节流，再用固定种子的线性同余发生器生成 1 到 97
 * 字节的 feed 大小，模拟 TCP 半帧、完整帧和多帧混合到达。
 *
 * @note 固定种子 0xD32 保证每次运行可复现；函数还会校验最终统计计数和
 *       解析器暂存区是否完全排空。
 */
static void test_random_split_and_merge(void)
{
    const size_t stream_capacity = RANDOM_FRAME_COUNT * FRAME_MAX_SIZE;
    unsigned char *stream = malloc(stream_capacity);
    frame_parser_t parser;
    size_t stream_size = 0;
    size_t frame_index;
    size_t offset = 0;
    size_t next_frame = 0;
    uint32_t random_state = 0xD32u;

    assert(stream != NULL);
    for (frame_index = 0; frame_index < RANDOM_FRAME_COUNT; ++frame_index)
    {
        unsigned char payload[FRAME_MAX_PAYLOAD];
        size_t payload_size = expected_payload_size(frame_index);
        size_t encoded_size = 0;
        size_t byte_index;

        for (byte_index = 0; byte_index < payload_size; ++byte_index)
            payload[byte_index] = expected_payload_byte(frame_index,
                                                        byte_index);
        assert(frame_encode(payload,
                            payload_size,
                            stream + stream_size,
                            stream_capacity - stream_size,
                            &encoded_size) == FRAME_READY);
        stream_size += encoded_size;
    }

    frame_parser_init(&parser);
    while (offset < stream_size)
    {
        size_t chunk_size;

        random_state = random_state * 1664525u + 1013904223u;
        chunk_size = 1u + (size_t)(random_state % 97u);
        if (chunk_size > stream_size - offset)
            chunk_size = stream_size - offset;
        assert(frame_parser_feed(&parser,
                                 stream + offset,
                                 chunk_size) == FRAME_READY);
        offset += chunk_size;
        drain_and_verify(&parser, &next_frame);
    }
    drain_and_verify(&parser, &next_frame);

    assert(next_frame == RANDOM_FRAME_COUNT);
    assert(parser.size == 0);
    assert(parser.stats.frame_ok == RANDOM_FRAME_COUNT);
    assert(parser.stats.bad_crc == 0);
    assert(parser.stats.invalid_lengths == 0);
    assert(parser.stats.buffer_overflows == 0);
    free(stream);
}
