/* Sprite layer, used for the on-screen keyboard panel.
 *
 * The OSK used to be drawn into the background tilemap. That was wrong in a way
 * no amount of care would have fixed: the console scrolls that layer with the
 * hardware scroll registers, so the panel scrolled with the text — drifting up
 * through the display and corrupting whatever it landed on.
 *
 * Sprites are positioned in absolute screen coordinates and are completely
 * unaffected by background scrolling, so the problem stops existing rather than
 * being carefully avoided. It also means the panel can sit anywhere on screen,
 * and needs no save/restore of what it covers.
 *
 * In Mode 5 the background is 512 dots wide but sprites stay at 256, so the
 * keyboard renders at double the width of the 64-column text. That reads as
 * "this is a control, not terminal output", which is what we want.
 */
#ifndef SNOS_SPRITE_H
#define SNOS_SPRITE_H

#include <stdint.h>

/* Cell codes for spr_panel(). Anything else is an ASCII character; OR in
 * SPR_SELECTED to draw it inverted. */
#define SPR_BORDER 0xFFu
#define SPR_PAD 0xFEu
#define SPR_SELECTED 0x80u

void spr_init(void);          /* upload the sprite font and palettes */
void spr_enable(int on);      /* show/hide sprites on the main screen */
void spr_panel(const uint8_t *cells, int w, int h, int x0, int y0);
void spr_commit(void);        /* DMA the OAM during vblank */

#endif
