#include "mqtt_command_receiver.h"

#include <stdlib.h>
#include <stdio.h>

static void print_message(void *context,
                          const char *topic,
                          const void *payload,
                          size_t payload_length)
{
    (void)context;

    printf("topic: %s\n", topic);
    printf("payload: %.*s\n",
           (int)payload_length,
           (const char *)payload);
}

int main(void)
{
    mqtt_command_receiver_config_t config = {
        .host = "127.0.0.1",
        .port = 1883,
        .client_id = "gateway-command-receiver-demo",
        .request_topic =
            "edgevision/v1/devices/gateway-01/commands/request",
        .keepalive_seconds = 60,
        .message_handler = print_message,
        .message_handler_context = NULL
    };

    mqtt_command_receiver_t *receiver = mqtt_command_receiver_create(&config);

    if (receiver == NULL)
    {
        fprintf(stderr, "mqtt_command_receiver_t failed\n");
        return EXIT_FAILURE;
    }

    mqtt_command_receiver_result_t  ret = mqtt_command_receiver_start(receiver);
    if (ret != MQTT_COMMAND_RECEIVER_OK)
    {
        fprintf(stderr, "mqtt_command_receiver_start failed %d\n", ret);
        mqtt_command_receiver_destroy(receiver);
        return EXIT_FAILURE;
    }
    
    ret = mqtt_command_receiver_wait_ready(receiver, 5);

    if (ret != MQTT_COMMAND_RECEIVER_OK)
    {
        fprintf(stderr, "mqtt_command_receiver_wait_ready  failed %d\n", ret);
        mqtt_command_receiver_destroy(receiver);
        return EXIT_FAILURE;
    }

    getchar();

    mqtt_command_receiver_destroy(receiver);
    return EXIT_SUCCESS;
    
}