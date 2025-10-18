#include <stdio.h>
#include <stdarg.h>
#include "custom_logger.h"

void custom_log(const char* tag, const char* format, ...)
{
    va_list args;
    va_start(args, format);

    // prefix แบบมีสี/หนา
    printf(LOG_BOLD(LOG_COLOR_CYAN) "[CUSTOM] %s: " LOG_RESET_COLOR, tag);

    // เนื้อความจากผู้ใช้
    vprintf(format, args);
    printf("\n");

    va_end(args);
}