#include "measurement_source.h"

#include <stddef.h>

measurement_source_result_t measurement_source_next(
    measurement_source_t *source,
    measurement_t *output)
{
    if (source == NULL || output == NULL ||  source->next == NULL)
    {
       return MEASUREMENT_SOURCE_ERROR;
    }

    return source->next(source->context, output);
}