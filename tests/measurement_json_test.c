#include "measurement_json.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * measurement_from_json()的验收测试，覆盖三个核心要求：
 * 1. 合法JSON能够完整转换成measurement_t；
 * 2. 非法JSON返回对应错误，而且不能修改调用者的output；
 * 3. JSON数字转换到uint32_t、int64_t和double时正确处理边界。
 * 4. measurement_t能够完整编码成JSON，且调用者可以安全释放结果。
 */

/* 测试JSON都很短，固定缓冲区可以避免在测试代码中引入动态内存。 */
#define TEST_JSON_CAPACITY 1024u

/*
 * 根据各字段的“原始JSON表示”构造一条完整测量数据。
 *
 * 参数不是普通C字符串值，而是可以直接放进JSON的文本。例如：
 *   device_id传入"\"meter-01\""，生成JSON字符串"meter-01"；
 *   sequence传入"42"，生成JSON数字42；
 *   device_id传入"123"，可以专门构造类型错误的测试数据。
 *
 * 返回生成的JSON字节数；格式化失败或缓冲区不足时返回-1。
 */
static int build_json(
    char *json,
    size_t json_capacity,
    const char *device_id,
    const char *metric,
    const char *unit,
    const char *schema_version,
    const char *sequence,
    const char *timestamp_ms,
    const char *value,
    const char *quality)
{
    int written = snprintf(
        json,
        json_capacity,
        "{\"device_id\":%s,\"metric\":%s,\"unit\":%s,"
        "\"schema_version\":%s,\"sequence\":%s,"
        "\"timestamp_ms\":%s,\"value\":%s,\"quality\":%s}",
        device_id,
        metric,
        unit,
        schema_version,
        sequence,
        timestamp_ms,
        value,
        quality);

    if (written < 0 || (size_t)written >= json_capacity)
    {
        fprintf(stderr, "failed to build test JSON\n");
        return -1;
    }

    return written;
}

static int expect_error_without_output_change(
    const char *case_name,
    const char *json,
    size_t json_size,
    measurement_json_result_t expected)
{
    measurement_t output;
    unsigned char output_before[sizeof(output)];

    /*
     * 使用明显的0xA5填充整个结构体，包括可能存在的填充字节；
     * 随后保存原始字节，解析结束后用memcmp检查每一个字节。
     * 如果错误路径提前写过output，即使只改了一个字段，也会被发现。
     */
    memset(&output, 0xA5, sizeof(output));
    memcpy(output_before, &output, sizeof(output));

    /* 调用被测方法，并检查错误分类是否与用例预期一致。 */
    measurement_json_result_t actual =
        measurement_from_json(json, json_size, &output);

    if (actual != expected)
    {
        fprintf(
            stderr,
            "%s failed: actual result=%d expected=%d\n",
            case_name,
            actual,
            expected);
        return -1;
    }

    /* 错误输入必须满足事务性：失败时output保持调用前的状态。 */
    if (memcmp(output_before, &output, sizeof(output)) != 0)
    {
        fprintf(stderr, "%s failed: invalid input changed output\n", case_name);
        return -1;
    }

    return 0;
}

static int test_normal_decode(void)
{
    /*
     * 使用表驱动方式验证三个可读字符串都映射到正确的枚举值，
     * 其余字段保持相同，避免复制三份几乎一致的测试代码。
     */
    static const struct
    {
        const char *json_text;
        measurement_quality_t expected;
    } quality_cases[] = {
        {"\"good\"", MEASUREMENT_QUALITY_GOOD},
        {"\"uncertain\"", MEASUREMENT_QUALITY_UNCERTAIN},
        {"\"bad\"", MEASUREMENT_QUALITY_BAD}
    };

    for (size_t i = 0; i < sizeof(quality_cases) / sizeof(quality_cases[0]); ++i)
    {
        char json[TEST_JSON_CAPACITY];

        /* 构造一条所有字段均合法的完整Measurement JSON。 */
        int json_size = build_json(
            json,
            sizeof(json),
            "\"power-meter-01\"",
            "\"voltage\"",
            "\"volt\"",
            "1",
            "42",
            "1787623200123",
            "220.6",
            quality_cases[i].json_text);

        if (json_size < 0)
        {
            return -1;
        }

        measurement_t output = {0};
        measurement_json_result_t result =
            measurement_from_json(json, (size_t)json_size, &output);

        /* 不只检查返回值，还逐字段检查解析和枚举映射结果。 */
        if (result != MEASUREMENT_JSON_OK ||
            output.schema_version != MEASUREMENT_SCHEMA_VERSION ||
            strcmp(output.device_id, "power-meter-01") != 0 ||
            output.sequence != UINT32_C(42) ||
            output.timestamp_ms != INT64_C(1787623200123) ||
            strcmp(output.metric, "voltage") != 0 ||
            output.value != 220.6 ||
            strcmp(output.unit, "volt") != 0 ||
            output.quality != quality_cases[i].expected)
        {
            fprintf(stderr, "normal decode case %zu failed\n", i);
            return -1;
        }
    }

    puts("normal decode tests passed");
    return 0;
}

static int test_invalid_input_does_not_change_output(void)
{
    /* 参数层错误：JSON地址为空。 */
    if (expect_error_without_output_change(
            "NULL JSON input",
            NULL,
            1,
            MEASUREMENT_JSON_INVALID_ARGUMENT) != 0)
    {
        return -1;
    }

    /* 参数层错误：有输入地址，但声明的输入长度为0。 */
    const char nonempty_json[] = "{}";
    if (expect_error_without_output_change(
            "zero JSON size",
            nonempty_json,
            0,
            MEASUREMENT_JSON_INVALID_ARGUMENT) != 0)
    {
        return -1;
    }

    /* output本身为空时没有可检查的输出对象，只检查错误码。 */
    if (measurement_from_json(
            nonempty_json,
            strlen(nonempty_json),
            NULL) != MEASUREMENT_JSON_INVALID_ARGUMENT)
    {
        fprintf(stderr, "NULL output argument test failed\n");
        return -1;
    }

    /* JSON语法不完整，解析器应在字段检查之前失败。 */
    const char malformed_json[] = "{\"device_id\":";
    if (expect_error_without_output_change(
            "malformed JSON",
            malformed_json,
            strlen(malformed_json),
            MEASUREMENT_JSON_PARSE_ERROR) != 0)
    {
        return -1;
    }

    /* 合法JSON数组不是Measurement要求的JSON对象。 */
    const char array_root[] = "[]";
    if (expect_error_without_output_change(
            "non-object root",
            array_root,
            strlen(array_root),
            MEASUREMENT_JSON_ROOT_NOT_OBJECT) != 0)
    {
        return -1;
    }

    /* 根对象合法，但第一个必填字段device_id不存在。 */
    const char missing_fields[] = "{}";
    if (expect_error_without_output_change(
            "missing required field",
            missing_fields,
            strlen(missing_fields),
            MEASUREMENT_JSON_MISSING_FIELD) != 0)
    {
        return -1;
    }

    /*
     * 以下用例共享一套合法的metric、unit、timestamp_ms和value，
     * 每一行只替换需要制造错误的字段以及对应预期错误码。
     */
    static const struct
    {
        const char *name;
        const char *device_id;
        const char *schema_version;
        const char *sequence;
        const char *quality;
        measurement_json_result_t expected;
    } cases[] = {
        {
            "wrong device_id type",
            "123",
            "1",
            "1",
            "\"good\"",
            MEASUREMENT_JSON_WRONG_TYPE
        },
        {
            "oversized device_id",
            "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"",
            "1",
            "1",
            "\"good\"",
            MEASUREMENT_JSON_OUT_OF_RANGE
        },
        {
            "unsupported schema",
            "\"meter-01\"",
            "2",
            "1",
            "\"good\"",
            MEASUREMENT_JSON_UNSUPPORTED_SCHEMA
        },
        {
            "invalid sequence",
            "\"meter-01\"",
            "1",
            "0",
            "\"good\"",
            MEASUREMENT_JSON_INVALID_MEASUREMENT
        },
        {
            "wrong quality type",
            "\"meter-01\"",
            "1",
            "1",
            "0",
            MEASUREMENT_JSON_WRONG_TYPE
        },
        {
            "unknown quality text",
            "\"meter-01\"",
            "1",
            "1",
            "\"excellent\"",
            MEASUREMENT_JSON_INVALID_MEASUREMENT
        }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        char json[TEST_JSON_CAPACITY];

        /* 从表中取出原始JSON片段，构造当前错误场景。 */
        int json_size = build_json(
            json,
            sizeof(json),
            cases[i].device_id,
            "\"temperature\"",
            "\"celsius\"",
            cases[i].schema_version,
            cases[i].sequence,
            "1787623200123",
            "12.5",
            cases[i].quality);

        if (json_size < 0 ||
            expect_error_without_output_change(
                cases[i].name,
                json,
                (size_t)json_size,
                cases[i].expected) != 0)
        {
            return -1;
        }
    }

    puts("invalid-input output-preservation tests passed");
    return 0;
}

static int expect_numeric_success(
    const char *case_name,
    const char *sequence,
    const char *timestamp_ms,
    const char *value,
    uint32_t expected_sequence,
    int64_t expected_timestamp,
    double expected_value)
{
    /*
     * 数字成功用例的公共入口：只替换三个数字字段，其他字段保持合法，
     * 成功后检查转换得到的C类型数值是否与预期完全一致。
     */
    char json[TEST_JSON_CAPACITY];
    int json_size = build_json(
        json,
        sizeof(json),
        "\"meter-01\"",
        "\"temperature\"",
        "\"celsius\"",
        "1",
        sequence,
        timestamp_ms,
        value,
        "\"good\"");

    if (json_size < 0)
    {
        return -1;
    }

    measurement_t output = {0};
    measurement_json_result_t actual =
        measurement_from_json(json, (size_t)json_size, &output);

    if (actual != MEASUREMENT_JSON_OK ||
        output.sequence != expected_sequence ||
        output.timestamp_ms != expected_timestamp ||
        output.value != expected_value)
    {
        fprintf(stderr, "%s failed\n", case_name);
        return -1;
    }

    return 0;
}

static int expect_numeric_error(
    const char *case_name,
    const char *sequence,
    const char *timestamp_ms,
    const char *value)
{
    /*
     * 数字失败用例的公共入口。除了检查错误码，还复用哨兵检查，
     * 保证数字越界或转换失败不会把部分字段写入output。
     */
    char json[TEST_JSON_CAPACITY];
    int json_size = build_json(
        json,
        sizeof(json),
        "\"meter-01\"",
        "\"temperature\"",
        "\"celsius\"",
        "1",
        sequence,
        timestamp_ms,
        value,
        "\"good\"");

    if (json_size < 0)
    {
        return -1;
    }

    return expect_error_without_output_change(
        case_name,
        json,
        (size_t)json_size,
        MEASUREMENT_JSON_INVALID_MEASUREMENT);
}

static int test_numeric_conversion_boundaries(void)
{
    /* uint32_t和正时间戳的最小合法值，value的0值也是合法测量值。 */
    if (expect_numeric_success(
            "numeric lower boundaries",
            "1",
            "1",
            "0",
            UINT32_C(1),
            INT64_C(1),
            0.0) != 0)
    {
        return -1;
    }

    /* 2^63-1024是double在INT64_MAX附近可以精确表示的最大整数。 */
    if (expect_numeric_success(
            "numeric upper representable boundaries",
            "4294967295",
            "9223372036854774784",
            "1.7976931348623157e308",
            UINT32_MAX,
            INT64_C(9223372036854774784),
            DBL_MAX) != 0)
    {
        return -1;
    }

    /*
     * 表中依次覆盖下界越界、上界越界、整数目标收到小数，
     * 以及strtod把过大的value转换成正负无穷大的情况。
     */
    static const struct
    {
        const char *name;
        const char *sequence;
        const char *timestamp_ms;
        const char *value;
    } invalid_cases[] = {
        {"sequence below minimum", "0", "1", "0"},
        {"sequence above UINT32_MAX", "4294967296", "1", "0"},
        {"fractional sequence", "1.5", "1", "0"},
        {"timestamp below minimum", "1", "0", "0"},
        {"fractional timestamp", "1", "1.5", "0"},
        /* INT64_MAX会在cJSON的double表示中舍入到2^63，不能安全转换。 */
        {"timestamp not exactly representable", "1", "9223372036854775807", "0"},
        {"timestamp above INT64_MAX", "1", "9223372036854775808", "0"},
        {"value positive overflow", "1", "1", "1e309"},
        {"value negative overflow", "1", "1", "-1e309"}
    };

    for (size_t i = 0;
         i < sizeof(invalid_cases) / sizeof(invalid_cases[0]);
         ++i)
    {
        /* 每个失败用例都同时验收错误码和output不变。 */
        if (expect_numeric_error(
                invalid_cases[i].name,
                invalid_cases[i].sequence,
                invalid_cases[i].timestamp_ms,
                invalid_cases[i].value) != 0)
        {
            return -1;
        }
    }

    puts("numeric conversion boundary tests passed");
    return 0;
}

static int test_normal_encode_and_round_trip(void)
{
    measurement_t input = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "power-meter-01",
        .sequence = UINT32_C(42),
        .timestamp_ms = INT64_C(1787623200123),
        .metric = "voltage",
        .value = 220.6,
        .unit = "volt",
        .quality = MEASUREMENT_QUALITY_UNCERTAIN
    };
    char *json = NULL;
    size_t json_size = 0;

    if (measurement_to_json(&input, &json, &json_size) != MEASUREMENT_JSON_OK ||
        json == NULL ||
        json_size != strlen(json))
    {
        fprintf(stderr, "normal encode failed\n");
        measurement_json_free(json);
        return -1;
    }

    /*
     * 把生成的JSON重新交给解析方法，并逐字段比较；这样可以同时证明
     * 八个字段都已经写入，而且quality字符串能够正确映射回枚举。
     */
    measurement_t decoded = {0};
    measurement_json_result_t decode_result =
        measurement_from_json(json, json_size, &decoded);

    if (decode_result != MEASUREMENT_JSON_OK ||
        decoded.schema_version != input.schema_version ||
        strcmp(decoded.device_id, input.device_id) != 0 ||
        decoded.sequence != input.sequence ||
        decoded.timestamp_ms != input.timestamp_ms ||
        strcmp(decoded.metric, input.metric) != 0 ||
        decoded.value != input.value ||
        strcmp(decoded.unit, input.unit) != 0 ||
        decoded.quality != input.quality)
    {
        fprintf(stderr, "encode/decode round trip failed\n");
        measurement_json_free(json);
        return -1;
    }

    measurement_json_free(json);
    puts("normal encode and round-trip test passed");
    return 0;
}

static int test_encode_timestamp_precision(void)
{
    measurement_t input = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "meter-01",
        .sequence = UINT32_MAX,
        .timestamp_ms = INT64_MAX,
        .metric = "energy",
        .value = 1.0,
        .unit = "kwh",
        .quality = MEASUREMENT_QUALITY_GOOD
    };
    char *json = NULL;
    size_t json_size = 0;

    if (measurement_to_json(&input, &json, &json_size) != MEASUREMENT_JSON_OK ||
        json == NULL)
    {
        fprintf(stderr, "INT64_MAX timestamp encode failed\n");
        measurement_json_free(json);
        return -1;
    }

    /*
     * INT64_MAX不能被double精确表示，不能用解析回来的double验证；
     * 直接检查JSON文本，确保序列化没有把末尾数字舍入成2^63。
     */
    if (strstr(
            json,
            "\"timestamp_ms\":9223372036854775807") == NULL)
    {
        fprintf(stderr, "timestamp precision was lost: %s\n", json);
        measurement_json_free(json);
        return -1;
    }

    measurement_json_free(json);
    puts("timestamp encode precision test passed");
    return 0;
}

static int test_encode_error_does_not_change_output(void)
{
    measurement_t invalid = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "meter-01",
        .sequence = 0,
        .timestamp_ms = 1,
        .metric = "temperature",
        .value = 12.5,
        .unit = "celsius",
        .quality = MEASUREMENT_QUALITY_GOOD
    };
    char *const pointer_sentinel = (char *)(uintptr_t)0x1234u;
    char *json = pointer_sentinel;
    size_t json_size = 1234u;

    if (measurement_to_json(&invalid, &json, &json_size) !=
            MEASUREMENT_JSON_INVALID_MEASUREMENT ||
        json != pointer_sentinel ||
        json_size != 1234u)
    {
        fprintf(stderr, "encode error changed output arguments\n");
        return -1;
    }

    puts("encode error output-preservation test passed");
    return 0;
}

int main(void)
{
    /* 任意一组失败就立即以非0状态退出，方便CTest判定测试失败。 */
    if (test_normal_decode() != 0 ||
        test_invalid_input_does_not_change_output() != 0 ||
        test_numeric_conversion_boundaries() != 0 ||
        test_normal_encode_and_round_trip() != 0 ||
        test_encode_timestamp_precision() != 0 ||
        test_encode_error_does_not_change_output() != 0)
    {
        return 1;
    }

    puts("measurement JSON tests passed");
    return 0;
}
