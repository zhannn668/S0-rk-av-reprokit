// src/log.c
#include "log.h"
#include <stdarg.h>

void log_print(int level, const char *fmt, ...)
{
    const char *lv = "I";
    if (level == LOG_LEVEL_WARN)  lv = "W";
    if (level == LOG_LEVEL_ERROR) lv = "E";

    fprintf(stderr, "[%s %s] ", lv, log_timestamp());

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}
