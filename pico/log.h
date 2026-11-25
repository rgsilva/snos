#ifndef SNOS_PICO_LOG_H
#define SNOS_PICO_LOG_H
#include <stdbool.h>

/* Every line the firmware prints starts with one of these, once, at the start
 * of the line; the text after it is the message. tests/log_test.py reads this
 * list, so a new prefix goes here and nowhere else. */
#define P_SNES  "[SNES] "   /* controller-port link: stats line, link events */
#define P_NAT   "[NAT] "
#define P_BT    "[BT] "
#define P_WIFI  "[WIFI] "
#define P_KBD   "[KBD] "
#define P_RELAY "[RELAY] "
#define P_MODE  "[MODE] "
#define P_LOG   "[LOG] "
#define P_CTRL  "[CTRL] "   /* control channel to the SNES */
#define P_CON   "[CON] "    /* the console itself: help, line errors */
#define P_WIRE  "[WIRE] "   /* snos_wiretest targets */

/* Two classes of output. A reply is the consequence of a command the user
 * typed and is printed whatever the log setting; use it for answers, echo,
 * results, and the progress of an operation the user started and is waiting
 * on (a pairing, a WiFi join). Anything else is unsolicited: periodic stats,
 * background events, warnings. It goes through log_*, which prints only
 * after `log on`. Logging is off at boot and is not saved. */
void log_set_enabled(bool enabled);
bool log_enabled(void);
int log_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int log_puts(const char *text);
int reply_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int reply_puts(const char *text);

#endif
