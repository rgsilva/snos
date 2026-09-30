#ifndef SNOS_PICO_CTRL_H
#define SNOS_PICO_CTRL_H
#include "link.h"
void ctrl_reset(void);
void ctrl_bytes(const uint8_t *data, size_t len);
void ctrl_address(const uint8_t *address);
bool ctrl_error(const uint8_t *payload);
void ctrl_poll(void);
#endif
