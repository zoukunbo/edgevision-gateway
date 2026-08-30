#include "real_serial_source.h"

#include <errno.h>
#include <stddef.h>

int real_serial_source_open(real_serial_source_t *source,
                            const real_serial_source_config_t *config)
{
    if (source == NULL || config == NULL || config->receive_length == 0 ||
        config->receive_length > REAL_SERIAL_SOURCE_CAPACITY ||
        config->timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (rs485_port_open(&source->port, config->serial_path,
                        config->gpiochip_path, config->line_offset) < 0) {
        return -1;
    }
    source->received = 0;
    source->receive_length = config->receive_length;
    source->timeout_ms = config->timeout_ms;
    return 0;
}

void real_serial_source_close(real_serial_source_t *source)
{
    if (source == NULL) return;
    rs485_port_close(&source->port);
    source->received = 0;
    source->receive_length = 0;
    source->timeout_ms = 0;
}

int real_serial_source_send(real_serial_source_t *source,
                            const uint8_t *data, size_t length)
{
    if (source == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (source->port.serial_fd < 0) {
        errno = ENODEV;
        return -1;
    }
    if (data == NULL || length == 0) {
        errno = EINVAL;
        return -1;
    }
    return rs485_send_frame(source->port.serial_fd, &source->port.rse,
                            data, length);
}

real_serial_source_result_t real_serial_source_read(
    real_serial_source_t *source, serial_stop_fn should_stop, void *stop_context)
{
    if (source == NULL) {
        errno = EINVAL;
        return REAL_SERIAL_SOURCE_ERROR;
    }
    source->received = 0;
    if (source->port.serial_fd < 0) {
        errno = ENODEV;
        return REAL_SERIAL_SOURCE_ERROR;
    }
    if (source->receive_length == 0 ||
        source->receive_length > sizeof(source->buffer) || source->timeout_ms <= 0) {
        errno = EINVAL;
        return REAL_SERIAL_SOURCE_ERROR;
    }
    int result = serial_read_exact_timeout_stop(
        source->port.serial_fd, source->buffer, source->receive_length,
        source->timeout_ms, &source->received, should_stop, stop_context);
    if (result > 0) return REAL_SERIAL_SOURCE_COMPLETE;
    if (result == 0) return REAL_SERIAL_SOURCE_TIMEOUT;
    if (errno == ECANCELED) return REAL_SERIAL_SOURCE_CANCELED;
    return REAL_SERIAL_SOURCE_ERROR;
}

/* 原始串口字节还没有协议语义，不能构造Measurement。 */
static measurement_source_result_t placeholder_next(
    void *context, measurement_t *output)
{
    (void)context;
    if (output == NULL) return MEASUREMENT_SOURCE_ERROR;
    return MEASUREMENT_SOURCE_NO_DATA;
}

measurement_source_t real_serial_source_placeholder(void)
{
    const measurement_source_t source = {
        .context = NULL,
        .next = placeholder_next,
    };
    return source;
}
