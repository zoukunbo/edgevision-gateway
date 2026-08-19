#include "blocking_queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

static int make_deadline(int timeout_ms, struct timespec *deadline)
{
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0)
    {
        return -1;
    }
    
    // 1 秒 = 1 000 000 000 纳秒
    // 1 毫秒 = 1 000 000 纳秒
    deadline->tv_sec += timeout_ms / 1000;// 秒
    deadline->tv_nsec += (timeout_ms % 1000) * 1000000L;// 纳秒

    if (deadline->tv_nsec >= 1000000000L)
    {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }

    return 0;
}

/** 
 *
 * 声明初始化函数
 */
bq_result_t bq_init(bounded_queue_t *q, size_t capacity, size_t element_size)
{
    int rc;

    // 检查参数
    if (q == NULL)
    {
        return BQ_INVALID;
    }

    if (capacity == 0) {
        return  BQ_INVALID;
    }
    
    if (element_size == 0)
    {
        return BQ_INVALID;
    }
    
    q->element_size = element_size;
    // 给 buffer 分配内存
    q->buffer = malloc(element_size * capacity);

    if (q->buffer == NULL)
    {
        perror("malloc");
        return BQ_NOMEM;
    }

    // 初始化普通字段
    q->capacity = capacity;
    q->size = 0;
    q->head = 0;
    q->tail = 0;
    q->closed = 0;

    // 初始化 mutex
    rc = pthread_mutex_init(&q->mutex, NULL);
    if (rc != 0) {
        goto FAIL_BUFFER;
    }
    // 初始化 not_empty
    rc = pthread_cond_init(& q->not_empty,NULL);
    if (rc != 0) {
        goto FAIL_MUTEX;
    }
    // 初始化 not_full
    rc = pthread_cond_init(& q->not_full,NULL);
    if (rc != 0) {
        goto FAIL_NOT_EMPTY;
    }

    return BQ_OK;

FAIL_NOT_EMPTY:
    pthread_cond_destroy(&q->not_empty);

FAIL_MUTEX:
    pthread_mutex_destroy(&q->mutex);
    
FAIL_BUFFER:
    free(q->buffer);

    q->buffer = NULL;

    // 返回 BQ_OK
    return BQ_SYSTEM;
}


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
bq_result_t bq_push(bounded_queue_t *q, const void *item, int timeout_ms)
{
    int rc;
    
    if (q == NULL)
    {
        return BQ_INVALID;
    }

    if (item == NULL)
    {
        return BQ_INVALID;
    }
    

    pthread_mutex_lock(&q->mutex);

    if (q->closed)
    {
        pthread_mutex_unlock(&q->mutex);
        return BQ_CLOSED;
    }

    struct timespec deadline; // 最晚等到几点几分几秒。

    if (timeout_ms > 0)
    {
        if (make_deadline(timeout_ms, &deadline) != 0)
        {
            pthread_mutex_unlock(&q->mutex);
            return BQ_SYSTEM;
        }
        
    }

    while (q->size == q->capacity && !q->closed)
    {
        if (timeout_ms == 0)
        {
            pthread_mutex_unlock(&q->mutex);
            return BQ_TIMEOUT;
        }
        else if (timeout_ms < 0)
        {
            rc = pthread_cond_wait(&q->not_full, &q->mutex);
            
            if (rc != 0)
            {
                pthread_mutex_unlock(&q->mutex);
                return BQ_SYSTEM;
            }
        }
        else
        {
            rc = pthread_cond_timedwait(&q->not_full, &q->mutex, &deadline);
            
            if (rc == ETIMEDOUT)
            {
                pthread_mutex_unlock(&q->mutex);
                return BQ_TIMEOUT;
            }
            
            if (rc != 0)
            {
                pthread_mutex_unlock(&q->mutex);
                return BQ_SYSTEM;
            }
        }
    }

    if (q->closed == 1)
    {
        pthread_mutex_unlock(&q->mutex);
        return BQ_CLOSED;
    }
    

    memcpy(q->buffer + q->tail * q->element_size, item, q->element_size);

    q->tail = (q->tail + 1) % q->capacity;

    assert(q->size < q->capacity);

    q->size++;

    assert(q->size <= q->capacity);

    pthread_cond_signal(&q->not_empty);

    pthread_mutex_unlock(&q->mutex);

    return BQ_OK;
}

/*
 *
 * 声明 pop
 *
 * 参数需要表达：
 * queue
 * 输出 message
 * timeout
 */
bq_result_t bq_pop(bounded_queue_t *q, void *out, int timeout_ms)
{
    int rc;

    if (q == NULL)
    {
        return BQ_INVALID;
    }

    if (out == NULL)
    {
        return BQ_INVALID;
    }

    pthread_mutex_lock(&q->mutex);

    struct timespec deadline; // 最晚等到几点几分几秒。
    if (timeout_ms > 0)
    {
        if (make_deadline(timeout_ms, &deadline) != 0)
        {
            pthread_mutex_unlock(&q->mutex);
            return BQ_SYSTEM;
        }
        
    }

    while (q->size == 0 && !q->closed)
    {
        if (timeout_ms == 0)
        {
            pthread_mutex_unlock(&q->mutex);
            return BQ_TIMEOUT;
        }
        else if (timeout_ms < 0)
        {
            rc = pthread_cond_wait(&q->not_empty, &q->mutex);
            if (rc != 0)
            {
                pthread_mutex_unlock(&q->mutex);
                return BQ_SYSTEM;
            }
        }
        else
        {
            rc = pthread_cond_timedwait(&q->not_empty, &q->mutex, &deadline);
            
            if (rc == ETIMEDOUT)
            {
                pthread_mutex_unlock(&q->mutex);
                return BQ_TIMEOUT;
            }

            if (rc != 0)
            {
                pthread_mutex_unlock(&q->mutex);
                return BQ_SYSTEM;
            }
            
        }
    }

    if (q->closed == 1 && q->size == 0)
    {
        pthread_mutex_unlock(&q->mutex);
        return BQ_CLOSED;
    }

    memcpy(out, q->buffer + q->head * q->element_size, q->element_size);


    q->head = (q->head + 1) % q->capacity;

    assert(q->size > 0);

    q->size--;

    

    pthread_cond_signal(&q->not_full);
    
    pthread_mutex_unlock(&q->mutex);

    return BQ_OK;
}


/*
 * 声明 close
 */
bq_result_t bq_close(bounded_queue_t *q)
{
    if (q == NULL)
    {
        return BQ_INVALID;
    }

    pthread_mutex_lock(&q->mutex);

    q->closed = 1;

    pthread_cond_broadcast(&q->not_empty);

    pthread_cond_broadcast(&q->not_full);
    
    pthread_mutex_unlock(&q->mutex);

    return BQ_OK;
}

/*
 * 声明 size
 */
bq_result_t bq_size(bounded_queue_t *q, size_t *out_size)
{

    if (q == NULL)
    {
        return BQ_INVALID;
    }

    if (out_size == NULL)
    {
        return BQ_INVALID;
    }

    pthread_mutex_lock(&q->mutex);

    *out_size = q->size;
    
    pthread_mutex_unlock(&q->mutex);

    return BQ_OK;
}

/*
 * 声明 destroy
 * 前置条件： 调用者必须已经没有任何线程使用 q
 */
bq_result_t bq_destroy(bounded_queue_t *q)
{

    if (q == NULL)
    {
        return BQ_INVALID;
    }
    

    int rc;

    rc = pthread_cond_destroy(&q->not_full);

    if (rc != 0)
    {
        return BQ_SYSTEM;
    }


    rc = pthread_cond_destroy(&q->not_empty);

    if (rc != 0)
    {
        return BQ_SYSTEM;
    }

    rc = pthread_mutex_destroy(&q->mutex);

    if (rc != 0)
    {
        return BQ_SYSTEM;
    }
    
    free(q->buffer);
    q->buffer = NULL;
    
    

    return BQ_OK;
}
