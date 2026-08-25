#include "measurement.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int expect_result(
    const char *case_name,
    measurement_validation_result_t actual,
    measurement_validation_result_t expected)
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

    return 0;
}

int main(void)
{
    measurement_t valid = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "power-meter-01",
        .sequence = 1,
        .timestamp_ms = 1787623200123LL,
        .metric = "voltage",
        .value = 220.6,
        .unit = "volt",
        .quality = MEASUREMENT_QUALITY_GOOD
    };

    if (expect_result(
            "valid measurement",
            measurement_validate(&valid),
            MEASUREMENT_VALID) != 0)
    {
        return 1;
    }

    measurement_t missing_terminator = valid;
    memset(
        missing_terminator.device_id,
        'x',
        sizeof(missing_terminator.device_id));

    if (expect_result(
            "device_id without terminator",
            measurement_validate(&missing_terminator),
            MEASUREMENT_INVALID_DEVICE_ID) != 0)
    {
        return 1;
    }

    measurement_t invalid_number = valid;
    invalid_number.value = NAN;

    if (expect_result(
            "NaN value",
            measurement_validate(&invalid_number),
            MEASUREMENT_INVALID_VALUE) != 0)
    {
        return 1;
    }

    puts("measurement tests passed");
    return 0;
}