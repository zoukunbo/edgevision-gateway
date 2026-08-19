#ifndef BLOCKING_QUEUE_H
#define BLOCKING_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/*
 * TODO 1
 *
 * 定义返回值枚举
 *
 * 至少区分：
 *
 * 成功
 * 超时
 * 已关闭
 * 参数错误
 */
typedef enum{
    BQ_OK = 0,
    BQ_CLOSED,
    BQ_TIMEOUT,
    BQ_INVALID, // 参数传错了

    BQ_NOMEM, // malloc 失败
    BQ_SYSTEM //pthread 初始化等系统操作失败
} bq_result_t;

/*
 * TODO 2
 *
 * 定义 blocking_queue_t
 *
 * 思考需要：
 *
 * 消息存储区域
 * capacity
 * size
 * head
 * tail
 * closed
 *
 * mutex
 * not_empty
 * not_full
 */
typedef struct
{
    unsigned char *buffer; // 实际保存消息

    size_t element_size; // 每个元素占多少字节

    size_t capacity; // 队列最大容量
    size_t size; // 消息数  0 <= size <= capacity

    size_t head; // 下一个读取的位置
    size_t tail; // 下一个写入的位置

    int closed;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_queue_t;


/** 
 *
 * 声明初始化函数
 */
bq_result_t bq_init(bounded_queue_t *q, size_t capacity, size_t element_size);


/*
 *
 * 声明 push
 *
 * timeout_ms < 0 永久等待
 * 
 * timeout_ms == 0 不等待
 * 
 * timeout_ms > 0
 * 最多等待指定毫秒数
 */
bq_result_t bq_push(bounded_queue_t *q, const void *item, int timeout_ms);

/*
 *
 * 声明 pop
 *
 * 参数需要表达：
 * queue
 * 输出 message
 * timeout
 */
bq_result_t bq_pop(bounded_queue_t *q, void *out, int timeout_ms);


/*
 * TODO 6
 *
 * 声明 close
 */
bq_result_t bq_close(bounded_queue_t *q);

/*
 * TODO 7
 *
 * 声明 size
 */
bq_result_t bq_size(bounded_queue_t *q, size_t *out_size);

/*
 * TODO 8
 *
 * 声明 destroy
 */
bq_result_t bq_destroy(bounded_queue_t *q);

#endif