/* Application registry: find apps in ROM, and run them. */
#include "app.h"
#include "snos.h"
#include "snos_abi.h"
#include "farmem.h"

static app_entry_t apps[APP_MAX];
static int napps;

/* How many 64 KB banks of ROM actually exist. Without this the scan would walk
 * off the end of the image and "find" every app again in the mirrors: a 1 MB
 * HiROM cart answers banks $C0-$CF for real and then repeats them at $D0, $E0
 * and $F0. The size byte in the cartridge header is authoritative, and
 * raw2sfc.py patches it to match the image it actually produced. */
static uint16_t rom_last_bank(void) {
  uint8_t szb = far_rd(0x00, 0xFFD7); /* log2 of the ROM size in KB */
  uint16_t kb, banks;
  if (szb > 12) szb = 12; /* 4 MB ceiling for a stock cart */
  kb = (uint16_t)(1u << szb);
  banks = (uint16_t)(kb / 64u);
  if (banks == 0) banks = 1;
  return (uint16_t)(0xC0 + banks - 1);
}

void app_scan(void) {
  uint16_t b, last = rom_last_bank();
  napps = 0;
  for (b = SNOS_APP_BANK_FIRST; b <= last && napps < APP_MAX;) {
    if (far_rd(b, APPHDR_MAGIC + 0) == 'S' && far_rd(b, APPHDR_MAGIC + 1) == 'N' &&
        far_rd(b, APPHDR_MAGIC + 2) == 'O' && far_rd(b, APPHDR_MAGIC + 3) == 'S') {
      app_entry_t *a = &apps[napps];
      int i;
      for (i = 0; i < APP_NAME_LEN; i++)
        a->name[i] = (char)far_rd(b, (uint16_t)(APPHDR_NAME + i));
      a->name[APP_NAME_LEN] = 0;
      for (i = APP_NAME_LEN - 1; i >= 0 && (a->name[i] == ' ' || a->name[i] == 0); i--)
        a->name[i] = 0;
      a->bank = b;
      a->entry = far_rd16(b, APPHDR_ENTRY);
      a->minabi = far_rd16(b, APPHDR_MINABI);
      a->banks = far_rd(b, APPHDR_BANKS);
      a->type = far_rd(b, APPHDR_TYPE);
      if (a->banks == 0) a->banks = 1;
      napps++;
      b = (uint16_t)(b + a->banks);
    } else {
      b++;
    }
  }
}

int app_count(void) { return napps; }

const app_entry_t *app_get(int i) {
  if (i < 0 || i >= napps) return 0;
  return &apps[i];
}

/* Case-insensitive, deliberately. Filenames on this system are conventionally
 * uppercase (that is what the ROM packer emits and what BASIC's SAVE writes),
 * but typing uppercase on a SNES means holding shift on an on-screen keyboard
 * for every character. Making the user's case match the disk's is not a
 * meaningful thing to ask of them. */
static int name_eq(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

const app_entry_t *app_find(const char *name) {
  int i;
  for (i = 0; i < napps; i++)
    if (name_eq(apps[i].name, name)) return &apps[i];
  return 0;
}

uint16_t app_run(const app_entry_t *a) {
  SNOS_ARGS.p2 = a->bank;
  SNOS_ARGS.p3 = a->entry;
  return app_exec();
}
