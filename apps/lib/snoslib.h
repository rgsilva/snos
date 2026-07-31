/* snoslib: shared helpers for SNOS applications. See snoslib.c for what
 * belongs here and — as importantly — what deliberately does not. */
#ifndef SNOS_APPLIB_H
#define SNOS_APPLIB_H

#include <stdint.h>
#include "snos_abi.h"

/* Print an unsigned 32-bit number in decimal (sys_putu stops at 16 bits). */
void sl_putu32(uint32_t v);

/* First decimal number in s, or `def` when there is none. */
uint16_t sl_parse_port(const char *s, uint16_t def);

/* Block until all n bytes are queued on ch: 1 ok, 0 the channel died. */
int sl_send_all(uint16_t ch, const char *p, uint16_t n);

/* Wait out NET_ST_OPENING; closes ch on every non-OK outcome. */
#define SL_OPEN_OK 0
#define SL_OPEN_TIMEOUT 1
#define SL_OPEN_CANCELLED 2
#define SL_OPEN_FAILED 3
uint16_t sl_wait_open(uint16_t ch, uint16_t frames, int allow_cancel);

/* dst = src, with `suffix` appended when src carries no ':' of its own. */
void sl_hostport(char *dst, int cap, const char *src, const char *suffix);

#endif
