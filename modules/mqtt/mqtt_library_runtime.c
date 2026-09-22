#include "mqtt_library_runtime.h"

#include <mosquitto.h>
#include <pthread.h>

/*
 * libmosquitto 的初始化/清理由进程内所有 MQTT 客户端共享。
 * mutex 保护引用计数，使 publisher 和 receiver 可以从不同线程安全使用。
 */
static pthread_mutex_t library_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int library_reference_count;

/* 获取一个库引用；第一个使用者负责执行全局初始化。 */
bool mqtt_library_acquire(void)
{
    int result = MOSQ_ERR_SUCCESS;

    pthread_mutex_lock(&library_mutex);

    if (library_reference_count == 0)
        result = mosquitto_lib_init();

    if (result == MOSQ_ERR_SUCCESS)
        ++library_reference_count;

    pthread_mutex_unlock(&library_mutex);
    return result == MOSQ_ERR_SUCCESS;
}

/* 释放一个库引用；最后一个使用者负责执行全局清理。 */
void mqtt_library_release(void)
{
    pthread_mutex_lock(&library_mutex);

    if (library_reference_count > 0)
    {
        --library_reference_count;
        if (library_reference_count == 0)
            (void)mosquitto_lib_cleanup();
    }

    pthread_mutex_unlock(&library_mutex);
}
