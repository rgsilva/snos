/* Application registry.
 *
 * An application is a ROM bank whose first bytes are a SNOS header. There is no
 * directory anywhere: the kernel finds apps by walking the bank starts once at
 * boot and looking for the magic, which makes the ROM self-describing and means
 * adding an app is purely a build-time act.
 */
#ifndef SNOS_APP_H
#define SNOS_APP_H

#include <stdint.h>

#define APP_MAX 16
#define APP_NAME_LEN 16

/* Header layout, at offset $0000 of the app's first bank. Modelled on the BS-X
 * cartridge header with the broadcast fields removed — see PLAN.md §3. */
#define APPHDR_MAGIC 0x00   /* "SNOS" */
#define APPHDR_ABI 0x04     /* app ABI version */
#define APPHDR_MINABI 0x06  /* minimum kernel ABI it needs */
#define APPHDR_ENTRY 0x08   /* entry offset within the bank */
#define APPHDR_TYPE 0x0A    /* 0 = native 65816 */
#define APPHDR_BANKS 0x0B   /* how many banks it occupies */
#define APPHDR_NAME 0x0C    /* 16 bytes, NUL padded */
#define APPHDR_SRAM 0x1C    /* SRAM quota, 8 KB blocks */
#define APPHDR_FLAGS 0x1D
/* Arena slot (M10 step 4), in 256-byte pages of bank $7F: where this app's
 * data was LINKED. Two apps may co-run iff their page ranges are disjoint;
 * the kernel refuses an overlapping spawn deterministically at launch. The
 * two bytes were the checksum word, which nothing ever computed or checked. */
#define APPHDR_ARENA_PAGE 0x1E  /* start page: data at $7F:page*256 */
#define APPHDR_ARENA_PAGES 0x1F /* page count; 0 means all 256 (64 KB) */

typedef struct {
  char name[APP_NAME_LEN + 1];
  uint16_t bank;
  uint16_t entry;
  uint16_t minabi;
  uint8_t banks;
  uint8_t type;
  uint8_t arena_page;
  uint8_t arena_pages; /* normalised: 0-in-header stored here as full count
                        * via uint16 would not fit; kept raw, 0 = 256 */
} app_entry_t;

void app_scan(void);
int app_count(void);
const app_entry_t *app_get(int i);
const app_entry_t *app_find(const char *name);
uint16_t app_run(const app_entry_t *a);

#endif
