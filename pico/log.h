#ifndef SNOS_PICO_LOG_H
#define SNOS_PICO_LOG_H
#include <stdbool.h>
void log_set_enabled(bool enabled);
bool log_enabled(void);
int log_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int log_puts(const char *text);
#endif
