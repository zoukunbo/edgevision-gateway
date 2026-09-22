#ifndef EDGEVISION_MQTT_LIBRARY_RUNTIME_H
#define EDGEVISION_MQTT_LIBRARY_RUNTIME_H

#include <stdbool.h>
/**
 * 头文件提供声明，各个.c独立编译 连接器按函数父好明明成寻找实现
 */
/*
 * 获取一个进程级 libmosquitto 引用。
 * 第一个引用负责执行 mosquitto_lib_init()。
 */
bool mqtt_library_acquire(void);

/*
 * 释放一个进程级引用。
 * 最后一个引用负责执行 mosquitto_lib_cleanup()。
 */
void mqtt_library_release(void);

#endif