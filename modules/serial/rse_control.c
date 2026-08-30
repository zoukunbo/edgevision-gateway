#include "rse_control.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * 通过 GPIO v2 line handle 修改 RSE 电平。
 * mask 只选择本控制器申请的第一个 line，避免影响同一请求中的其他 GPIO。
 */
static int rse_control_set_value(rse_control_t *control, int value)
{
    if (control == NULL || control->line_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct gpio_v2_line_values values = {
        .bits = value != 0 ? 1ULL : 0ULL,
        .mask = 1ULL,
    };

    return ioctl(control->line_fd,
                 GPIO_V2_LINE_SET_VALUES_IOCTL,
                 &values);
}

int rse_control_open(rse_control_t *control,
                     const char *gpiochip_path,
                     unsigned int line_offset)
{
    if (control == NULL || gpiochip_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    control->chip_fd = -1;
    control->line_fd = -1;

    int chip_fd = open(gpiochip_path, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
        return -1;
    }

    struct gpio_v2_line_request request;
    memset(&request, 0, sizeof(request));

    request.offsets[0] = line_offset;
    request.num_lines = 1;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;

    /* 请求 line 时原子设置为 LOW，避免短暂进入发送模式。 */
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id =
        GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].attr.values = 0;
    request.config.attrs[0].mask = 1;

    /* consumer 标签用于 gpioinfo 等诊断工具识别当前占用者。 */
    (void)strncpy(request.consumer,
                  "edgevision-rse",
                  sizeof(request.consumer) - 1);

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        int saved_errno = errno;
        (void)close(chip_fd);
        errno = saved_errno;
        return -1;
    }

    control->chip_fd = chip_fd;
    control->line_fd = request.fd;
    return 0;
}

int rse_control_set_tx(rse_control_t *control)
{
    return rse_control_set_value(control, 1);
}

int rse_control_set_rx(rse_control_t *control)
{
    return rse_control_set_value(control, 0);
}

void rse_control_close(rse_control_t *control)
{
    if (control == NULL) {
        return;
    }

    if (control->line_fd >= 0) {
        (void)close(control->line_fd);
    }

    if (control->chip_fd >= 0) {
        (void)close(control->chip_fd);
    }

    control->line_fd = -1;
    control->chip_fd = -1;
}
