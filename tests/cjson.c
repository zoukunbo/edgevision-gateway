#include "cJSON.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char json[] =
        "{"
        "\"device_id\":\"sensor-01\","
        "\"sequence\":42,"
        "\"value\":23.5"
        "}";
    /* JSON字符串 -> cJSON 根节点*/
    cJSON *root = cJSON_ParseWithLength(
        json,
        strlen(json));

    if (root == NULL)
    {
        fprintf(stderr, "JSON syntax error\n");
        return 1;
    }
    /* 判断是不是一个对象 */
    if (!cJSON_IsObject(root))
    {
        fprintf(stderr, "root is not an object\n");
        cJSON_Delete(root);
        return 1;
    }
    /* 获取对应字段 device_id */
    const cJSON *device_id =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "device_id");
    /* 判断字段数据类型 */
    if (!cJSON_IsString(device_id))
    {
        fprintf(stderr, "device_id is missing or not a string\n");
        cJSON_Delete(root);
        return 1;
    }
    /* 获取对应字段 sequence */
    const cJSON *sequence =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "sequence");

    if (!cJSON_IsNumber(sequence))
    {
        fprintf(stderr, "sequence is missing or not a number\n");
        cJSON_Delete(root);
        return 1;
    }
    /* 获取对应字段 value */
    const cJSON *value =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "value");

    if (!cJSON_IsNumber(value))
    {
        fprintf(stderr, "value is missing or not a number\n");
        cJSON_Delete(root);
        return 1;
    }

    printf(
        "device=%s sequence=%.0f value=%.1f\n",
        device_id->valuestring,
        sequence->valuedouble,
        value->valuedouble);

    cJSON_Delete(root);
    return 0;
}