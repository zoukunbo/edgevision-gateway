#ifndef EDGEVISION_INTERVAL_CONFIG_H
#define EDGEVISION_INTERVAL_CONFIG_H

/* 0：成功；1：文件不存在；-1：读取或内容错误。
 * 非 0 时不修改 *out_ms。
 */
int load_interval_file(const char *path, int *out_ms);

#endif