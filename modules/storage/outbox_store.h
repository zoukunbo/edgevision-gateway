#ifndef EDGEVISION_STORAGE_OUTBOX_STORE_H
#define EDGEVISION_STORAGE_OUTBOX_STORE_H
#include "measurement.h"
#include <stdint.h>

#define OUTBOX_MESSAGE_ID_HEX_LENGTH 32u
#define OUTBOX_MESSAGE_ID_CAPACITY (OUTBOX_MESSAGE_ID_HEX_LENGTH + 1u)

/**
 * @file outbox_store.h
 *
 * 【全局生命周期 & 线程契约】
 * 1. 线程安全约束：同一个 outbox_store_t 实例 **不可被多个线程并发调用**。
 *    多线程场景建议每个线程独立打开Store，或由调用方自行添加互斥锁保护。
 * 2. 对象所有权：outbox_store_open 成功返回后，调用方最终必须调用 outbox_store_close()释放资源。
 * 3. 关闭规则：outbox_store_close 执行成功后，原 store 指针永久失效，禁止再传入任何store接口。
 */

typedef enum
{
    OUTBOX_STORE_OK = 0,                // 操作成功
    OUTBOX_STORE_EMPTY = 1,             // 没有 pending，属于正常状态
    OUTBOX_STORE_NOT_FOUND = -1,        // 指定 Outbox id 不存在
    OUTBOX_STORE_INVALID_ARGUMENT = -2, // 空指针或非法配置
    OUTBOX_STORE_BUSY = -3,             // 等待后数据库仍被占用
    OUTBOX_STORE_DB_ERROR = -4,         // 其他 SQLite 错误
    OUTBOX_STORE_SCHEMA_ERROR = -5,     // 版本不支持或迁移失败
    OUTBOX_STORE_SERIALIZATION_ERROR = -6, // Measurement 无效或 JSON 生成失败
    OUTBOX_STORE_NO_MEMORY = -7         // 内存分配失败
} outbox_store_result_t;
// 返回码约定：成功=0；正常非错误状态=正数；失败错误=负数

typedef struct outbox_store outbox_store_t;

typedef struct
{
    int64_t id;
    int64_t measurement_id;
    uint64_t attempt_count;
    char message_id[OUTBOX_MESSAGE_ID_CAPACITY];
    char* topic;
    char* payload_json;
} outbox_item_t;

typedef struct {
    uint64_t measurement_total;
    uint64_t pending_count;
    uint64_t sent_count;
} outbox_store_stats_t;

typedef struct
{
    const char* db_path;
    int busy_timeout_ms;
} outbox_store_config_t;

/**
 * @brief 打开数据库、配置连接、检查或迁移 schema
 * @param config 打开配置
 * @param out_store 输出句柄；成功时写入有效store指针；失败时 *out_store 将置为 NULL
 * @return 结果码
 */
outbox_store_result_t outbox_store_open(const outbox_store_config_t *config, outbox_store_t **out_store);

/**
 * @brief 释放 item 内部 topic、payload_json 字符串内存，清空指针
 * @param item 允许传入空指针；item->topic/payload_json 为NULL时调用也安全
 * @note 禁止调用方手动 free(item->topic) / free(item->payload_json)，统一使用此函数释放
 */
void outbox_store_free_outbox_item(outbox_item_t *item);

/**
 * @brief 结束事务并释放 SQLite 资源
 * @param store 待关闭实例；关闭后该指针永久失效
 * @return 结果码
 */
outbox_store_result_t outbox_store_close(outbox_store_t *store);

/**
 * @brief 在一个事务中保存 Measurement 和 pending Outbox
 * @param store 存储实例
 * @param meas 待保存测量数据
 * @param topic 必须非空：Store只在调用期间读取并复制，所有权仍归调用方
 * @return 结果码
 */
outbox_store_result_t outbox_store_save_measurement(outbox_store_t *store, const measurement_t * meas, const char *topic);

/**
 * @brief 读取最早一条待发送的pending消息
 *
 * 【前置条件】调用前 out_item 必须已经清零，或已调用 outbox_store_free_outbox_item()。
 *              本函数**不会释放 out_item 中原有的残留内存**；携带旧字符串传入会造成内存泄漏。
 *
 * @param store 存储实例
 * @param out_item 输出参数，成功返回后item内部topic、payload_json内存所有权归调用方
 *                 EMPTY 或失败时，out_item 所有字段均为零，不只是两个指针为 NULL
 *
 * @return
 *    OUTBOX_STORE_OK:成功读取，item有效，用完必须调用 outbox_store_free_outbox_item()释放
 *    OUTBOX_STORE_EMPTY:没有pending消息，item内字符串为NULL，无需释放
 *    其他值:读取失败，item内字符串为NULL，无需释放
 */
outbox_store_result_t outbox_store_read_earliest_pending(outbox_store_t *store, outbox_item_t *out_item);

/**
 * @brief PUBACK 后把指定 pending 更新为 sent
 *        成功时： state = 'sent'  写入 sent_at_ms 将 last_error 清为 NULL 保留 attempt_count，表示历史失败次数
 * @param store 存储实例
 * @param outbox_id 待标记消息ID
 * @param sent_at_ms 发送成功的时间戳(毫秒)，由上层MQTT工作线程提供
 * @note 幂等约定：该记录已经标记sent → 返回OK；id不存在 → 返回 OUTBOX_STORE_NOT_FOUND
 * @return 结果码
 */
outbox_store_result_t outbox_store_mark_sent(outbox_store_t *store, int64_t outbox_id, int64_t sent_at_ms);

/**
 * @brief 返回 Measurement、pending、sent 数量
 * @param store 存储实例
 * @param out_stats 统计结果输出
 * @return 结果码
 */
outbox_store_result_t outbox_store_get_stats(outbox_store_t *store, outbox_store_stats_t* out_stats);

/**
 * @brief 记录一次投递失败
 *        ID 不存在： OUTBOX_STORE_NOT_FOUND
 *        已经是sent: 不修改数据返回 OUTBOX_STORE_OK
 *        error_text == NULL 或空字符串：OUTBOX_STORE_INVALID_ARGUMENT
 * @param store 存储实例
 * @param outbox_id 失败消息ID
 * @param error_text 失败原因文本
 * @note 行为：attempt_count += 1，保存错误信息；消息状态仍然保持 pending，等待重试
 * @return 结果码
 */
outbox_store_result_t outbox_store_record_delivery_failure(outbox_store_t *store, int64_t outbox_id, const char* error_text);

#endif // !EDGEVISION_STORAGE_OUTBOX_STORE_H
