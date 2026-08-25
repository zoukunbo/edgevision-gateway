#ifndef EDGEVISION_DOMAIN_MEASUREMENT_JSON_H
#define EDGEVISION_DOMAIN_MEASUREMENT_JSON_H

#include "measurement.h"

#include <stddef.h>

typedef enum
{
    MEASUREMENT_JSON_OK = 0,
    MEASUREMENT_JSON_INVALID_ARGUMENT = -1,
    MEASUREMENT_JSON_PARSE_ERROR = -2,
    MEASUREMENT_JSON_ROOT_NOT_OBJECT = -3,
    MEASUREMENT_JSON_MISSING_FIELD = -4,
    MEASUREMENT_JSON_WRONG_TYPE = -5,
    MEASUREMENT_JSON_OUT_OF_RANGE = -6,
    MEASUREMENT_JSON_UNSUPPORTED_SCHEMA = -7,
    MEASUREMENT_JSON_INVALID_MEASUREMENT = -8,
    MEASUREMENT_JSON_ALLOCATION_ERROR = -9
} measurement_json_result_t;

measurement_json_result_t measurement_from_json(
    const char *json,
    size_t json_size,
    measurement_t *output);

measurement_json_result_t measurement_to_json(
    const measurement_t *measurement,
    char **json_output,
    size_t *json_size);

void measurement_json_free(char *json);

#endif