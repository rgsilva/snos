#include "log.h"
#include <stdarg.h>
#include <stdio.h>
static log_level_t level = LOG_INFO;
void log_set_level(log_level_t l) { level = l; }
log_level_t log_level(void) { return level; }
const char *log_level_name(log_level_t l) {
    return l == LOG_OFF ? "off" : l == LOG_INFO ? "info" : "debug";
}
static int say(log_level_t needed, const char *format, va_list args) {
    return level >= needed ? vprintf(format, args) : 0;
}
int reply_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vprintf(format, args);
    va_end(args);
    return n;
}
int reply_puts(const char *text) { return reply_printf("%s\n", text); }
int log_event(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = say(LOG_INFO, format, args);
    va_end(args);
    return n;
}
int log_diag(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = say(LOG_DEBUG, format, args);
    va_end(args);
    return n;
}
