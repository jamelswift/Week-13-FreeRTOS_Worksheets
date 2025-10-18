@@ -0,0 +1,19 @@
#ifndef CUSTOM_LOGGER_H
#define CUSTOM_LOGGER_H

#include <stdarg.h>

#define LOG_COLOR_BLACK   "30"
#define LOG_COLOR_RED     "31"
#define LOG_COLOR_GREEN   "32"
#define LOG_COLOR_BROWN   "33"
#define LOG_COLOR_BLUE    "34"
#define LOG_COLOR_PURPLE  "35"
#define LOG_COLOR_CYAN    "36"

#define LOG_BOLD(COLOR)   "\033[1;" COLOR "m"
#define LOG_RESET_COLOR   "\033[0m"

void custom_log(const char* tag, const char* format, ...);

#endif // CUSTOM_LOGGER_H