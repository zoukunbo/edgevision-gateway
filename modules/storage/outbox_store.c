#include "outbox_store.h"
#include "measurement_json.h"

#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#define EDGEVISION_OUTBOX_STORE_SCHEMA_VERSION 3
#define EDGEVISION_STRINGIFY_IMPL(value) #value
#define EDGEVISION_STRINGIFY(value) EDGEVISION_STRINGIFY_IMPL(value)

struct outbox_store
{
    sqlite3 *db;
};

/**
 * @brief 将 SQLite 返回码转换为 Outbox Store 对外使用的结果码
 * @param rc SQLite API 返回的状态码
 * @return SQLITE_OK 映射为 OUTBOX_STORE_OK；忙或锁定映射为
 *         OUTBOX_STORE_BUSY；内存不足映射为 OUTBOX_STORE_NO_MEMORY；
 *         其他状态统一映射为 OUTBOX_STORE_DB_ERROR
 */
static outbox_store_result_t map_sqlite_result(int rc)
{
    if (rc == SQLITE_OK) {
        return OUTBOX_STORE_OK;
    }
    else if (rc == SQLITE_LOCKED || rc == SQLITE_BUSY)
    {
        return OUTBOX_STORE_BUSY;
    }
    else if (rc == SQLITE_NOMEM)
    {
        return OUTBOX_STORE_NO_MEMORY;
    }
    else
    {
        return OUTBOX_STORE_DB_ERROR;
    }
}

/**
 * @brief 开启并校验 SQLite 外键约束
 * @param db 已打开的数据库句柄
 * @return 成功返回 OUTBOX_STORE_OK；设置、查询或校验失败时返回对应错误码
 * @note SQLite 的外键开关是连接级配置，因此每次打开新连接后都需要设置
 */
static outbox_store_result_t outbox_store_enable_and_check_foreign_keys(sqlite3 * db)
{
    int rc;

    // 1, 开启外键
    rc = sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        return map_sqlite_result(rc);
    }

    // 2, 查询校验是否整真正生效 PRAGMA foreign_keys
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA foreign_keys;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return map_sqlite_result(rc);
    }

    // 3. 步进取一行结果
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return map_sqlite_result(rc);
    }

    // 4. 读取整数结果，必须等于 1
    int value = sqlite3_column_int(stmt, 0);
    // 5. 无论成败，只要 stmt 创建成功，必须 finalize
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK)
    {
        return map_sqlite_result(rc);
    }


    if (value != 1)
    {
        return OUTBOX_STORE_DB_ERROR;
    }

    return OUTBOX_STORE_OK;
}

/**
 * @brief 将数据库日志模式设置为 WAL，并校验 SQLite 返回的实际模式
 * @param db 已打开的数据库句柄
 * @return 实际模式为 WAL 时返回 OUTBOX_STORE_OK；否则返回对应错误码
 * @note PRAGMA journal_mode 会返回最终采用的模式，不能只根据 prepare/step 成功
 *       就认定 WAL 已经启用
 */
static outbox_store_result_t outbox_store_set_journal_mode_wal(sqlite3* db)
{
    int rc;
    sqlite3_stmt* stmt = NULL;

    rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode = WAL;", -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        return map_sqlite_result(rc);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return map_sqlite_result(rc);
    }

    const unsigned char* result = sqlite3_column_text(stmt, 0);


    if (result == NULL)
    {
        sqlite3_finalize(stmt);
        return OUTBOX_STORE_DB_ERROR;
    }


    // sqlite3_stricmp() 与 "wal" 比较，避免大小写差异。
    int mode  = sqlite3_stricmp((const char *)result, "wal");

    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK)
    {
        return map_sqlite_result(rc);
    }

    if (mode == 0)
    {
        return OUTBOX_STORE_OK;
    }

    return OUTBOX_STORE_DB_ERROR;
}

/**
 * @brief 读取 SQLite PRAGMA user_version 中保存的 schema 版本号
 * @param db 已打开的数据库句柄
 * @param out_version 输出版本号；查询成功后写入数据库中的 user_version
 * @return 成功返回 OUTBOX_STORE_OK；SQLite 操作失败时返回对应错误码
 */
static outbox_store_result_t outbox_store_read_user_version(sqlite3* db, int* out_version)
{
    *out_version = 0;
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return map_sqlite_result(rc);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return map_sqlite_result(rc);
    }

    int value = sqlite3_column_int(stmt, 0);

    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK)
    {
        return map_sqlite_result(rc);
    }

    *out_version = value;

    return OUTBOX_STORE_OK;
}

/**
 * @brief 在空数据库中以单个事务创建当前版本表、约束、索引及版本号
 * @param db 已打开且尚未初始化 schema 的数据库句柄
 * @return 创建成功返回 OUTBOX_STORE_OK；数据库忙或内存不足返回对应错误码；
 *         其他建表失败返回 OUTBOX_STORE_SCHEMA_ERROR
 */
static outbox_store_result_t outbox_store_create_schema(sqlite3 *db)
{
    /*
     * 用一个 IMMEDIATE 事务原子地创建完整的当前版本 schema：
     * - measurements 保存序列化后的原始测量数据；
     * - outbox 保存投递状态，并通过 measurement_id 与测量数据一一对应；
     * - (state, id) 索引服务于“按 id 读取最早 pending 消息”的查询；
     * - user_version 只在所有 DDL 均成功后随 COMMIT 一起生效。
     *
     * SQLite 会自动回滚未提交事务，但这里在失败路径显式 ROLLBACK，确保当前
     * 连接可以立即继续使用。
     */
    static const char sql[] =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE measurements ("
        "id INTEGER PRIMARY KEY,"
        "payload_json TEXT NOT NULL CHECK(length(payload_json) > 0)"
        ");"
        "CREATE TABLE outbox ("
        "id INTEGER PRIMARY KEY,"
        "measurement_id INTEGER NOT NULL UNIQUE REFERENCES measurements(id),"
        "message_id TEXT NOT NULL UNIQUE "
        "CHECK(length(message_id) = 32),"
        "topic TEXT NOT NULL CHECK(length(topic) > 0),"
        "state TEXT NOT NULL DEFAULT 'pending' "
        "CHECK(state IN ('pending', 'sent')),"
        "attempt_count INTEGER NOT NULL DEFAULT 0,"
        "last_error TEXT,"
        "sent_at_ms INTEGER"
        ");"
        "CREATE INDEX idx_outbox_state_id ON outbox(state, id);"
        "PRAGMA user_version = "
        EDGEVISION_STRINGIFY(EDGEVISION_OUTBOX_STORE_SCHEMA_VERSION)
        ";"
        "COMMIT;";

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK)
    {
        return OUTBOX_STORE_OK;
    }

    outbox_store_result_t result = map_sqlite_result(rc);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);

    if (result == OUTBOX_STORE_BUSY || result == OUTBOX_STORE_NO_MEMORY)
    {
        return result;
    }

    return OUTBOX_STORE_SCHEMA_ERROR;
}

/**
 * @brief 在单个事务中把 v1 schema 迁移到 v2
 * @param db 已打开且当前 user_version 为 1 的数据库句柄
 * @return 迁移成功返回 OUTBOX_STORE_OK；数据库忙或内存不足返回对应错误码；
 *         其他迁移失败返回 OUTBOX_STORE_SCHEMA_ERROR
 */
static outbox_store_result_t outbox_store_migrate_v1_to_v2(sqlite3* db)
{
    /*
     * v1 -> v2 的全部变更放在同一事务中，避免出现“列已增加但版本号未更新”
     * 之类的半迁移状态。旧数据会使用各列的 DEFAULT/NULL 初始值。
     */
    static const char sql[] =
        "BEGIN IMMEDIATE;"
        "ALTER TABLE outbox ADD COLUMN attempt_count INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE outbox ADD COLUMN last_error TEXT;"
        "ALTER TABLE outbox ADD COLUMN sent_at_ms INTEGER;"
        "CREATE INDEX idx_outbox_state_id ON outbox(state, id);"
        "PRAGMA user_version = "
         "2"
        ";"
        "COMMIT;";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK)
    {
        return OUTBOX_STORE_OK;
    }

    outbox_store_result_t result = map_sqlite_result(rc);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);

    if (result == OUTBOX_STORE_BUSY || result == OUTBOX_STORE_NO_MEMORY)
    {
        return result;
    }

    return OUTBOX_STORE_SCHEMA_ERROR;
}

/**
 * @brief 在单个事务中把 v2 schema 迁移到 v3，并为旧记录补齐稳定消息 ID
 */
static outbox_store_result_t outbox_store_migrate_v2_to_v3(sqlite3 *db)
{
    static const char sql[] =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE outbox_v3 ("
        "id INTEGER PRIMARY KEY,"
        "measurement_id INTEGER NOT NULL UNIQUE REFERENCES measurements(id),"
        "message_id TEXT NOT NULL UNIQUE "
        "CHECK(length(message_id) = 32),"
        "topic TEXT NOT NULL CHECK(length(topic) > 0),"
        "state TEXT NOT NULL DEFAULT 'pending' "
        "CHECK(state IN ('pending', 'sent')),"
        "attempt_count INTEGER NOT NULL DEFAULT 0,"
        "last_error TEXT,"
        "sent_at_ms INTEGER"
        ");"
        "INSERT INTO outbox_v3("
        "id, measurement_id, message_id, topic, state, "
        "attempt_count, last_error, sent_at_ms"
        ") SELECT id, measurement_id, lower(hex(randomblob(16))), topic, "
        "state, attempt_count, last_error, sent_at_ms FROM outbox;"
        "DROP TABLE outbox;"
        "ALTER TABLE outbox_v3 RENAME TO outbox;"
        "CREATE INDEX idx_outbox_state_id ON outbox(state, id);"
        "PRAGMA user_version = "
        EDGEVISION_STRINGIFY(EDGEVISION_OUTBOX_STORE_SCHEMA_VERSION)
        ";"
        "COMMIT;";

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK)
    {
        return OUTBOX_STORE_OK;
    }

    outbox_store_result_t result = map_sqlite_result(rc);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    if (result == OUTBOX_STORE_BUSY || result == OUTBOX_STORE_NO_MEMORY)
    {
        return result;
    }
    return OUTBOX_STORE_SCHEMA_ERROR;
}

/**
 * @brief 查询指定名称的普通表是否存在
 * @param db 已打开的数据库句柄
 * @param table_name 待查询的非空表名
 * @param out_exists 输出存在标志；成功时写入 1（存在）或 0（不存在）
 * @return 查询成功返回 OUTBOX_STORE_OK；参数无效或 SQLite 操作失败时返回对应错误码
 * @note 仅在返回 OUTBOX_STORE_OK 时，out_exists 中的值才有效
 */
static outbox_store_result_t outbox_store_table_exists(sqlite3 *db, const char *table_name, int *out_exists)
{
    if (db == NULL || table_name == NULL || table_name[0] == '\0' || out_exists == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    sqlite3_stmt *stmt = NULL;
    outbox_store_result_t result = OUTBOX_STORE_OK;

    /*
     * sqlite_schema 保存数据库对象定义。只查常量 1 可以避免读取无用元数据；
     * ?1 由 sqlite3_bind_text() 绑定，不把 table_name 拼进 SQL，以免名称中的
     * 特殊字符改变查询含义。LIMIT 1 表示这里只关心“是否存在”。
     */
    static const char *sql = "SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = ?1 LIMIT 1;";
    int rc = sqlite3_prepare_v2(db, sql, -1,&stmt, NULL);

    if (rc != SQLITE_OK)
    {
        return map_sqlite_result(rc);
    }

    // 绑定表名，索引1， -1自动计算长度
    rc = sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
     if (rc != SQLITE_OK)
    {
        sqlite3_finalize(stmt);
        return map_sqlite_result(rc);
    }

    // 步骤4.执行step，判断结果
    int exists = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        // 查询到一行 → 表存在
        exists = 1;
    }
    else if (rc == SQLITE_DONE)
    {
        // 无结果 → 表不存在
        exists = 0;
    }
    else
    {
        // 其他错误
        result = map_sqlite_result(rc);
    }

    // 步骤5.清理statement，所有路径必须finalize一次
    int finalize_rc = sqlite3_finalize(stmt);
    if (result == OUTBOX_STORE_OK)
    {
        // 前面无错，但是finalize出错，返回finalize的错误
        result = map_sqlite_result(finalize_rc);
    }

    // 步骤6.全部成功，输出结果
    if(result == OUTBOX_STORE_OK)
    {
        *out_exists = exists;
    }

    return result;
}

/**
 * @brief 校验当前 schema 所需的 measurements 和 outbox 表是否存在
 * @param db 已打开的数据库句柄
 * @return 两张表都存在时返回 OUTBOX_STORE_OK；缺少任一表时返回
 *         OUTBOX_STORE_SCHEMA_ERROR；查询失败时返回对应错误码
 * @note 这里只验证必要表是否存在，不对列定义、约束和索引做完整比对
 */
static outbox_store_result_t verify_current_schema(sqlite3 *db)
{
    outbox_store_result_t result = OUTBOX_STORE_OK;

    int exists = 0;
    result = outbox_store_table_exists(db, "measurements", &exists);

    if (result != OUTBOX_STORE_OK)
    {
        return result;
    }

    if (exists != 1)
    {
        return OUTBOX_STORE_SCHEMA_ERROR;
    }


    result = outbox_store_table_exists(db, "outbox", &exists);

    if (result != OUTBOX_STORE_OK)
    {
        return result;
    }

    if (exists != 1)
    {
        return OUTBOX_STORE_SCHEMA_ERROR;
    }

    return OUTBOX_STORE_OK;
}

/**
 * @brief 打开数据库、配置连接、检查或迁移 schema
 * @param config 打开配置
 * @param out_store 输出句柄；成功时写入有效store指针；失败时 *out_store 将置为 NULL
 * @return 结果码
 */
outbox_store_result_t outbox_store_open(const outbox_store_config_t *config,
                                         outbox_store_t **out_store)
{
    // 1. 校验输出二级指针
    if (out_store == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }
    // 预先清零输出槽，失败时保证 *out_store == NULL
    *out_store = NULL;
    // 2. 校验入参配置
    if (config == NULL || config->db_path == NULL || config->db_path[0] == '\0' || config->busy_timeout_ms < 0)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }
    // 3. 分配store对象内存，calloc初始化为0
    struct outbox_store* store = calloc(1, sizeof(struct outbox_store));
    if (store == NULL)
    {
        return OUTBOX_STORE_NO_MEMORY;
    }


    // 4. 打开sqlite数据库
    int rc = sqlite3_open(config->db_path, &store->db);

    if (rc != SQLITE_OK)
    {
        // 打开失败：清理资源再返回
        sqlite3_close(store->db);
        free(store);
        return map_sqlite_result(rc);
    }

    // 5. 设置数据库busy超时
    rc = sqlite3_busy_timeout(store->db, config->busy_timeout_ms);

    if (rc != SQLITE_OK)
    {
       sqlite3_close(store->db);
       free(store);
       return map_sqlite_result(rc);
    }
    // 开启并校验外键
    outbox_store_result_t result = outbox_store_enable_and_check_foreign_keys(store->db);
    if (result != OUTBOX_STORE_OK)
    {
       sqlite3_close(store->db);
       free(store);
       return result;
    }
    // wal
    result = outbox_store_set_journal_mode_wal(store->db);
    if (result != OUTBOX_STORE_OK)
    {
        sqlite3_close(store->db);
        free(store);
        return result;
    }

    int version;
    result = outbox_store_read_user_version(store->db, &version);
    if (result != OUTBOX_STORE_OK)
    {
        sqlite3_close(store->db);
        free(store);
        return result;
    }

    if (version < 0 || version > EDGEVISION_OUTBOX_STORE_SCHEMA_VERSION)
    {
        sqlite3_close(store->db);
        free(store);
        return OUTBOX_STORE_SCHEMA_ERROR;
    }

    if (version == 0)
    {
        result = outbox_store_create_schema(store->db);
        if (result != OUTBOX_STORE_OK)
        {
            sqlite3_close(store->db);
            free(store);
            return result;
        }

        result = outbox_store_read_user_version(store->db, &version);
        if (result != OUTBOX_STORE_OK)
        {
            sqlite3_close(store->db);
            free(store);
            return result;
        }
    }

    if (version == 1)
    {
        result = outbox_store_migrate_v1_to_v2(store->db);
        if (result != OUTBOX_STORE_OK)
        {
            sqlite3_close(store->db);
            free(store);
            return result;
        }
        result = outbox_store_read_user_version(store->db, &version);
    }

    if (result == OUTBOX_STORE_OK && version == 2)
    {
        result = outbox_store_migrate_v2_to_v3(store->db);
        if (result == OUTBOX_STORE_OK)
        {
            result = outbox_store_read_user_version(store->db, &version);
        }
    }

    if (result != OUTBOX_STORE_OK || version != EDGEVISION_OUTBOX_STORE_SCHEMA_VERSION)
    {
        sqlite3_close(store->db);
        free(store);
        return result != OUTBOX_STORE_OK ? result : OUTBOX_STORE_SCHEMA_ERROR;
    }

    result = verify_current_schema(store->db);
    if (result != OUTBOX_STORE_OK)
    {
        sqlite3_close(store->db);
        free(store);
        return result;
    }

    // 全部成功，写入输出参数
    *out_store = store;

    return OUTBOX_STORE_OK;

}
/**
 * @brief 释放 item 内部 topic、payload_json 字符串内存，清空指针
 * @param item 允许传入空指针；item->topic/payload_json 为NULL时调用也安全
 * @note 禁止调用方手动 free(item->topic) / free(item->payload_json)，统一使用此函数释放
 */
void outbox_store_free_outbox_item(outbox_item_t *item)
{
    if (item == NULL)
    {
        return;
    }

    free(item->topic);
    free(item->payload_json);
    memset(item, 0, sizeof(*item));
}

/**
 * @brief 结束事务并释放 SQLite 资源
 * @param store 待关闭实例；关闭后该指针永久失效
 * @return 结果码
 */
outbox_store_result_t outbox_store_close(outbox_store_t *store)
{
    if (store == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    int rc = sqlite3_close(store->db);

    if (rc == SQLITE_OK)
    {
        free(store);
        return OUTBOX_STORE_OK;
    }

    return map_sqlite_result(rc);
}

/**
 * @brief 校验保存操作的输入参数，并把 Measurement 序列化为 JSON
 * @param store 已打开的存储实例
 * @param meas 待序列化的测量数据
 * @param topic 非空的消息主题；本函数只校验，不接管其所有权
 * @param out_json 输出新分配的 JSON 字符串；失败时保证为 NULL
 * @return 成功返回 OUTBOX_STORE_OK；参数、内存分配或序列化失败时返回对应错误码
 * @note 成功后 JSON 所有权交给调用方，必须使用 measurement_json_free() 释放
 */
static outbox_store_result_t prepare_save_payload(outbox_store_t *store, const measurement_t *meas, const char *topic, char **out_json)
{
    if (out_json == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }
    *out_json = NULL;

    if (store == NULL || store->db == NULL || meas == NULL || topic == NULL || topic[0] == '\0')
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    char *json = NULL;
    size_t json_len  = 0;
    measurement_json_result_t result;
    result = measurement_to_json(meas, &json, &json_len);

    if (result != MEASUREMENT_JSON_OK)
    {
        if (result == MEASUREMENT_JSON_ALLOCATION_ERROR)
        {
            return OUTBOX_STORE_NO_MEMORY;
        }
        return OUTBOX_STORE_SERIALIZATION_ERROR;
    }

    *out_json = json;
    return OUTBOX_STORE_OK;
}

/**
 * @brief 在一个事务中保存 Measurement 和 pending Outbox
 * @param store 存储实例
 * @param meas 待保存测量数据
 * @param topic 必须非空：Store只在调用期间读取并复制，所有权仍归调用方
 * @return 结果码
 */
outbox_store_result_t outbox_store_save_measurement(outbox_store_t *store, const measurement_t * meas, const char *topic)
{
    char *json_str = NULL;
    outbox_store_result_t result = OUTBOX_STORE_OK;

    result = prepare_save_payload(store, meas, topic, &json_str);
    if (result != OUTBOX_STORE_OK)
    {
        return result;
    }

    int transcation_started = 0;
    int rc = sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
    {
        measurement_json_free(json_str);
        return map_sqlite_result(rc);
    }
    transcation_started = 1;

    /*
     * 先插入 measurements，再用 sqlite3_last_insert_rowid() 取得本连接刚生成的
     * 主键。?1 对应下面绑定的 json_str；SQLITE_TRANSIENT 要求 SQLite 在
     * bind 返回前复制字符串，因此本函数可在事务结束后释放 json_str。
     */
    const char *sql = "INSERT INTO measurements(payload_json) VALUES(?1);";
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_text(stmt, 1, json_str, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    sqlite3_int64 measuerment_id = sqlite3_last_insert_rowid(store->db);
    rc = sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    /*
     * 为同一条 measurement 创建待投递记录：?1 是刚取得的 measurement id，
     * ?2 是 topic。state 显式设为 pending，虽然表定义也提供了相同默认值，
     * 这里写出可让 Outbox 的入队语义更直观。
     * 两次 INSERT 位于同一事务中，任一步失败都会回滚，不会留下孤立记录。
     */
    sql = "INSERT INTO outbox(measurement_id, message_id, topic, state) "
          "VALUES(?1, lower(hex(randomblob(16))), ?2, 'pending');";
    rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_int64(stmt, 1, measuerment_id);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_text(stmt, 2, topic, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }



    rc = sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }
    transcation_started = 0;

cleanup:

    if (transcation_started == 1)
    {
        sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    }

    if (stmt != NULL)
    {
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    measurement_json_free(json_str);

    return result;
}

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
outbox_store_result_t outbox_store_read_earliest_pending(outbox_store_t *store, outbox_item_t *out_item)
{
    if (out_item == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }
    memset(out_item, 0, sizeof(*out_item));
    if (store == NULL || store->db == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    outbox_item_t item;
    memset(&item, 0,sizeof(item));
    sqlite3_stmt *stmt = NULL;
    outbox_store_result_t result = OUTBOX_STORE_OK;

    /*
     * JOIN 根据 measurement_id 取回消息正文；WHERE 只保留待发送记录；
     * ORDER BY id ASC + LIMIT 1 选出最早入队的一条。SELECT 列的顺序必须与
     * 下方 sqlite3_column_*() 使用的 0..5 列索引保持一致。
     */
    const char *sql =
        "SELECT o.id, o.measurement_id, o.attempt_count, o.message_id,"
        "o.topic, m.payload_json "
        "FROM outbox AS o "
        "JOIN measurements AS m ON m.id = o.measurement_id "
        "WHERE o.state = 'pending' "
        "ORDER BY o.id ASC "
        "LIMIT 1;";

    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        result =  map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        if (rc == SQLITE_DONE)
        {
            result =  OUTBOX_STORE_EMPTY;
            goto cleanup;
        }
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    item.id =  sqlite3_column_int64(stmt, 0);
    item.measurement_id = sqlite3_column_int64(stmt, 1);
    int64_t attempt_count = sqlite3_column_int64(stmt, 2);
    if (attempt_count >= 0)
    {
        item.attempt_count = (uint64_t)attempt_count;
    }
    else
    {
        result = OUTBOX_STORE_DB_ERROR;
        goto cleanup;
    }

    /*
     * sqlite3_column_text() 返回 SQLite 管理的内部缓冲区。该指针最晚会在下一次
     * sqlite3_step()/sqlite3_finalize() 后失效，调用方不能持有它。因此不能写成
     * item.topic = (char *)raw_topic，而要在 statement 仍有效时复制到自有内存。
     */
    const unsigned char *raw_message_id = sqlite3_column_text(stmt, 3);
    if (raw_message_id == NULL ||
        strlen((const char *)raw_message_id) != OUTBOX_MESSAGE_ID_HEX_LENGTH)
    {
        result = OUTBOX_STORE_DB_ERROR;
        goto cleanup;
    }
    memcpy(item.message_id,
           raw_message_id,
           OUTBOX_MESSAGE_ID_HEX_LENGTH + 1u);

    const unsigned char *raw_topic = sqlite3_column_text(stmt, 4);
    if (raw_topic == NULL)
    {
        int sqlite_err = sqlite3_errcode(store->db);
        if (sqlite_err == SQLITE_NOMEM)
        {
           result = OUTBOX_STORE_NO_MEMORY;
        }
        else
        {
            result = OUTBOX_STORE_DB_ERROR;
        }
        goto cleanup;
    }
    const char* src = (const char*) raw_topic;
    size_t str_len = strlen(src);

    char* topic_buf = malloc(str_len + 1);
    if (topic_buf == NULL)
    {
        result = OUTBOX_STORE_NO_MEMORY;
        goto cleanup;
    }
    /* 连同结尾的 '\0' 一起复制，使 item.topic 成为独立、完整的 C 字符串。 */
    memcpy(topic_buf, src, str_len + 1);

    /* 从这里起临时 item 拥有 topic_buf；任何后续错误均由 cleanup 统一释放。 */
    item.topic = topic_buf;

    /* payload_json 与 topic 相同，也必须脱离 SQLite statement 的生命周期。 */
    const unsigned char* raw_json = sqlite3_column_text(stmt, 5);
    if (raw_json == NULL)
    {
        int sqlite_err = sqlite3_errcode(store->db);
        if (sqlite_err == SQLITE_NOMEM)
        {
           result = OUTBOX_STORE_NO_MEMORY;
        }
        else
        {
            result = OUTBOX_STORE_DB_ERROR;
        }
        goto cleanup;
    }

    const char * json_src = (const char*)raw_json;
    size_t json_len = strlen(json_src);
    char *json_buf = malloc(json_len + 1);
    if (json_buf == NULL)
    {
        result = OUTBOX_STORE_NO_MEMORY;
        goto cleanup;
    }
    /* 同样复制末尾 '\0'；成功返回后由调用方通过 free_outbox_item() 释放。 */
    memcpy(json_buf, json_src, json_len + 1);
    item.payload_json = json_buf;

    rc = sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    /*
     * 浅拷贝结构体即可完成所有权移交：两个指针指向刚分配的独立缓冲区。
     * 此后不能再释放临时 item，否则 out_item 中的指针会悬空。调用方最终必须
     * 调用 outbox_store_free_outbox_item(out_item)。
     */
    *out_item = item;
    return result;

cleanup:
    if (stmt != NULL)
    {
        rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if ((result == OUTBOX_STORE_OK || result == OUTBOX_STORE_EMPTY) && rc != SQLITE_OK)
        {
           result = map_sqlite_result(rc);
        }
    }

    /* 失败时临时 item 仍持有已成功分配的字符串，由这里一次性清理。 */
    outbox_store_free_outbox_item(&item);

    return result;
}
/**
 * @brief PUBACK 后把指定 pending 更新为 sent
 *        成功时： state = 'sent'  写入 sent_at_ms 将 last_error 清为 NULL 保留 attempt_count，表示历史失败次数
 * @param store 存储实例
 * @param outbox_id 待标记消息ID
 * @param sent_at_ms 发送成功的时间戳(毫秒)，由上层MQTT工作线程提供
 * @note 幂等约定：该记录已经标记sent → 返回OK；id不存在 → 返回 OUTBOX_STORE_NOT_FOUND
 * @return 结果码
 */
outbox_store_result_t outbox_store_mark_sent(
    outbox_store_t *store,
    int64_t outbox_id,
    int64_t sent_at_ms)
{
    if (store == NULL || store->db == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    sqlite3_stmt *stmt = NULL;
    outbox_store_result_t result = OUTBOX_STORE_OK;
    int affected_rows = 0;

    /*
     * ?1 = outbox_id，?2 = sent_at_ms。
     * 无论原状态如何都把 state 设为 sent；两个 CASE 只在首次 pending -> sent 时
     * 记录发送时间并清空错误。重复调用不会覆盖首次 sent_at_ms，因而具备幂等性。
     * WHERE 仅按 id 匹配，sqlite3_changes()==0 表示该 id 不存在。
     */
    const char *sql =
        "UPDATE outbox "
        " SET state = 'sent', "
        " sent_at_ms = CASE WHEN state = 'pending' THEN ?2 ELSE sent_at_ms END, "
        " last_error = CASE WHEN state = 'pending' THEN NULL ELSE last_error END "
        " WHERE id = ?1;";

    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_int64(stmt, 1, outbox_id);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_int64(stmt, 2, sent_at_ms);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    affected_rows = sqlite3_changes(store->db);

cleanup:
    if (stmt != NULL)
    {
        rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if (result == OUTBOX_STORE_OK && rc != SQLITE_OK)
        {
            result = map_sqlite_result(rc);
        }
    }

    if (result != OUTBOX_STORE_OK)
    {
        return result;
    }

    return affected_rows == 0
               ? OUTBOX_STORE_NOT_FOUND
               : OUTBOX_STORE_OK;
}

/**
 * @brief 返回 Measurement、pending、sent 数量
 * @param store 存储实例
 * @param out_stats 统计结果输出
 * @return 结果码
 */
outbox_store_result_t outbox_store_get_stats(
    outbox_store_t *store,
    outbox_store_stats_t *out_stats)
{
    if (out_stats == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    if (store == NULL || store->db == NULL)
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    outbox_store_stats_t stats = {0};
    sqlite3_stmt *stmt = NULL;
    outbox_store_result_t result = OUTBOX_STORE_OK;

    /*
     * 三个标量子查询各自始终返回一行 COUNT(*)，因此整条 SELECT 也应恰好返回
     * 一行三列。列顺序与下面 measurement_total/pending_count/sent_count 对应。
     */
    const char *sql =
        "SELECT (SELECT COUNT(*) FROM measurements),"
        " (SELECT COUNT(*) FROM outbox WHERE state = 'pending'),"
        " (SELECT COUNT(*) FROM outbox WHERE state = 'sent');";

    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW)
    {
        /* COUNT 查询即使表为空也应返回一行。 */
        result = (rc == SQLITE_DONE)
                     ? OUTBOX_STORE_DB_ERROR
                     : map_sqlite_result(rc);
        goto cleanup;
    }

    sqlite3_int64 measurement_total = sqlite3_column_int64(stmt, 0);
    sqlite3_int64 pending_count = sqlite3_column_int64(stmt, 1);
    sqlite3_int64 sent_count = sqlite3_column_int64(stmt, 2);

    if (measurement_total < 0 || pending_count < 0 || sent_count < 0)
    {
        result = OUTBOX_STORE_DB_ERROR;
        goto cleanup;
    }

    stats.measurement_total = (uint64_t)measurement_total;
    stats.pending_count = (uint64_t)pending_count;
    stats.sent_count = (uint64_t)sent_count;

cleanup:
    if (stmt != NULL)
    {
        rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if (result == OUTBOX_STORE_OK && rc != SQLITE_OK)
        {
            result = map_sqlite_result(rc);
        }
    }

    if (result == OUTBOX_STORE_OK)
    {
        *out_stats = stats;
    }
    return result;
}

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
outbox_store_result_t outbox_store_record_delivery_failure(
    outbox_store_t *store,
    int64_t outbox_id,
    const char *error_text)
{
    if (store == NULL || store->db == NULL ||
        error_text == NULL || error_text[0] == '\0')
    {
        return OUTBOX_STORE_INVALID_ARGUMENT;
    }

    sqlite3_stmt *stmt = NULL;
    outbox_store_result_t result = OUTBOX_STORE_OK;
    int affected_rows = 0;

    /*
     * ?1 = outbox_id，?2 = error_text。仅 pending 消息累加失败次数并更新错误；
     * sent 消息虽然能被 WHERE 命中，但 CASE 会保留原值，实现“已发送则不修改”。
     * WHERE 仍按 id 匹配，因此 changes()==0 用来区分 id 不存在。
     */
    const char *sql =
        "UPDATE outbox "
        " SET attempt_count = CASE WHEN state = 'pending' THEN attempt_count + 1 ELSE attempt_count END, "
        " last_error = CASE WHEN state = 'pending' THEN ?2 ELSE last_error END "
        " WHERE id = ?1;";

    int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_int64(stmt, 1, outbox_id);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_bind_text(stmt, 2, error_text, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
    {
        result = map_sqlite_result(rc);
        goto cleanup;
    }

    affected_rows = sqlite3_changes(store->db);

cleanup:
    if (stmt != NULL)
    {
        rc = sqlite3_finalize(stmt);
        stmt = NULL;
        if (result == OUTBOX_STORE_OK && rc != SQLITE_OK)
        {
            result = map_sqlite_result(rc);
        }
    }

    if (result != OUTBOX_STORE_OK)
    {
        return result;
    }

    return affected_rows == 0
               ? OUTBOX_STORE_NOT_FOUND
               : OUTBOX_STORE_OK;
}
