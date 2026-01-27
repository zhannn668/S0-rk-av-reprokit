// src/log.h
#pragma once

#include <stdio.h>
#include <time.h>

/* 打印当前时间，格式：HH:MM:SS.mmm */
static inline const char *log_timestamp(void)
{
    static char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_now;
    localtime_r(&ts.tv_sec, &tm_now);

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (int)(ts.tv_nsec / 1000000));
    return buf;
}

/* 日志级别 */
enum {
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
};

/* 实际的打印函数，在 log.c 里实现 */
void log_print(int level, const char *fmt, ...);

/* 对外用的简化宏 */
#define LOGI(fmt, ...) log_print(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) log_print(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) log_print(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
