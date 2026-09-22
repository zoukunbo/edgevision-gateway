#include <mosquitto.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static const char *request_topic =
    "edgevision/v1/devices/gateway-01/commands/request";

#define COMMAND_PAYLOAD_CAPACITY 256u

typedef struct
{
    int failed;
    bool received;
    size_t payload_length;
    char payload[COMMAND_PAYLOAD_CAPACITY];
} command_receiver_state_t;

/*
 * 成功返回 0，并写入 request_id 和 command。
 * JSON 无效、字段缺失、字段不是字符串或输出空间不足时返回 -1。
 */
static int parse_remote_command(
    const char *payload,
    char *request_id,
    size_t request_id_capacity,
    char *command,
    size_t command_capacity)
{
    if (payload == NULL ||
        request_id == NULL ||
        request_id_capacity == 0 ||
        command == NULL ||
        command_capacity == 0)
    {
        return -1;
    }

    cJSON *root = cJSON_Parse(payload);

    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return -1;
    }

    /*
     * 这两个指针只是借用 JSON 树中的节点。
     * 不能单独释放，也不能在 cJSON_Delete(root) 后继续使用。
     */
    const cJSON *request_id_item =
        cJSON_GetObjectItemCaseSensitive(root, "request_id");

    const cJSON *command_item =
        cJSON_GetObjectItemCaseSensitive(root, "command");

    if (!cJSON_IsString(request_id_item) ||
        request_id_item->valuestring == NULL ||
        request_id_item->valuestring[0] == '\0' ||
        !cJSON_IsString(command_item) ||
        command_item->valuestring == NULL ||
        command_item->valuestring[0] == '\0')
    {
        cJSON_Delete(root);
        return -1;
    }

    size_t request_id_length =
        strlen(request_id_item->valuestring);
    size_t command_length =
        strlen(command_item->valuestring);

    /* >= 是因为还要保存字符串末尾的 '\0'。 */
    if (request_id_length >= request_id_capacity ||
        command_length >= command_capacity)
    {
        cJSON_Delete(root);
        return -1;
    }

    /*
     * 在删除 JSON 树之前复制出来。
     * +1 把结尾的 '\0' 也一起复制。
     */
    memcpy(request_id,
           request_id_item->valuestring,
           request_id_length + 1);

    memcpy(command,
           command_item->valuestring,
           command_length + 1);

    cJSON_Delete(root);
    return 0;
}

/* 回调函数名由我们命名；返回类型、参数类型和顺序必须匹配注册 API。
 * 以下三个回调均由本例主线程中的 loop_forever 调用，而不是各自创建线程。
 * on_connect：网络循环收到 Broker 的 CONNACK（连接响应）后执行。
 * client：产生该事件的客户端实例，可用它发起 subscribe 或 publish。
 * userdata：mosquitto_new 第三个参数原样传回的指针，本例是 &state。
 * result：Broker 的连接响应码，0 表示接受；非 0 表示拒绝。
 * 每次连接/重连收到连接响应都会触发，不是每收到一条业务消息就触发。
 */
static void on_connect(struct mosquitto *client,
                       void *userdata,
                       int result)
{
    /* void * 恢复为本例传入的 command_receiver_state_t *；库不会替我们
     * 复制或释放 state。main 的 state 必须在客户端及回调使用期间有效。
     */
    command_receiver_state_t *state = userdata;

    if (result != 0) {
        fprintf(stderr, "Broker refused connection: %d\n", result);
        state->failed = 1;
        (void)mosquitto_disconnect(client);
        return;
    }

    /* 连接被接受后，本客户端既可以订阅，也可以发布；两者互不要求。
     * 此处 subscribe 的 NULL 表示不需要取回订阅请求的 mid，0 是请求的 QoS。
     * rc 成功只表示订阅调用成功，Broker 是否接受要看后续 on_subscribe。
     * 可以在回调内调用 mosquitto_publish，但不能阻塞等待同一网络循环
     * 处理发布确认，也不能在回调内再次调用 mosquitto_loop 系列函数。
     */
    int rc = mosquitto_subscribe(
        client, NULL, request_topic, 0);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "subscribe: %s\n", mosquitto_strerror(rc));
        state->failed = 1;
        (void)mosquitto_disconnect(client);
    }
}

/* on_subscribe：网络循环收到 Broker 的 SUBACK（订阅响应）后执行。
 * client、userdata：与 on_connect 相同，分别为客户端和自定义上下文。
 * mid：本次订阅请求的编号，可与 subscribe 输出的 mid 对应；
 *      它不是 JSON 中由应用定义的 request_id。
 * count：本次确认包含的订阅结果数量，也是 granted_qos 数组的长度。
 * granted_qos：逐项表示 Broker 授予的 QoS；本例 MQTT 3.1.1 中
 *              0/1/2 为接受，0x80 为拒绝。此示范只请求一个主题、QoS 0。
 * 多次订阅收到多次响应，就会多次调用；不随每条业务消息调用。
 */
static void on_subscribe(struct mosquitto *client,
                         void *userdata,
                         int mid,
                         int count,
                         const int *granted_qos)
{
    (void)mid; /* 本示范不关联多个订阅请求，明确标记该参数未使用。 */
    command_receiver_state_t *state = userdata;

    if (count != 1 || granted_qos[0] != 0) {
        fprintf(stderr, "Subscription was not accepted\n");
        state->failed = 1;
        (void)mosquitto_disconnect(client);
        return;
    }

    puts("订阅已确认，现在可以发布请求");
    fflush(stdout);
}

/* on_message：网络循环每收到一条订阅消息，就执行一次。
 * client、userdata：仍为产生事件的客户端和 mosquitto_new 的上下文。
 * message：库提供的只读消息描述：
 *   topic      —— 消息所在的主题字符串。
 *   payloadlen —— 内容的字节数，不是 strlen；内容可以含二进制数据。
 *   payload    —— 内容地址，不能假定末尾有 '\0'。
 *   qos/retain —— 这次收到的消息的 QoS 和保留消息标志。
 * message 及关联内存由库管理，回调结束后不能继续保存指针使用；
 * 若要交给业务队列，应在回调内复制到我们自己的存储中。
 */
static void on_message(struct mosquitto *client,
                       void *userdata,
                       const struct mosquitto_message *message)
{
    // 局部指针
    command_receiver_state_t *state = userdata;
    if (message->payloadlen < 0 || (size_t)message->payloadlen >= sizeof(state->payload))
    {
         fprintf(stderr, "payload is too large\n");
        state->failed = 1;
        (void)mosquitto_disconnect(client);
        return;
    }
    
    if (message->payloadlen > 0)
    {
        memcpy(state->payload, message->payload, (size_t)message->payloadlen);
    }

    state->payload[message->payloadlen] = '\0';
    state->payload_length = (size_t)message->payloadlen;
    state->received = true;
    

    printf("topic: %s\n", message->topic);
    printf("payload length: %d\n", message->payloadlen);

    if (message->payloadlen > 0) {
        /* 按长度限制文本打印；这不是任意二进制内容的完整打印方式。 */
        printf("payload: %.*s\n",
               message->payloadlen,
               (const char *)message->payload);
    }

    /* 主动断开使本示范的 loop_forever 返回；不是回调只能执行一次。
     * 如果后来增加发布响应，不能照搬“publish 后立即断开”的结束方式。
     */
    (void)mosquitto_disconnect(client);
}

/**
 * 发布测试指令：
 * mosquitto_pub -h 127.0.0.1 -p 1883 \
  -t 'edgevision/v1/devices/gateway-01/commands/request' \
  -m '{"request_id":"demo-001","command":"status"}'
 */
int main(void)
{
    command_receiver_state_t state = {0};
    int rc = mosquitto_lib_init(); /* 初始化库级资源，不创建连接。 */

    if (rc != MOSQ_ERR_SUCCESS)
        return EXIT_FAILURE;

    /* 创建客户端：NULL 使用自动客户端 ID，true 使用 clean session。
     * &state 作为 userdata 交给各回调；此处尚未连接 Broker。
     */
    struct mosquitto *client =
        mosquitto_new(NULL, true, &state);

    if (client == NULL) {
        mosquitto_lib_cleanup();
        return EXIT_FAILURE;
    }

    /* 登记函数地址，不在这三行执行回调；名称可以改，但注册也要同步改。
     * 函数参数名可以改，参数数量/顺序/类型及 void 返回类型不能随意改。
     */
    mosquitto_connect_callback_set(client, on_connect);
    mosquitto_subscribe_callback_set(client, on_subscribe);
    mosquitto_message_callback_set(client, on_message);

    /* 发起网络/MQTT 连接；参数为 Broker 地址、端口、Keep Alive 秒数。
     * 返回成功不等于 Broker 已接受 MQTT 连接，后者由 on_connect 报告。
     */
    rc = mosquitto_connect(client, "127.0.0.1", 1883, 60);

    /* 持续处理收发、心跳和回调；1000 是单次网络等待上限（毫秒），
     * 不是整个程序只运行一秒；最后的 1 按 API 要求传入。
     * 本例没有 loop_start，因此回调在调用 loop_forever 的主线程执行。
     */
    if (rc == MOSQ_ERR_SUCCESS)
        rc = mosquitto_loop_forever(client, 1000, 1);

    if (state.received)
    {
        char request_id[64] = {0};
        char command[32] = {0};

        int parse_result =
            parse_remote_command(
                state.payload,
                request_id,
                sizeof(request_id),
                command,
                sizeof(command));

        if (parse_result == 0)
        {
            printf("request_id: %s\n", request_id);
            printf("command: %s\n", command);
        }
        else
        {
            fprintf(stderr, "invalid remote command JSON\n");
            state.failed = 1;
        }
    }

    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "MQTT: %s\n", mosquitto_strerror(rc));

    /* 网络循环已结束，先释放客户端，再清理库级资源。 */
    mosquitto_destroy(client);
    mosquitto_lib_cleanup();

    return rc == MOSQ_ERR_SUCCESS && !state.failed
               ? EXIT_SUCCESS : EXIT_FAILURE;
}
