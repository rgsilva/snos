#ifndef SNOS_PICO_BTKBD_H
#define SNOS_PICO_BTKBD_H
#include <stdbool.h>

void btkbd_init(void);
void btkbd_poll(void);
bool btkbd_command(const char *line);

#endif
