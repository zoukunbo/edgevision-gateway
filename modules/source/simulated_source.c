#include "simulated_source.h"

#include <stddef.h>

static measurement_source_result_t simulated_source_next(
    void *context,
    measurement_t *output)
{
    simulated_source_t *source = context;

    if (source == NULL || output == NULL)
    {
        return MEASUREMENT_SOURCE_ERROR;
    }

    const measurement_t measurement = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "sim-temperature-01",
        .sequence = source->next_sequence,
        .timestamp_ms = source->next_timestamp_ms,
        .metric = "temperature",
        .value = 20.0 + (source->next_sequence - 1u) * 0.125,
        .unit = "celsius",
        .quality = MEASUREMENT_QUALITY_GOOD
    };

    *output = measurement;

    source->next_sequence += 1u;
    source->next_timestamp_ms += 1;
    return MEASUREMENT_SOURCE_OK;
}

void simulated_source_init(simulated_source_t *source)
{
    if (source == NULL)
    {
        return;
    }

    source->next_sequence = 1u;
    source->next_timestamp_ms = INT64_C(1787623200000);
}

measurement_source_t simulated_source_as_measurement_source(
    simulated_source_t *source)
{
    const measurement_source_t interface = {
        .context = source,
        .next = simulated_source_next
    };

    return interface;
}