#include "measurement_json.h"
#include "cJSON.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * cJSON学习提示
 * ------------
 * cJSON把一段JSON解析成由cJSON节点组成的树：对象、字符串、数字等都是节点。
 * 本文件用到的内存所有权规则是理解这些接口的关键：
 *
 * 1. cJSON_ParseWithLength()返回根节点，调用者拥有整棵树，最终必须
 *    cJSON_Delete(root)。删除根节点会递归释放它拥有的所有子节点和字符串。
 * 2. cJSON_GetObjectItemCaseSensitive()只返回树内节点的“借用指针”，不能单独
 *    cJSON_Delete()，也不能在根节点删除后继续访问其valuestring/valuedouble。
 * 3. cJSON_CreateObject()创建的根对象同样由调用者负责；Add*ToObject()成功后，
 *    新字段节点的所有权归根对象，所以清理根对象即可。
 * 4. cJSON_PrintUnformatted()返回一块独立分配的JSON文本。删除树不会删除该文本，
 *    文本必须另行用cJSON_free()释放，以兼容cJSON可能配置的自定义分配器。
 *
 * 本文件坚持“先完整校验临时值，最后提交输出”：失败路径只释放本函数拥有的
 * cJSON内存，不修改调用者的measurement_t或输出指针。
 */

measurement_json_result_t measurement_from_json(
    const char *json,
    size_t json_size,
    measurement_t *output)
{
    if (json == NULL || json_size == 0 || output == NULL)
    {
        fprintf(
            stderr,
            "measurement_from_json: json/output is NULL or json_size is zero\n");
        return MEASUREMENT_JSON_INVALID_ARGUMENT;
    }

    /*
     * 按调用者给出的长度解析，不依赖strlen()寻找输入末尾。
     * 成功时root由本函数拥有；失败返回NULL且没有可供本函数释放的树。
     */
    cJSON *root = cJSON_ParseWithLength(json, json_size);

    if (root == NULL)
    {
        fprintf(stderr, "measurement_from_json: invalid JSON syntax\n");
        return MEASUREMENT_JSON_PARSE_ERROR;
    }

    /* IsObject只检查节点类型；Measurement协议要求最外层必须是JSON对象。 */
    if (!cJSON_IsObject(root))
    {
        fprintf(stderr, "measurement_from_json: JSON root must be an object\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ROOT_NOT_OBJECT;
    }

    /*
     * 所有字段都先校验，最后才写入 output。
     * 因此任意字段失败时，调用者传入的 output 都保持不变。
     */
    /*
     * GetObjectItemCaseSensitive按键名精确匹配大小写。
     * 返回值是root内部的借用节点：只读取，不单独释放。
     */
    /* device_id 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *device_id =
        cJSON_GetObjectItemCaseSensitive(root, "device_id");

    if (device_id == NULL)
    {
        fprintf(stderr, "measurement_from_json: missing required field 'device_id'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /*
     * IsString判断JSON类型，不会把数字、布尔值自动转换为文本。
     * valuestring同样由root拥有，只能在cJSON_Delete(root)之前读取。
     */
    /* device_id 第2层：节点必须是JSON字符串，并且字符串存储有效。 */
    if (!cJSON_IsString(device_id) || device_id->valuestring == NULL)
    {
        fprintf(stderr, "measurement_from_json: field 'device_id' must be a string\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /* device_id 第3层：字符串非空，且必须给末尾的'\0'保留空间。 */
    size_t device_id_length = strlen(device_id->valuestring);
    if (device_id_length == 0 ||
        device_id_length >= MEASUREMENT_DEVICE_ID_CAPACITY)
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'device_id' length must be 1..%u\n",
            (unsigned int)(MEASUREMENT_DEVICE_ID_CAPACITY - 1));
        cJSON_Delete(root);
        return MEASUREMENT_JSON_OUT_OF_RANGE;
    }

    /* metric 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *metric =
        cJSON_GetObjectItemCaseSensitive(root, "metric");

    if (metric == NULL)
    {
        fprintf(stderr, "measurement_from_json: missing required field 'metric'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /* metric 第2层：节点必须是JSON字符串，并且字符串存储有效。 */
    if (!cJSON_IsString(metric) || metric->valuestring == NULL)
    {
        fprintf(stderr, "measurement_from_json: field 'metric' must be a string\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /* metric 第3层：字符串非空，且不能超过结构体数组容量。 */
    size_t metric_length = strlen(metric->valuestring);
    if (metric_length == 0 || metric_length >= MEASUREMENT_METRIC_CAPACITY)
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'metric' length must be 1..%u\n",
            (unsigned int)(MEASUREMENT_METRIC_CAPACITY - 1));
        cJSON_Delete(root);
        return MEASUREMENT_JSON_OUT_OF_RANGE;
    }

    /* unit 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *unit =
        cJSON_GetObjectItemCaseSensitive(root, "unit");

    if (unit == NULL)
    {
        fprintf(stderr, "measurement_from_json: missing required field 'unit'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /* unit 第2层：节点必须是JSON字符串，并且字符串存储有效。 */
    if (!cJSON_IsString(unit) || unit->valuestring == NULL)
    {
        fprintf(stderr, "measurement_from_json: field 'unit' must be a string\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /* unit 第3层：字符串非空，且不能超过结构体数组容量。 */
    size_t unit_length = strlen(unit->valuestring);
    if (unit_length == 0 || unit_length >= MEASUREMENT_UNIT_CAPACITY)
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'unit' length must be 1..%u\n",
            (unsigned int)(MEASUREMENT_UNIT_CAPACITY - 1));
        cJSON_Delete(root);
        return MEASUREMENT_JSON_OUT_OF_RANGE;
    }

    /* schema_version 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *schema_version =
        cJSON_GetObjectItemCaseSensitive(root, "schema_version");

    if (schema_version == NULL)
    {
        fprintf(
            stderr,
            "measurement_from_json: missing required field 'schema_version'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /*
     * IsNumber只接受JSON数字节点；数值通过valuedouble读取。
     * cJSON以double为主要数字表示，因此整数还需要范围和整数性校验。
     */
    /* schema_version 第2层：节点必须是JSON数字。 */
    if (!cJSON_IsNumber(schema_version))
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'schema_version' must be a number\n");
        /* Delete根节点会递归释放所有字段；借用字段指针也随即失效。 */
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /* schema_version 第3层：只接受当前程序支持的协议版本。 */
    if (schema_version->valuedouble != (double)MEASUREMENT_SCHEMA_VERSION)
    {
        fprintf(
            stderr,
            "measurement_from_json: unsupported schema_version, expected %u\n",
            (unsigned int)MEASUREMENT_SCHEMA_VERSION);
        cJSON_Delete(root);
        return MEASUREMENT_JSON_UNSUPPORTED_SCHEMA;
    }

    /* sequence 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *sequence =
        cJSON_GetObjectItemCaseSensitive(root, "sequence");

    if (sequence == NULL)
    {
        fprintf(stderr, "measurement_from_json: missing required field 'sequence'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /* sequence 第2层：节点必须是JSON数字。 */
    if (!cJSON_IsNumber(sequence))
    {
        fprintf(stderr, "measurement_from_json: field 'sequence' must be a number\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /*
     * sequence 第3层：必须是uint32_t可表示的正整数。
     * cJSON用double保存JSON数字；isfinite排除NaN以及正、负无穷大，
     * 最后一项通过“转换后再转回double”判断它有没有小数部分。
     */
    if (!isfinite(sequence->valuedouble) ||
        sequence->valuedouble < 1.0 ||
        sequence->valuedouble > (double)UINT32_MAX ||
        sequence->valuedouble != (double)(uint32_t)sequence->valuedouble)
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'sequence' must be an integer in 1..%u\n",
            (unsigned int)UINT32_MAX);
        cJSON_Delete(root);
        return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    /* timestamp_ms 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *timestamp_ms =
        cJSON_GetObjectItemCaseSensitive(root, "timestamp_ms");

    if (timestamp_ms == NULL)
    {
        fprintf(
            stderr,
            "measurement_from_json: missing required field 'timestamp_ms'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /* timestamp_ms 第2层：节点必须是JSON数字。 */
    if (!cJSON_IsNumber(timestamp_ms))
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'timestamp_ms' must be a number\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /*
     * timestamp_ms 第3层：必须是int64_t可表示的正整数。
     * isfinite保证转换为int64_t之前不是NaN或无穷大；最后一项排除小数。
     * (double)INT64_MAX会舍入为2^63，所以与上限比较时必须使用严格小于。
     */
    if (!isfinite(timestamp_ms->valuedouble) ||
        timestamp_ms->valuedouble < 1.0 ||
        timestamp_ms->valuedouble >= (double)INT64_MAX ||
        timestamp_ms->valuedouble != (double)(int64_t)timestamp_ms->valuedouble)
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'timestamp_ms' must be a positive integer representable as int64_t\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    /* value 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *value =
        cJSON_GetObjectItemCaseSensitive(root, "value");

    if (value == NULL)
    {
        fprintf(stderr, "measurement_from_json: missing required field 'value'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /* value 第2层：节点必须是JSON数字。 */
    if (!cJSON_IsNumber(value))
    {
        fprintf(stderr, "measurement_from_json: field 'value' must be a number\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /*
     * value 第3层：测量值可以是0或负数，但必须是有限数，
     * 因此使用isfinite排除NaN、正无穷大和负无穷大。
     */
    if (!isfinite(value->valuedouble))
    {
        fprintf(stderr, "measurement_from_json: field 'value' must be finite\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    /* quality 第1层：取得必填节点，并判断节点是否存在。 */
    const cJSON *quality =
        cJSON_GetObjectItemCaseSensitive(root, "quality");

    if (quality == NULL)
    {
        fprintf(stderr, "measurement_from_json: missing required field 'quality'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_MISSING_FIELD;
    }

    /* quality 第2层：节点必须是JSON字符串，并且字符串存储有效。 */
    if (!cJSON_IsString(quality) || quality->valuestring == NULL)
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'quality' must be a string\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_WRONG_TYPE;
    }

    /*
     * quality 第3层：把可读字符串映射成measurement_quality_t枚举值。
     * strcmp区分大小写，因此这里只接受以下三个完全匹配的字符串。
     */
    measurement_quality_t quality_value;

    if (strcmp(quality->valuestring, "good") == 0)
    {
        quality_value = MEASUREMENT_QUALITY_GOOD;
    }
    else if (strcmp(quality->valuestring, "uncertain") == 0)
    {
        quality_value = MEASUREMENT_QUALITY_UNCERTAIN;
    }
    else if (strcmp(quality->valuestring, "bad") == 0)
    {
        quality_value = MEASUREMENT_QUALITY_BAD;
    }
    else
    {
        fprintf(
            stderr,
            "measurement_from_json: field 'quality' must be 'good', 'uncertain', or 'bad'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    /* 走到这里说明全部字段有效，现在才执行最终赋值。 */
    measurement_t temporary = {0};

    memcpy(
        temporary.device_id,
        device_id->valuestring,
        device_id_length + 1);

    memcpy(
        temporary.metric,
        metric->valuestring,
        metric_length + 1);

    memcpy(
        temporary.unit,
        unit->valuestring,
        unit_length + 1);

    temporary.schema_version =
        (uint32_t)schema_version->valuedouble;

    temporary.sequence =
        (uint32_t)sequence->valuedouble;

    temporary.timestamp_ms =
        (int64_t)timestamp_ms->valuedouble;

    temporary.value =
        value->valuedouble;

    temporary.quality =
        quality_value;

    if (measurement_validate(&temporary) !=
        MEASUREMENT_VALID)
    {
        cJSON_Delete(root);
        return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    /* 唯一允许修改正式输出的位置。 */
    *output = temporary;

    /* 复制工作已经完成，释放解析树；output中的字符数组不依赖该树。 */
    cJSON_Delete(root);
    return MEASUREMENT_JSON_OK;
}

measurement_json_result_t measurement_to_json(
    const measurement_t *measurement,
    char **json_output,
    size_t *json_size)
{
    /*
     * 1. 三个参数都必须是有效指针。
     * 在整个函数成功之前不修改*json_output和*json_size，保证失败不污染输出。
     */
    if (measurement == NULL || json_output == NULL || json_size == NULL)
    {
        fprintf(stderr, "measurement_to_json: argument must not be NULL\n");
        return MEASUREMENT_JSON_INVALID_ARGUMENT;
    }

    /* 2. 只允许序列化符合Measurement领域契约的数据。 */
    if (measurement_validate(measurement) != MEASUREMENT_VALID)
    {
        fprintf(stderr, "measurement_to_json: invalid measurement\n");
        return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    /*
     * 3. 创建空JSON对象。CreateObject只创建根节点，不会自动添加字段；
     * 后续任意字段添加失败都必须Delete它，统一释放此前已添加的子节点。
     */
    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to allocate JSON object\n");
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    /*
     * 4. 添加八个必填字段。
     * AddStringToObject是“创建字符串节点 + 挂到对象”两个动作的便捷接口；
     * cJSON会复制传入文本，成功后字段由root拥有，返回值是新字段节点。
     * 返回NULL通常表示分配或挂接失败，此时仍只需Delete(root)。
     *
     * uint32_t最大值可以被double精确表示，因此schema_version和sequence
     * 可以直接使用cJSON_AddNumberToObject()。
     */
    if (cJSON_AddStringToObject(root, "device_id", measurement->device_id) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'device_id'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    if (cJSON_AddStringToObject(root, "metric", measurement->metric) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'metric'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    if (cJSON_AddStringToObject(root, "unit", measurement->unit) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'unit'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    /* AddNumberToObject接收double；这里的uint32_t转换不会损失整数精度。 */
    if (cJSON_AddNumberToObject(
            root,
            "schema_version",
            (double)measurement->schema_version) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'schema_version'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    if (cJSON_AddNumberToObject(
            root,
            "sequence",
            (double)measurement->sequence) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'sequence'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    /*
     * cJSON通常用double保存数字，但double不能精确表示所有int64_t。
     * 先把timestamp_ms格式化成十进制文本，再作为JSON原始数字添加，
     * 可以保证序列化结果中的毫秒时间戳没有精度损失。
     * AddRawToObject会复制timestamp_text，并在打印时原样输出、不加字符串引号；
     * 因此只能传入已经确认是合法JSON值的文本，不能直接放入外部未校验输入。
     */
    char timestamp_text[32];
    int timestamp_length = snprintf(
        timestamp_text,
        sizeof(timestamp_text),
        "%" PRId64,
        measurement->timestamp_ms);

    if (timestamp_length < 0 ||
        (size_t)timestamp_length >= sizeof(timestamp_text))
    {
        fprintf(stderr, "measurement_to_json: failed to format 'timestamp_ms'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    if (cJSON_AddRawToObject(root, "timestamp_ms", timestamp_text) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'timestamp_ms'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    if (cJSON_AddNumberToObject(root, "value", measurement->value) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'value'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    /* 5. 把quality枚举转换成对人可读的JSON字符串。 */
    const char *quality_text = NULL;

    switch (measurement->quality)
    {
        case MEASUREMENT_QUALITY_GOOD:
            quality_text = "good";
            break;

        case MEASUREMENT_QUALITY_UNCERTAIN:
            quality_text = "uncertain";
            break;

        case MEASUREMENT_QUALITY_BAD:
            quality_text = "bad";
            break;

        default:
            fprintf(stderr, "measurement_to_json: unsupported quality value\n");
            cJSON_Delete(root);
            return MEASUREMENT_JSON_INVALID_MEASUREMENT;
    }

    if (cJSON_AddStringToObject(root, "quality", quality_text) == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to add 'quality'\n");
        cJSON_Delete(root);
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    /*
     * 6. PrintUnformatted遍历整棵树并生成无缩进、无多余换行的紧凑JSON。
     * 返回文本是独立分配的缓冲区，不属于root，由调用者负责释放。
     */
    char *json = cJSON_PrintUnformatted(root);

    /* 7. 文本与树互相独立；JSON文本生成后可以立即递归删除树。 */
    cJSON_Delete(root);

    if (json == NULL)
    {
        fprintf(stderr, "measurement_to_json: failed to allocate JSON text\n");
        return MEASUREMENT_JSON_ALLOCATION_ERROR;
    }

    /*
     * 8. 到这里所有操作都已成功，最后才提交两个正式输出。
     * 不能在这里释放json；调用者使用完后应调用measurement_json_free()。
     */
    *json_output = json;
    *json_size = strlen(json);
    return MEASUREMENT_JSON_OK;
}

void measurement_json_free(char *json)
{
    /*
     * 不直接调用free()：cJSON允许通过cJSON_InitHooks()替换内存分配器，
     * cJSON_free()能保证与PrintUnformatted使用同一套释放函数。NULL也是安全的。
     */
    cJSON_free(json);
}
