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
 * typed and is printed at every level, `off` included; use it for answers,
 * echo, results, and the progress of an operation the user started and is
 * waiting on (a pairing, a WiFi join). Anything else is unsolicited and has a
 * level, given at the call and not printed in the line:
 *   log_event  (info) connection-level events and every warning or error: Bluetooth
 *              connect/pair/ready/disconnect, WiFi join/lost/failed, SNES link
 *              up/lost/restarted, relay link up/down, queue full, desync,
 *              init or setup failed.
 *   log_diag   (debug) diagnostics: the stats lines every two seconds, per-flow and
 *              per-packet chatter, scan-time noise.
 * `log off` shows neither, `log info` (the boot default) shows info,
 * `log debug` both; `log on` is an alias for debug. Not saved to flash. A line
 * that is a reply while a window is open is not also sent through a level. */
/* While the console is captured (the keyboard passthrough) nothing prints
 * through reply_* or the levels, whatever the level; nothing is kept for later.
 * console_puts is the capturing mode's own line and always prints. */
void log_capture(bool on);
int console_puts(const char *text);
typedef enum { LOG_OFF, LOG_INFO, LOG_DEBUG } log_level_t;
void log_set_level(log_level_t level);
log_level_t log_level(void);
const char *log_level_name(log_level_t level);
int log_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
int log_diag(const char *format, ...) __attribute__((format(printf, 1, 2)));
int reply_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int reply_puts(const char *text);

#endif
