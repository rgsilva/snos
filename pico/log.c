#include "log.h"
#include <stdarg.h>
#include <stdio.h>
static bool enabled = true;
void log_set_enabled(bool on) { enabled = on; }
bool log_enabled(void) { return enabled; }
int log_printf(const char *format, ...) {
    if (!enabled) return 0;
    va_list args;
    va_start(args, format);
    int n = vprintf(format, args);
    va_end(args);
    return n;
}
int log_puts(const char *text) { return log_printf("%s\n", text); }
