#include "cJSON.h"
#include "measurement.h"

#include <stdio.h>
#include <string.h>

typedef enum
{
    SCHEMA_CHECK_OK = 0,
    SCHEMA_CHECK_INVALID_JSON,
    SCHEMA_CHECK_ROOT_NOT_OBJECT,
    SCHEMA_CHECK_MISSING,
    SCHEMA_CHECK_WRONG_TYPE,
    SCHEMA_CHECK_UNSUPPORTED
} schema_check_result_t;

typedef enum
{
    JSON_FIELD_OK = 0,
    JSON_FIELD_MISSING,
    JSON_FIELD_WRONG_TYPE,
    JSON_FIELD_OUT_OF_RANGE
} json_field_result_t;

static json_field_result_t json_copy_required_string(
    const cJSON *root,
    const char *field_name,
    char *output,
    size_t output_capacity)
{
    if (root == NULL ||
        field_name == NULL ||
        output == NULL ||
        output_capacity == 0)
    {
        return JSON_FIELD_OUT_OF_RANGE;
    }

    const cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            root,
            field_name);

    if (item == NULL)
    {
        return JSON_FIELD_MISSING;
    }

    if (!cJSON_IsString(item) ||
        item->valuestring == NULL)
    {
        return JSON_FIELD_WRONG_TYPE;
    }

    size_t length = strlen(item->valuestring);

    /*
     * 空字符串非法。
     * length必须小于容量，因为还要保存末尾的'\0'。
     */
    if (length == 0 || length >= output_capacity)
    {
        return JSON_FIELD_OUT_OF_RANGE;
    }

    /*
     * 前面已经证明目标空间足够。
     * length + 1 会把末尾的'\0'一起复制。
     */
    memcpy(
        output,
        item->valuestring,
        length + 1);

    return JSON_FIELD_OK;
}

static schema_check_result_t check_schema_version(
    const char *json,
    size_t json_size)
{
    cJSON *root = cJSON_ParseWithLength(json, json_size);

    if (root == NULL)
    {
        return SCHEMA_CHECK_INVALID_JSON;
    }

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return SCHEMA_CHECK_ROOT_NOT_OBJECT;
    }

    const cJSON *schema_version =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "schema_version");

    if (schema_version == NULL)
    {
        cJSON_Delete(root);
        return SCHEMA_CHECK_MISSING;
    }

    if (!cJSON_IsNumber(schema_version))
    {
        cJSON_Delete(root);
        return SCHEMA_CHECK_WRONG_TYPE;
    }

    if (schema_version->valuedouble !=
        (double)MEASUREMENT_SCHEMA_VERSION)
    {
        cJSON_Delete(root);
        return SCHEMA_CHECK_UNSUPPORTED;
    }

    cJSON_Delete(root);
    return SCHEMA_CHECK_OK;
}

static int expect_result(
    const char *case_name,
    schema_check_result_t actual,
    schema_check_result_t expected)
{
    if (actual != expected)
    {
        fprintf(
            stderr,
            "%s failed: actual=%d expected=%d\n",
            case_name,
            actual,
            expected);
        return -1;
    }

    printf("%s passed\n", case_name);
    return 0;
}

static int test_device_id_copy(void)
{
    char output[MEASUREMENT_DEVICE_ID_CAPACITY] = {0};

    const char valid_json[] =
        "{\"device_id\":\"power-meter-01\"}";

    cJSON *root = cJSON_ParseWithLength(
        valid_json,
        strlen(valid_json));

    if (root == NULL)
    {
        fprintf(stderr, "valid device JSON parsing failed\n");
        return -1;
    }

    json_field_result_t result =
        json_copy_required_string(
            root,
            "device_id",
            output,
            sizeof(output));

    cJSON_Delete(root);

    if (result != JSON_FIELD_OK ||
        strcmp(output, "power-meter-01") != 0)
    {
        fprintf(stderr, "valid device_id copy failed\n");
        return -1;
    }

    const char wrong_type_json[] =
        "{\"device_id\":123}";

    root = cJSON_ParseWithLength(
        wrong_type_json,
        strlen(wrong_type_json));

    if (root == NULL)
    {
        fprintf(stderr, "wrong-type JSON parsing failed\n");
        return -1;
    }

    result = json_copy_required_string(
        root,
        "device_id",
        output,
        sizeof(output));

    cJSON_Delete(root);

    if (result != JSON_FIELD_WRONG_TYPE)
    {
        fprintf(stderr, "wrong device_id type was accepted\n");
        return -1;
    }

    char long_device_id[
        MEASUREMENT_DEVICE_ID_CAPACITY + 1];

    memset(
        long_device_id,
        'x',
        MEASUREMENT_DEVICE_ID_CAPACITY);

    long_device_id[
        MEASUREMENT_DEVICE_ID_CAPACITY] = '\0';

    char oversized_json[128];

    int written = snprintf(
        oversized_json,
        sizeof(oversized_json),
        "{\"device_id\":\"%s\"}",
        long_device_id);

    if (written < 0 ||
        (size_t)written >= sizeof(oversized_json))
    {
        fprintf(stderr, "oversized JSON construction failed\n");
        return -1;
    }

    root = cJSON_ParseWithLength(
        oversized_json,
        strlen(oversized_json));

    if (root == NULL)
    {
        fprintf(stderr, "oversized JSON parsing failed\n");
        return -1;
    }

    result = json_copy_required_string(
        root,
        "device_id",
        output,
        sizeof(output));

    cJSON_Delete(root);

    if (result != JSON_FIELD_OUT_OF_RANGE)
    {
        fprintf(stderr, "oversized device_id was accepted\n");
        return -1;
    }

    puts("device_id copy tests passed");
    return 0;
}

int main(void)
{
    const char valid_json[] = "{\"schema_version\":1}";

    if (expect_result(
            "valid schema",
            check_schema_version(
                valid_json,
                strlen(valid_json)),
            SCHEMA_CHECK_OK) != 0)
    {
        return 1;
    }

    const char invalid_json[] = "{\"schema_version\":"; 

    if (expect_result(
            "invalid JSON syntax",
            check_schema_version(
                invalid_json,
                strlen(invalid_json)),
            SCHEMA_CHECK_INVALID_JSON) != 0)
    {
        return 1;
    }

    const char array_root[] = "[]";

    if (expect_result(
            "root is not object",
            check_schema_version(
                array_root,
                strlen(array_root)),
            SCHEMA_CHECK_ROOT_NOT_OBJECT) != 0)
    {
        return 1;
    }

    const char missing_schema[] = "{}";

    if (expect_result(
            "missing schema_version",
            check_schema_version(
                missing_schema,
                strlen(missing_schema)),
            SCHEMA_CHECK_MISSING) != 0)
    {
        return 1;
    }

    const char wrong_type[] =
        "{\"schema_version\":\"1\"}";

    if (expect_result(
            "schema_version wrong type",
            check_schema_version(
                wrong_type,
                strlen(wrong_type)),
            SCHEMA_CHECK_WRONG_TYPE) != 0)
    {
        return 1;
    }

    const char fractional_version[] =
        "{\"schema_version\":1.5}";

    if (expect_result(
            "fractional schema_version",
            check_schema_version(
                fractional_version,
                strlen(fractional_version)),
            SCHEMA_CHECK_UNSUPPORTED) != 0)
    {
        return 1;
    }

    const char unknown_version[] =
        "{\"schema_version\":2}";

    if (expect_result(
            "unsupported schema_version",
            check_schema_version(
                unknown_version,
                strlen(unknown_version)),
            SCHEMA_CHECK_UNSUPPORTED) != 0)
    {
        return 1;
    }

    if (test_device_id_copy() != 0)
    {
        return 1;
    }
    puts("all cJSON schema tests passed");
    return 0;
}