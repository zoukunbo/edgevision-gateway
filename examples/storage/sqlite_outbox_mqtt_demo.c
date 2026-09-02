/*
 * D40真实出口练习：最早pending Outbox -> MQTT QoS 1 PUBACK -> sent。
 * 发布与状态更新不是原子操作：PUBACK后、UPDATE前崩溃会导致重启后重复发布，
 * 因此这里只承诺at-least-once方向，不承诺exactly-once。
 * 输入是历史Measurement回放，不访问UART/GPIO，也不查询STM32。
 */
#include "measurement_json.h"
#include "mqtt_publisher.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MQTT_OUTBOX_EXERCISE_READY 1
#define OUTBOX_NO_PENDING (-1001)
#define OUTBOX_DB_ERROR (-1002)
#define OUTBOX_PUBLISH_UNCONFIRMED (-1003)

typedef struct {
    /* load函数在finalize前复制三列，避免跨SQLite语句生命周期借用内存。 */
    sqlite3_int64 id;
    char topic[160];
    char payload[4096];
} pending_item_t;

static int load_oldest_pending(sqlite3 *db, pending_item_t *item)
{
    /* 每次只处理一条，ORDER BY id使重启后的处理顺序可预测。 */
    sqlite3_stmt *stmt=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT o.id,o.topic,m.payload_json FROM outbox o"
        " JOIN measurements m ON m.id=o.measurement_id"
        " WHERE o.state='pending' ORDER BY o.id LIMIT 1;",-1,&stmt,NULL);
    if (rc==SQLITE_OK) rc=sqlite3_step(stmt);
    if (rc==SQLITE_DONE) rc=OUTBOX_NO_PENDING;
    if (rc==SQLITE_ROW) {
        const unsigned char *topic=sqlite3_column_text(stmt,1);
        const unsigned char *payload=sqlite3_column_text(stmt,2);
        if (topic==NULL || payload==NULL ||
            strlen((const char *)topic)>=sizeof(item->topic) ||
            strlen((const char *)payload)>=sizeof(item->payload)) rc=SQLITE_TOOBIG;
        else {
            item->id=sqlite3_column_int64(stmt,0);
            strcpy(item->topic,(const char *)topic);
            strcpy(item->payload,(const char *)payload);
            rc=SQLITE_OK;
        }
    }
    {
        int finish_rc=sqlite3_finalize(stmt);
        if (rc==SQLITE_OK) rc=finish_rc;
    }
    return rc;
}

static int mark_sent(sqlite3 *db, sqlite3_int64 id)
{
    /* 条件更新和changes检查防止重复调用被误报为本次成功。 */
    sqlite3_stmt *stmt=NULL;
    int rc=sqlite3_prepare_v2(db,
        "UPDATE outbox SET state='sent' WHERE id=?1 AND state='pending';",
        -1,&stmt,NULL);
    if (rc==SQLITE_OK) rc=sqlite3_bind_int64(stmt,1,id);
    if (rc==SQLITE_OK) {
        rc=sqlite3_step(stmt);
        if (rc==SQLITE_DONE) rc=SQLITE_OK;
    }
    if (rc==SQLITE_OK && sqlite3_changes(db)!=1) rc=SQLITE_NOTFOUND;
    {
        int finish_rc=sqlite3_finalize(stmt);
        if (rc==SQLITE_OK) rc=finish_rc;
    }
    return rc;
}

/* 解析库内JSON，核对Outbox主题，再复用现有同步QoS1发布器。
 * MQTT_PUBLISHER_OK表示收到匹配PUBACK；不表示订阅者已处理。
 */
static mqtt_publisher_result_t publish_real(const pending_item_t *item, int broker_port)
{
    measurement_t measurement;
    if (measurement_from_json(item->payload,strlen(item->payload),&measurement)!=
        MEASUREMENT_JSON_OK) return MQTT_PUBLISHER_SERIALIZATION_ERROR;
    char expected_topic[160];
    int n=snprintf(expected_topic,sizeof(expected_topic),
        "edgevision/v1/devices/%s/measurements",measurement.device_id);
    if (n<0 || (size_t)n>=sizeof(expected_topic) ||
        strcmp(expected_topic,item->topic)!=0) return MQTT_PUBLISHER_INVALID_ARGUMENT;
    const mqtt_publisher_config_t config={
        .host="127.0.0.1", .port=broker_port,
        .client_id="edgevision-outbox-mqtt-demo",
        .topic_prefix="edgevision/v1/devices",
        .keepalive_seconds=30,
        .reconnect_delay_seconds=1u,
        .reconnect_delay_max_seconds=8u
    };
    mqtt_publisher_t *publisher=mqtt_publisher_create(&config);
    if (publisher==NULL) return MQTT_PUBLISHER_LIBRARY_ERROR;
    mqtt_publisher_result_t result=mqtt_publisher_start(publisher);
    if (result==MQTT_PUBLISHER_OK)
        result=mqtt_publisher_wait_connected(publisher,5);
    if (result==MQTT_PUBLISHER_OK)
        result=mqtt_publisher_publish(publisher,&measurement,5);
    mqtt_publisher_destroy(publisher);
    return result;
}

/* 本轮关键行为：只有真实mqtt_publisher返回PUBACK确认，才标记sent。 */
static int deliver_pending_mqtt(sqlite3 *db, int broker_port)
{
    pending_item_t item={0};
    int rc=OUTBOX_DB_ERROR;

    rc = load_oldest_pending(db, &item);

    if (rc == OUTBOX_NO_PENDING)
    {
        printf("NO_PENDING \n");
        return OUTBOX_NO_PENDING;
    }

    if (rc != SQLITE_OK)
    {
        printf("load_oldest_pending faild: %d \n", rc);
        return OUTBOX_DB_ERROR;
    }


    int publish_result = publish_real(&item, broker_port);

    if (publish_result != MQTT_PUBLISHER_OK)
    {
        printf("PUBLISH_UNCONFIRMED status=<%d> \n", publish_result);
        return OUTBOX_PUBLISH_UNCONFIRMED;
    }

    rc = mark_sent(db, item.id);
    if (rc != SQLITE_OK)
    {
        printf("PUBACK_THEN_MARKED_SENT fail: <%lld>, status = <%d>\n", item.id, rc);
        return OUTBOX_DB_ERROR;
    }

    printf("PUBACK_THEN_MARKED_SENT id=<%lld>\n", item.id);

    return rc;
}

static int print_status(sqlite3 *db)
{
    sqlite3_stmt *stmt=NULL;
    int rc=sqlite3_prepare_v2(db,
        "SELECT (SELECT count(*) FROM measurements),"
        " (SELECT count(*) FROM outbox WHERE state='pending'),"
        " (SELECT count(*) FROM outbox WHERE state='sent');",-1,&stmt,NULL);
    if (rc==SQLITE_OK) rc=sqlite3_step(stmt);
    if (rc==SQLITE_ROW) {
        printf("measurement_count=%d pending_count=%d sent_count=%d\n",
            sqlite3_column_int(stmt,0),sqlite3_column_int(stmt,1),sqlite3_column_int(stmt,2));
        rc=SQLITE_OK;
    }
    {
        int finish_rc=sqlite3_finalize(stmt);
        if (rc==SQLITE_OK) rc=finish_rc;
    }
    return rc;
}

int main(int argc,char **argv)
{
    int deliver=argc==3 && strcmp(argv[1],"deliver-mqtt")==0;
    int unavailable=argc==3 && strcmp(argv[1],"deliver-unavailable")==0;
    int status=argc==3 && strcmp(argv[1],"status")==0;
    if (!deliver && !unavailable && !status) {
        fprintf(stderr,"Usage: %s deliver-mqtt|deliver-unavailable|status DATABASE\n",argv[0]);
        return EXIT_FAILURE;
    }
    if (!MQTT_OUTBOX_EXERCISE_READY) {
        fputs("EXERCISE INCOMPLETE: fill Q1-Q3, then set MQTT_OUTBOX_EXERCISE_READY=1. No database or MQTT opened.\n",stderr);
        return EXIT_FAILURE;
    }
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(argv[2],&db,
        (deliver || unavailable) ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY,NULL);
    if (rc==SQLITE_OK)
        rc=(deliver || unavailable) ?
            deliver_pending_mqtt(db, unavailable ? 1884 : 1883) : print_status(db);
    else fprintf(stderr,"database open failed: rc=%d %s\n",rc,sqlite3_errstr(rc));
    int close_rc=sqlite3_close(db);
    return rc==0 && close_rc==SQLITE_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
