/* SNOS VNC — a Super Nintendo as a thin client.
 *
 *     vnc HOST[:PORT]        default port 5900
 *
 *     arrow keys   move the cursor
 *     W A S D      pan the window over a larger desktop
 *     J K          scroll down / up, as mouse-wheel clicks
 *     space        click
 *     ESC          quit
 *
 * This is a real RFB client. The console does the protocol itself — handshake,
 * pixel format negotiation, framebuffer updates, pointer events — and nothing
 * on the far side knows or cares that it is talking to a SNES.
 *
 * ------------------------------------------------------------- why VNC fits
 *
 * Three things make RFB a better match for this machine than anything we tried
 * before it:
 *
 * 1. THE CLIENT PICKS THE PIXEL FORMAT. We ask for 8 bits per pixel, true
 *    colour, R:3 G:3 B:2, and preload CGRAM with exactly that cube — so an
 *    incoming pixel byte IS a palette index. Nothing converts anything. The
 *    browser needed Pillow in the middle; this needs nobody.
 *
 *    RFB's colour-map mode (true-colour=0) maps onto Mode 3 even more directly,
 *    but it is barely exercised in modern servers and some refuse it. A fixed
 *    3:3:2 cube gets the same result from a format everybody tests, with no
 *    SetColourMapEntries handling and a palette that never changes mid-session.
 *
 * 2. INCREMENTAL UPDATES ARE THE PROTOCOL, not something we had to invent. A
 *    still screen sends zero bytes — measured against a real server, not
 *    assumed.
 *
 * 3. CopyRect says "this rectangle is already on screen somewhere else", which
 *    makes scrolling nearly free.
 *
 * ...and a fourth, which is the whole of the panning below:
 *
 * 4. FramebufferUpdateRequest CARRIES A RECTANGLE. "Send me this part of your
 *    desktop" is already the protocol's own idea, so a console that shows a
 *    window into something bigger than itself needs no extension, no server
 *    cooperation and no new message. It just asks for the part it is looking at.
 *
 * ----------------------------------------------------------- the honest cost
 *
 * The SNES stores 8bpp as EIGHT BITPLANES, and RFB delivers linear bytes, so
 * every pixel has to be transposed. That is affordable but not free, and the
 * figure that used to be here — "roughly a screen per two seconds" — was an
 * estimate and was wrong by an order of magnitude. MEASURED: at -O0 one tile
 * cost 0.93 of a frame, so twelve seconds for a 768-tile screen, and the wire
 * had been made fast enough by then that this was the larger half of a
 * thirty-second first screen against TigerVNC through the real gateway.
 *
 * SO IT IS NOT C ANY MORE. tile.s does the transpose by hand — shift-and-rotate,
 * fully unrolled, the tile's 64 pixel bytes staged in direct page — and measures
 * 40.2k master clocks a tile against 333.4k, 8.3x. A screen is 1.4 seconds of
 * transpose rather than twelve, and a 96-tile pan step a quarter of a second
 * rather than two. THE WIRE IS THE BOTTLENECK AGAIN, which is where a thin
 * client ought to be, and the remaining console-side cost is spread thinly
 * enough that no one part of it is worth the next hand-written routine.
 *
 * It stays on the console on purpose. tools/gateway.py is a dumb pipe for this
 * app — pump() is sock.recv(512) straight through, no transformation anywhere —
 * so the SNES is a real VNC client rather than a display for tiles somebody else
 * interleaved. Sending pre-interleaved tiles would have been less work and would
 * have made the gateway the client.
 *
 * What it also costs is memory. Server rectangles are NOT tile-aligned — a real
 * server sent `123x21 at (133,104)` — so a rectangle lands across partial
 * tiles, and a tile cannot be rebuilt from a fragment. So the client keeps a
 * linear shadow of the framebuffer, writes rectangles into that, and rebuilds
 * whole tiles from it. 256x192 is 48 KB, which is why this lives in the app
 * arena in bank $7F rather than anywhere near bank $00's eight.
 *
 * ------------------------------------------------------------------ panning
 *
 * A 256-pixel-wide X server makes Chromium lay a page out at 256 CSS pixels,
 * and the text wraps into a column too narrow to read. The server is therefore
 * 512x384 (tools/vncbox/start.sh) and the console shows a 256x192 WINDOW into
 * it, moved with WASD. Same glyphs at the same size, four times the page in
 * reach. Downscaling a wider render instead was tried on paper and rejected:
 * 4-pixel glyphs are a grey smear.
 *
 * THE SHADOW STAYS VIEWPORT-SIZED. It has to: 48 KB already fills most of bank
 * $7F, and 512x384 would be 196 KB against a 64 KB VRAM with 48 KB of it in
 * tile data. So panning is not "scroll a bigger picture", it is "ask the server
 * for the rectangle you are now looking at" — which is one field of a message
 * RFB already has.
 *
 * The part that decides whether it feels usable is what panning does NOT do.
 * The naive version re-requests the whole 48 KB window: fourteen seconds at the
 * measured ~3.5 KB/s, which reads as broken. Instead:
 *
 *   THE SHADOW IS A TORUS. Server pixel (sx, sy) always lives at shadow byte
 *   (sx mod 256, sy mod 192), whatever the pan origin is. Move the window by
 *   32 pixels and the 224 columns you keep DO NOT MOVE — they are already at
 *   the right shadow address, so not one pixel is copied and not one tile is
 *   rebuilt. The 32 columns that scrolled off occupy exactly the shadow bytes
 *   the 32 newly exposed columns need, so the slots recycle themselves.
 *
 *   THE TILEMAP TURNS THE TORUS BACK INTO A PICTURE. Tile indices are
 *   arbitrary, so map cell (r, c) is pointed at tile ((py/8 + r) mod 24,
 *   (px/8 + c) mod 32) instead of at (r, c). That is 768 tilemap entries —
 *   1.5 KB, three DMAs, four frames — against 48 KB of tile data.
 *
 * PAN_STEP MUST STAY A MULTIPLE OF 8 or all of that evaporates: a pan that is
 * not a whole number of tiles cannot be expressed in the tilemap at all, and
 * every tile on screen would have to be rebuilt.
 *
 * What is left to pay for is the strip you have never seen: 32x192 is 6 KB, one
 * non-incremental FramebufferUpdateRequest, and 96 tiles to transpose. The
 * steady state stays incremental, so a still screen still sends nothing.
 *
 * Two coordinate rules fall out of this, and both are the kind that look
 * correct until you pan:
 *
 *   - INCOMING RECTANGLES ARE IN SERVER COORDINATES. They have to be translated
 *     by the pan origin and clipped to the window before they touch the shadow.
 *     While the server was exactly console-sized those were the same numbers,
 *     so the old code could — and did — treat one as the other.
 *
 *   - POINTER EVENTS GO OUT IN SERVER COORDINATES: pan origin plus cursor. Get
 *     that wrong and space clicks the right link until the first pan and the
 *     wrong one after it, which reads as a regression rather than an omission.
 *
 * ---------------------------------------------------- scrolling, J and K
 *
 * PANNING AND SCROLLING ARE NOT THE SAME THING and the app does both. A pan
 * moves the console's window over a desktop that has not changed; a scroll asks
 * the far side to move the PAGE, and the desktop underneath the window changes.
 * Once the window is smaller than the desktop you need both, or half of every
 * page is unreachable: WASD reaches the parts of the desktop the window is not
 * over, J and K reach the parts of the document the desktop is not showing.
 *
 * J and K go out as WHEEL CLICKS — PointerEvent with button 4 (mask bit 3,
 * value 8) for up and button 5 (mask bit 4, value 16) for down, each a press
 * followed by a release — rather than as Page Up / Page Down KeyEvents. Three
 * reasons, in order of how much they mattered:
 *
 *   1. IT SCROLLS WHAT IS UNDER THE CURSOR. That is what a wheel does, and it
 *      is the only version that reaches an inner scrollable pane. Page Up/Down
 *      go to whatever has keyboard focus, which on a freshly loaded page is
 *      often nothing at all.
 *
 *   2. It reuses the pointer path the space click already uses, so it needs no
 *      KeyEvent, no keysym table, and no new coordinate rule — the coordinates
 *      are the same `px + cx, py + cy` in SERVER space, and getting THAT wrong
 *      shows up only after a pan, which is exactly how it would slip through.
 *
 *   3. A wheel click is one message either way; a KeyEvent is two, and the
 *      keysym for Page Down is a number this file would have to carry.
 *
 * What comes back is the CopyRect path's first real workout: a server answers a
 * scroll with "this rectangle is already on your screen, n rows away" plus raw
 * pixels for the band that has newly come into view. Everything copy_rect does
 * about the torus and about row order was written against the test's synthetic
 * scroll before any of it had met a browser.
 *
 * WHAT IT COSTS, and the number is not the one the design predicts. Measured
 * end to end against TigerVNC and Chromium on news.ycombinator.com, the console
 * held at 60 fps:
 *
 *     first screen   1284 frames (21.4 s)    926 tiles   1.21 screens
 *     one pan         176 frames ( 2.9 s)     99 tiles   0.13 screens
 *     one J or K     ~2380 frames (39.6 s)  ~1670 tiles  2.17 screens
 *
 * The wire rate is the same in all three — about eighteen seconds a screenful —
 * so the tile column is the whole story, and it says a wheel click repaints TWO
 * AND A LITTLE SCREENS where a pan repaints an eighth of one. That is not
 * CopyRect failing: the pan figure proves the strip arithmetic works, and 99
 * tiles is the 96 the exposed strip contains. It is CHROMIUM'S SMOOTH SCROLLING.
 * One wheel click is an animation, the animation is a dozen framebuffer updates
 * as the page glides, and the client faithfully fetches every intermediate
 * position of a page that was on its way somewhere else.
 *
 * So the lever, if this ever matters enough, is on the far side and not here —
 * `--disable-smooth-scrolling` in tools/vncbox/start.sh should turn one wheel
 * click back into one CopyRect and one band, which is what the arithmetic says
 * it should have been. Left alone deliberately: the container is Ricardo's to
 * tune, and a forty-second scroll that is CORRECT is a better thing to hand over
 * than a four-second one that has not been measured.
 *
 * ------------------------------------------------- when it is allowed to draw
 *
 * The one thing here that is genuinely counter-intuitive: AN APPLICATION ON
 * THIS MACHINE HAS NO VBLANK. NMI moves a link frame on every one of them while
 * an app owns the PPU, and at -O0 that runs from the top of the window into the
 * middle of the next frame. So every VRAM and OAM write in this file happens
 * inside a forced blank over scanlines 192-223 — the strip below the picture,
 * which is deliberately black — rather than in a vblank that does not exist. See
 * "the write window" below; it answers both of the bugs this app has had.
 *
 * ------------------------------------------------------------------ the minimap
 *
 * The black strip is not entirely spent, and the reason is worth stating because
 * it is the opposite of what the strip looks like:
 *
 *   FORCED BLANK IS NOT A REGION. $2100 bit 7 blanks the WHOLE screen for as
 *   long as it is set. The strip is black only because that is where the beam
 *   happens to be while the blank is up — 192 at the earliest, and measured to
 *   release by scanline 213 at the latest. THE LINES BELOW THAT ARE NEVER
 *   BLANKED. Ten of them, 214-223, are as displayable as any line of the
 *   picture, and nothing but the assumption that "the strip is the blank" was
 *   keeping them empty.
 *
 * So they hold a box within a box: an outer rectangle standing for the whole
 * server desktop, and an inner marker showing where the 256x192 window sits in
 * it. That is the one thing panning cannot tell you from the picture — every
 * pan looks like a page that scrolled, and after four of them there is nothing
 * on screen that says whether you are at the top or the bottom of the desktop.
 *
 * The split follows what each half has to do:
 *
 *   THE OUTER FRAME NEVER MOVES, so it is tile data — eight 8bpp tiles at
 *   $F040 — with the strip's tilemap rows pointed at them instead of at the
 *   all-zero tile they used to share. Written once in setup_ppu, costing not one
 *   byte per frame afterwards.
 *
 *   Putting a SHAPE in the strip is also what finally showed that its tilemap
 *   entries had had both flip bits set all along. An all-zero tile is its own
 *   mirror image, so nothing could see it. See the note on the VRAM addresses.
 *
 *   ONLY THE MARKER MOVES, and it moves once per pan, so it is a sprite: two
 *   8x8 objects side by side, eight bytes of OAM. Two small sprites rather than
 *   one 16x16, because the large-object bit lives in the OAM high table's
 *   packed two-bits-per-sprite byte that place_cursor also writes — a shared
 *   byte between two writers, for a shape that saves four bytes.
 *
 * IT SHOWS PAN POSITION AND NOTHING ELSE. RFB carries pixels, not page state,
 * so the console cannot know the browser's scroll offset; after a J or K the
 * picture changes and the marker correctly does not. Inferring scroll from
 * pixel motion was considered and is not worth being wrong about.
 *
 * The frame is a fixed 32x10 whatever the desktop is, and the MARKER is what
 * carries the proportion — its size is the window's share of the desktop and
 * its travel is the rest.
 *
 * SO THE BOX IS NOT TO SCALE, and that is a choice rather than an oversight.
 * Ten lines is all the strip has, so a 4:3 desktop drawn true would be thirteen
 * pixels wide: a marker six pixels across with seven pixels to cross in nine
 * pan steps, which is a thing you have to be told is moving. 32x10 is 3.2:1
 * against the desktop's 1.33:1 — stretched two and a half times sideways — and
 * buys a marker that steps two pixels per press and is legible on a television.
 * The proportions that carry information, the marker's SHARE of the frame and
 * its position within it, are exact in both axes.
 */
#include "snos_abi.h"

#define FB_W 256
#define FB_H 192
/* Spelled out rather than FB_W * FB_H: int is 16 bits here, so the product
 * overflows to a negative array bound and the compiler rejects it. */
#define FB_SIZE 49152u
#define TILES_W (FB_W / 8)
#define TILES_H (FB_H / 8)
#define NTILES (TILES_W * TILES_H)

/* How far one WASD press moves the window, in server pixels.
 *
 * MUST BE A MULTIPLE OF 8, and 32 is deliberately four whole tiles. See
 * "panning" above: the retained part of the screen is moved by rewriting the
 * tilemap, and a tilemap can only express a whole-tile shift. A step of, say,
 * 20 would silently fall back to rebuilding all 768 tiles — seventeen seconds —
 * with nothing but the feel of it to tell you. */
#define PAN_STEP 32

/* VRAM, laid out so nothing overlaps — the same arithmetic `web` uses:
 *   $0000-$BFFF  768 image tiles, 8bpp
 *   $C000-$DFFF  sprites: tile 0 the cursor, tiles 1-2 the minimap marker
 *   $E000-$E7FF  BG1 tilemap
 *   $F000-$F03F  one all-zero tile, for the display below the picture
 *   $F040-$F23F  eight tiles holding the minimap's outer frame
 */
/* THE SHIFTS BELOW ARE NOT A DIVISION WRITTEN SHORT, and the `u` suffixes are
 * not decoration. Both are working around a Calypsi constant-folding bug that
 * this app has been quietly carrying since the strip existed.
 *
 * Every address here is above 0x7FFF, and Calypsi types such a hexadecimal
 * constant as a SIGNED int rather than the unsigned one C requires. So 0xF000
 * is -4096, and DIVISION folds signed: measured, cc65816 5.17 at -O0,
 *
 *     0xF000u / 64u              ->  lda ##-64      wrong
 *     ((uint16_t)0xF000u) / 64u  ->  lda ##-64      wrong; the cast does not help
 *     0xF000UL / 64u             ->  lda ##0x3c0    right
 *     0xF000u >> 6               ->  lda ##0x3c0    right
 *
 * The shift is right and the divide is wrong for the same constant, which is
 * why this is spelled as a shift.
 *
 * On the ADDRESSES the bug cancelled and hid: `VRAM_MAP >> 1` came out as
 * 0xF000 rather than 0x7000, and VRAM is 32K words, so the wrap landed it back
 * on the right place — including $2107, which was being handed 0xF0 while the
 * comment beside it explained why it must be 0x70. The `u` makes those the
 * numbers they always claimed to be.
 *
 * ON A TILEMAP ENTRY IT DOES NOT CANCEL. -64 is 0xFFC0: the low ten bits are
 * the right tile, and the top six are the palette, the priority, and BOTH FLIP
 * BITS. The strip pointed at an all-zero tile, which is its own mirror image,
 * so it was wrong and looked right for as long as the strip was blank.
 *
 * The minimap is the first thing below the picture with a shape, and it came
 * out rotated 180 degrees — the frame's top edge below where the tile data put
 * it, its sides seven pixels in from the ends. The tile data was perfect and so
 * was the entry's index field, which is exactly why the check that decoded them
 * agreed with itself: it masked the entry with 0x3FF, which is to say it masked
 * off the bug. test/vnc.lua now asserts the WHOLE entry. */
#define VRAM_OBJ 0xC000u
#define VRAM_MAP 0xE000u
#define VRAM_BLANK 0xF000u
#define VRAM_MM 0xF040u
/* 960 — and a tilemap entry's index field is 10 bits, so it reaches. */
#define TILE_BLANK (VRAM_BLANK >> 6)
/* 961-968, still inside the 10 bits. */
#define TILE_MM (VRAM_MM >> 6)

/* ---------------------------------------------------------- minimap geometry
 *
 * MM_H IS MEASURED, not chosen. The write window opens at scanline 192 and
 * offers no further transfer past WIN_END, and one transfer plus its check runs
 * about four scanlines past that — so the blank releases at 212 or 213, every
 * time, over the three hundred-odd of them `make test-vnc` checks (it prints
 * the worst as "latest close"). 214 is therefore the first line that is never
 * covered, and 224 is the last line displayed. The margin is one scanline, and
 * it is structural rather than lucky: win_room() refuses to START a transfer
 * past WIN_END, so the worst case is bounded by the cost of one transfer.
 *
 * If WIN_END ever moves, this moves with it: a box whose top row falls inside
 * the blank flickers on every repaint, which is the artefact the write window
 * exists to remove. */
#define MM_W 32
#define MM_H 10
#define MM_Y0 (224 - MM_H)          /* 214, the top line of the frame */
#define MM_R0 (MM_Y0 / 8)           /* 26 — the first tilemap row it touches */
#define MM_TR (28 - MM_R0)          /* 2 rows; 28 rows of 8 are displayed */
#define MM_TW (MM_W / 8)            /* 4 tile columns */
/* log2(MM_TW). The 65816 has no multiply, so the tile index below is a shift
 * and this is the one place that has to be kept in step with MM_W. */
#define MM_TSH 2
#define MM_C0 ((32 - MM_TW) / 2)    /* centred: columns 14-17 */
#define MM_X0 (MM_C0 * 8)           /* 112 */

/* The frame's colour, as an index into the 3:3:2 cube load_palette() builds:
 * r=5, g=5, b=2 of 7/7/3, which lands near 70% grey. Deliberately not white —
 * the marker is white, and a marker that shares the frame's colour disappears
 * into it exactly when it reaches the edge. */
#define MM_INK ((5 << 5) | (5 << 2) | 2)

/* What the marker sprite can draw: two 8x8 objects, so 16 wide, and one row of
 * them, so 8 tall. A desktop close to the console's size wants a marker larger
 * than that; it is clamped for drawing, while its TRAVEL still comes from the
 * true proportion, so the marker under-fills rather than overshooting the
 * frame. */
#define MM_MARK_W 16
#define MM_MARK_H 8

#define R8(a) (*(volatile __far uint8_t *)(uint32_t)(a))

/* RFB server -> client message types */
#define MSG_UPDATE 0
#define MSG_COLOURMAP 1
#define MSG_BELL 2
#define MSG_CUT 3

#define ENC_RAW 0
#define ENC_COPYRECT 1

/* How many tiles are staged in WRAM and offered to one write window.
 *
 * Eight, and eight is generous. console.h explains why byte-counting a budget
 * gives an answer ten times too big — the register setup costs about nine times
 * the transfer at -O0, so transfers per window are scarce, not bytes.
 * Then the window here is smaller again than the console's; see below.
 *
 * The binding constraint is the other way round anyway: the transpose runs at
 * about 1.3 frames per tile, so the display need only move ONE tile a frame to
 * stay ahead of the thing feeding it. Eight small transfers are also cheaper to
 * abandon halfway than one big coalesced one, which is what the first attempt
 * tried and what never fitted.
 *
 * It is also, not by accident, 8 x 32 words = exactly eight tilemap ROWS, which
 * is what write_tilemap() stages a band of the map in. */
#define BATCH 8

static char args[SNOS_ARGV_MAX];
static uint8_t fb[FB_SIZE];       /* linear shadow, bank $7F — a torus, see above */
static uint8_t dirty[NTILES];
static uint16_t stage[BATCH][32]; /* tiles as PPU words, built before a window */
static uint16_t stage_t[BATCH];   /* which tile each staged entry is */
static uint8_t hdr[32];
static uint8_t junk[64];          /* somewhere to drop bytes we are clipping away */
static uint8_t crow[FB_W];        /* one row, for CopyRect across the torus seam */
static uint16_t ch;
static uint16_t cx, cy;
static uint16_t srv_w, srv_h;
static int quit;

/* The pan origin: which server pixel the top-left of the console shows.
 *
 * px/py are absolute server coordinates and always multiples of 8. pxm/pym are
 * the same thing reduced modulo the shadow, which is what the torus arithmetic
 * actually uses — kept alongside rather than recomputed, because it is needed
 * per rectangle row and the 65816 has no divide either. */
static uint16_t px, py, pxm, pym;
static uint16_t maxpx, maxpy;

/* The minimap marker: how big it is drawn, and how far it may travel inside the
 * frame. Both are the window's share of the desktop, so both are settled once,
 * from ServerInit's size, and never again. mm_on is 0 when the desktop is no
 * bigger than the console — there is then nothing a position indicator could
 * say, so the strip stays entirely black. */
static uint16_t mm_mw, mm_mh, mm_travx, mm_travy;
static int mm_on;
static int16_t pend_dx, pend_dy;  /* pan asked for but not yet safe to apply */
static int parsing;               /* 1 while a server message is half-read */
static int ready;                 /* 1 once the app owns the PPU and has set it up */

/* One pending non-incremental request, as a bounding box in SERVER coordinates.
 * Panning puts the newly exposed strip here; a CopyRect whose source has fallen
 * outside the window puts the part it could not satisfy here. */
static uint16_t rqx, rqy, rqw, rqh;
static int rq_pending;

static void say(const char *s) { sys_puts(s); }

/* Defined below, but called from rd(): the read loop services the cursor so a
 * long transfer stays responsive. */
static int poll_input(void);

static void vaddr(uint16_t w) {
  R8(0x2116) = (uint8_t)w;
  R8(0x2117) = (uint8_t)(w >> 8);
}

/* n * num / den, rounded, by repeated addition.
 *
 * The 65816 has neither a multiply nor a divide, and the note on build_tile is
 * about what that costs PER PIXEL. This is the other end of the same rule: it
 * runs once per pan — a pan already costs six hundred frames — so the shape
 * that is obviously right beats one that needs a shift table and a special case
 * for every desktop size that is not a power of two.
 *
 * Callers pass eighths (px >> 3, maxpx >> 3), which changes no ratio and keeps
 * the loop under a few hundred passes on any plausible desktop. acc starts at
 * half the denominator, which is what makes this round rather than floor: with
 * a marker that travels five pixels over seven pan stops, flooring wastes one
 * of the five on a step nothing ever lands in. */
static uint16_t scale(uint16_t n, uint16_t num, uint16_t den) {
  uint16_t acc, out = 0, i;
  if (den == 0 || num == 0) return 0;
  acc = (uint16_t)(den >> 1);
  for (i = 0; i < n; i++) {
    acc = (uint16_t)(acc + num);
    while (acc >= den) {
      acc = (uint16_t)(acc - den);
      out++;
    }
  }
  return out;
}

/* --------------------------------------------------------- the write window
 *
 * THERE IS NO USABLE VBLANK HERE, and that is measured, not assumed. It is the
 * most surprising thing about an application that paints and downloads at the
 * same time on this machine, so it is worth stating plainly:
 *
 *   NMI runs net_service on EVERY frame while an application owns the PPU. The
 *   kernel defers it only for the console — con_dirty_pending() returns 0 while
 *   gfx_owned, deliberately, and `vnc` is the very app whose link died in the
 *   handshake when it did not. NMI fires at the top of vblank, and at -O0 that
 *   handler runs until about scanline 100 of the NEXT frame.
 *
 * So a foreground that waits for vblank is handed a window that has already
 * gone. The first attempt at fixing the flicker did exactly that, and
 * `make test-vnc` caught it: it logs the scanline of every DMA, every one landed
 * between 104 and 122 — the middle of the picture — and the vblank check failed
 * on 96 batches out of 96. con_flush's ten transfers per window are ten only
 * because the kernel gets NMI out of its own way first, and an app cannot.
 *
 * So the app takes a window of its own. The picture is 192 lines of a 224-line
 * display, and the tilemap rows below it point at an all-zero tile — so
 * scanlines 192-223 are solid black whatever else is happening. A forced blank
 * asserted and released inside that strip covers nothing anybody can see, VRAM
 * and OAM are both writable for all of it, and NMI is nowhere near it.
 *
 * WIN_END leaves margin. One 64-byte transfer and its check cost about four
 * scanlines at -O0, and a blank still asserted when vblank arrived would be held
 * by NMI across the top hundred lines of the next frame — which is precisely the
 * artefact all of this exists to remove. */
#define WIN_TOP FB_H     /* 192: the first scanline below the picture */
#define WIN_END 210      /* offer no further transfer past here */

/* The PPU's V counter. Read $213F to reset the low/high toggle, $2137 to latch,
 * then $213D twice. */
static uint16_t vcount(void) {
  uint16_t v;
  R8(0x213F);
  R8(0x2137);
  v = (uint16_t)R8(0x213D);
  return (uint16_t)(v | ((uint16_t)(R8(0x213D) & 1) << 8));
}

/* Park the beam at the TOP of the strip — leaving whichever window we may
 * already be standing in — and then blank. Entering from above is what makes "at
 * least one transfer always lands" true, and therefore what makes the sweep in
 * flush_dirty finite. */
static void win_open(void) {
  uint16_t guard = 0;
  /* Both waits are bounded, for the reason pad_latch's are: a spin that can
   * never finish hangs the machine, and ~4000 iterations is around ten frames of
   * them. If NMI ever grew to swallow the strip as well, this would paint in the
   * wrong place — visibly — rather than stop. */
  while (vcount() >= WIN_TOP && ++guard < 4000u) ;
  guard = 0;
  while (vcount() < WIN_TOP && ++guard < 4000u) ;
  R8(0x2100) = 0x8F;
}

static void win_close(void) { R8(0x2100) = 0x0F; }

static int win_room(void) {
  uint16_t v = vcount();
  return v >= WIN_TOP && v < WIN_END;
}

/* One DMA from the WRAM staging buffer into VRAM, channel 0.
 *
 * Channel 0 is free: con_flush owns it but is a no-op while an application holds
 * the PPU, and the keyboard panel's HDMA (channels 3/4/5) is disabled for the
 * same reason. Nothing in NMI does DMA at all, so there is no one to collide
 * with mid-setup. */
static void dma_tiles(uint16_t vword, const uint16_t *src, uint16_t nwords) {
  uint32_t s = (uint32_t)src;
  uint16_t bytes = (uint16_t)(nwords * 2);
  R8(0x2116) = (uint8_t)vword;
  R8(0x2117) = (uint8_t)(vword >> 8);
  R8(0x4300) = 0x01;                   /* A->B, mode 1: low,high -> $2118,$2119 */
  R8(0x4301) = 0x18;
  R8(0x4302) = (uint8_t)s;
  R8(0x4303) = (uint8_t)(s >> 8);
  R8(0x4304) = (uint8_t)(s >> 16);     /* source bank, $7F */
  R8(0x4305) = (uint8_t)bytes;
  R8(0x4306) = (uint8_t)(bytes >> 8);
  R8(0x420B) = 0x01;
}

/* ------------------------------------------------------------------- stream */

/* Read exactly n bytes, or fail. RFB is a framed protocol on a byte stream:
 * every message has a known length and reading "whatever arrived" would
 * desynchronise it permanently. */
static int rd(uint8_t *dst, uint16_t n) {
  uint16_t got = 0;
  while (got < n) {
    uint16_t k;
    /* Every pass, not only when starved. During a 48 KB transfer bytes are
     * almost always waiting, so the starvation branch below never runs — which
     * meant the cursor and ESC were ignored for the whole eighty seconds. */
    if (poll_input()) return 0;
    k = sys_net_read(ch, dst + got, (uint16_t)(n - got));
    if (k > 0 && k != SYS_ERR) {
      got = (uint16_t)(got + k);
      continue;
    }
    if (sys_net_state(ch) != NET_ST_OPEN && sys_net_state(ch) != NET_ST_EOF)
      return 0;
    if (poll_input()) return 0;
    sys_yield();
  }
  return 1;
}

/* Consume and throw away n bytes.
 *
 * A server wider than the console sends rectangles that hang off the side of
 * the window, and every byte of one still has to come off the wire in order or
 * the stream desynchronises. The old code returned failure — that is, hung up —
 * on a rectangle it could not place, which was invisible only because the
 * server was exactly console-sized and no such rectangle existed. */
static int skipn(uint16_t n) {
  while (n) {
    uint16_t k = (n > (uint16_t)sizeof junk) ? (uint16_t)sizeof junk : n;
    if (!rd(junk, k)) return 0;
    n = (uint16_t)(n - k);
  }
  return 1;
}

static int wr(const uint8_t *src, uint16_t n) {
  uint16_t sent = 0;
  while (sent < n) {
    uint16_t k = sys_net_write(ch, (const char *)(src + sent),
                               (uint16_t)(n - sent));
    if (k == 0) {
      if (sys_net_state(ch) != NET_ST_OPEN && sys_net_state(ch) != NET_ST_EOF)
        return 0;
      sys_yield();
      continue;
    }
    sent = (uint16_t)(sent + k);
  }
  return 1;
}

static uint16_t be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

/* ---------------------------------------------------------------- handshake */

static int handshake(void) {
  uint16_t n, i;

  if (!rd(hdr, 12)) return 0;             /* "RFB 003.008\n" */
  if (hdr[0] != 'R' || hdr[1] != 'F' || hdr[2] != 'B') {
    say("not an RFB server\n");
    return 0;
  }
  if (!wr((const uint8_t *)"RFB 003.008\n", 12)) return 0;

  if (!rd(hdr, 1)) return 0;
  n = hdr[0];
  if (n == 0) {
    say("server refused the connection\n");
    return 0;
  }
  if (n > sizeof hdr) n = sizeof hdr;
  if (!rd(hdr, n)) return 0;
  for (i = 0; i < n; i++)
    if (hdr[i] == 1) break;
  if (i == n) {
    say("server wants authentication; only 'None' is supported\n");
    return 0;
  }
  hdr[0] = 1;
  if (!wr(hdr, 1)) return 0;              /* security type: None */
  if (!rd(hdr, 4)) return 0;              /* SecurityResult */
  if (hdr[0] || hdr[1] || hdr[2] || hdr[3]) {
    say("server rejected 'None' authentication\n");
    return 0;
  }

  hdr[0] = 1;
  if (!wr(hdr, 1)) return 0;              /* ClientInit: share the desktop */

  if (!rd(hdr, 24)) return 0;             /* ServerInit: size + pixel format */
  /* KEEP THE REAL SIZE. It is not the console's size any more, and everything
   * about panning is arithmetic between the two. */
  srv_w = be16(hdr);
  srv_h = be16(hdr + 2);
  {
    uint32_t namelen = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) |
                       ((uint32_t)hdr[22] << 8) | hdr[23];
    while (namelen) {
      uint16_t take = (namelen > sizeof hdr) ? (uint16_t)sizeof hdr
                                             : (uint16_t)namelen;
      if (!rd(hdr, take)) return 0;
      namelen -= take;
    }
  }

  /* SetPixelFormat: 8bpp, true colour, R:3 G:3 B:2. This is the message that
   * makes the whole app possible — after it, a pixel byte is a CGRAM index. */
  {
    uint8_t m[20];
    for (i = 0; i < 20; i++) m[i] = 0;
    m[0] = 0;                 /* message type */
    m[4] = 8;                 /* bits-per-pixel */
    m[5] = 8;                 /* depth */
    m[6] = 0;                 /* little-endian */
    m[7] = 1;                 /* true-colour */
    put16(m + 8, 7);          /* red-max   */
    put16(m + 10, 7);         /* green-max */
    put16(m + 12, 3);         /* blue-max  */
    m[14] = 5;                /* red-shift   */
    m[15] = 2;                /* green-shift */
    m[16] = 0;                /* blue-shift  */
    if (!wr(m, 20)) return 0;
  }

  /* SetEncodings. CopyRect first: preference order is meaningful, and a
   * scroll expressed as "copy that rectangle" costs four bytes instead of a
   * screenful. */
  {
    uint8_t m[12];
    m[0] = 2;
    m[1] = 0;
    put16(m + 2, 2);
    m[4] = 0; m[5] = 0; m[6] = 0; m[7] = ENC_COPYRECT;
    m[8] = 0; m[9] = 0; m[10] = 0; m[11] = ENC_RAW;
    if (!wr(m, 12)) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------- what to ask for
 *
 * A FramebufferUpdateRequest names a rectangle, so the window IS the request.
 * Two kinds go out:
 *
 *   incremental, over the whole window — the steady state; a still screen
 *   answers it with nothing at all, which is the property the whole design
 *   leans on.
 *
 *   non-incremental, over one bounding box — "I have never seen this, send it
 *   whatever you think I know". That is how a pan pays for only the strip it
 *   exposed, and how a CopyRect whose source scrolled out of the window repairs
 *   the part it could not copy. */
static int req(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int incremental) {
  uint8_t m[10];
  m[0] = 3;
  m[1] = (uint8_t)(incremental ? 1 : 0);
  put16(m + 2, x);
  put16(m + 4, y);
  put16(m + 6, w);
  put16(m + 8, h);
  return wr(m, 10);
}

/* Merge a rectangle into the pending non-incremental request.
 *
 * A bounding box rather than a region list. In every case that actually occurs
 * — a pan strip, or the one band a scroll could not copy — there is a single
 * rectangle and the box is exact; the pathological case costs bandwidth, not
 * correctness, and a region library on a 65816 would cost more than it saved. */
static void need_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t x1, y1;
  if (w == 0 || h == 0) return;
  if (!rq_pending) {
    rqx = x; rqy = y; rqw = w; rqh = h;
    rq_pending = 1;
    return;
  }
  x1 = (uint16_t)(rqx + rqw);
  y1 = (uint16_t)(rqy + rqh);
  if ((uint16_t)(x + w) > x1) x1 = (uint16_t)(x + w);
  if ((uint16_t)(y + h) > y1) y1 = (uint16_t)(y + h);
  if (x < rqx) rqx = x;
  if (y < rqy) rqy = y;
  rqw = (uint16_t)(x1 - rqx);
  rqh = (uint16_t)(y1 - rqy);
}

/* Ask for the pending rectangle if there is one, otherwise for whatever has
 * changed in the window.
 *
 * ALWAYS SEND SOMETHING. An earlier version tracked whether poll_input() had
 * already sent a request this pass and skipped the loop's own, on the grounds
 * that two outstanding requests were untidy. That is not how a server accounts
 * for them: TigerVNC keeps a `requested` REGION, unions every request into it,
 * and CLEARS IT ENTIRELY when it sends an update. So the pan's strip request
 * consumed the window request that was outstanding behind it, and after the
 * last pan the client had nothing outstanding at all — at which point the
 * server has no reason to ever speak again.
 *
 * The symptom was a screen that panned perfectly and then stopped tracking the
 * page: end to end, a click navigated the browser (the web server logged the
 * hit) and the console went on showing the old page, repainting only the strips
 * that later pans fetched non-incrementally. An extra request costs ten bytes;
 * a missing one costs the session. */
static int send_request(void) {
  if (rq_pending) {
    rq_pending = 0;
    return req(rqx, rqy, rqw, rqh, 0);
  }
  return req(px, py, FB_W, FB_H, 1);
}

/* SERVER coordinates, not console ones: pan origin plus cursor. */
static int pointer(uint16_t x, uint16_t y, uint8_t mask) {
  uint8_t m[6];
  m[0] = 5;
  m[1] = mask;
  put16(m + 2, x);
  put16(m + 4, y);
  return wr(m, 6);
}

/* One wheel click: press and release at the cursor, in SERVER coordinates.
 *
 * A wheel is a BUTTON in RFB — 4 is up, 5 is down, so bit 3 and bit 4 of the
 * mask — and a click of it is the same press/release pair a real button gets.
 * There is no "wheel released" state to hold, so the two messages go out back
 * to back rather than being spread over two key events.
 *
 * The coordinates matter as much as the mask: a wheel event scrolls whatever is
 * under the pointer, so `px + cx` is what makes J and K reach an inner pane
 * rather than always moving the outermost document — and the pan origin is
 * exactly the part a click got wrong once already. */
static void wheel(uint8_t mask) {
  pointer((uint16_t)(px + cx), (uint16_t)(py + cy), mask);
  pointer((uint16_t)(px + cx), (uint16_t)(py + cy), 0);
}

/* ------------------------------------------------------------------ display */

/* CGRAM as the 3:3:2 cube the server is now sending indices into. Built here
 * rather than shipped as a table because the arithmetic is shorter than the
 * 512 bytes it would occupy. */
static void load_palette(void) {
  uint16_t i;
  R8(0x2121) = 0;
  for (i = 0; i < 256; i++) {
    uint16_t r = (uint16_t)((i >> 5) & 7);
    uint16_t g = (uint16_t)((i >> 2) & 7);
    uint16_t b = (uint16_t)(i & 3);
    /* 3 bits -> 5 by replicating the top bits, which keeps white at white. */
    uint16_t r5 = (uint16_t)((r << 2) | (r >> 1));
    uint16_t g5 = (uint16_t)((g << 2) | (g >> 1));
    uint16_t b5 = (uint16_t)((b << 3) | (b << 1) | (b >> 1));
    uint16_t v = (uint16_t)(r5 | (g5 << 5) | (b5 << 10));
    R8(0x2122) = (uint8_t)v;
    R8(0x2122) = (uint8_t)(v >> 8);
  }
}

static const uint8_t ARROW[8] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0x90, 0x10};
static const uint8_t EDGE[8] = {0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFC, 0xF8, 0x38};

static void load_cursor(void) {
  int i;
  vaddr(VRAM_OBJ >> 1);
  for (i = 0; i < 8; i++) {
    uint16_t w = (uint16_t)(ARROW[i] | ((uint16_t)(EDGE[i] & ~ARROW[i]) << 8));
    R8(0x2118) = (uint8_t)w;
    R8(0x2119) = (uint8_t)(w >> 8);
  }
  for (i = 0; i < 8; i++) {
    R8(0x2118) = 0;
    R8(0x2119) = 0;
  }
  /* Sprite palette 0 lives at CGRAM 128. The framebuffer owns all 256 entries,
   * so two of them are spent on a cursor that is always visible — a fair trade
   * against hunting for a grey arrow on a grey window. */
  R8(0x2121) = 129;
  R8(0x2122) = 0xFF; R8(0x2122) = 0x7F;   /* white */
  R8(0x2122) = 0x00; R8(0x2122) = 0x00;   /* black */
}

/* Move the cursor sprite — WHILE BLANKING, which is the whole of the second bug.
 *
 * OAM is not like VRAM. A write to $2104 during active display does not land
 * where OAMADDR points: sprite evaluation is using the OAM address counter
 * itself, so the byte goes wherever the renderer happens to be. The nine writes
 * below therefore did nothing at all, and the cursor sat where it was drawn.
 *
 * That is precisely why ESC worked and the arrow keys did not, and it is worth
 * spelling out because it looked exactly like a lost keystroke and sent me
 * hunting through Mesen's bindings, kbdbridge's key names and the kernel's d-pad
 * mapping — all three of which were fine. ESC has an effect that is not an OAM
 * write; an arrow key has nothing else. The harness settled it in one run: after
 * a RIGHT, OAM byte 0 was unchanged at 128 while the app's own pointer event
 * reported x=136. The key arrived, `cx` moved, and only the OAM write was lost.
 *
 * The fix is the same window the tiles use, for the same reason: below the
 * picture, forced blank, OAM writable. One frame of latency per move, and only
 * when something actually moved. */
static void place_cursor(void) {
  win_open();
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  R8(0x2104) = (uint8_t)cx;
  R8(0x2104) = (uint8_t)(cy ? cy - 1 : 0);
  R8(0x2104) = 0x00;
  R8(0x2104) = 0x30;
  R8(0x2102) = 0x00;
  R8(0x2103) = 0x01;
  R8(0x2104) = (uint8_t)((cx > 255) ? 0x01 : 0x00);
  win_close();
}

/* The marker's two sprite tiles, sized to the window's share of the desktop.
 *
 * Colour 1 — the same white the cursor's body uses, so no third CGRAM entry is
 * spent on this. 4bpp: plane 0 carries the whole shape and planes 1-3 are zero.
 *
 * The BOTTOM half of each tile is written as well, and that is not tidiness: the
 * sprite tile base is $C000, clear_tiles only reaches $BFFF, and what is left up
 * there is whatever the console's font put in VRAM before the app took the PPU.
 * An unwritten row of a sprite tile is not blank, it is somebody else's glyph. */
static void load_marker(void) {
  uint16_t t, row;
  R8(0x2115) = 0x80;
  vaddr((VRAM_OBJ + 32) >> 1);              /* sprite tiles 1 and 2 */
  for (t = 0; t < 2; t++) {
    uint16_t x0 = (uint16_t)(t << 3);
    uint16_t n = (mm_mw > x0) ? (uint16_t)(mm_mw - x0) : 0;
    if (n > 8) n = 8;
    for (row = 0; row < 8; row++) {
      /* 0xFF << (8 - n) truncated to a byte: n pixels, left-aligned. */
      R8(0x2118) = (uint8_t)((row < mm_mh && n) ? (0xFF << (8 - n)) : 0);
      R8(0x2119) = 0;                       /* plane 1 */
    }
    for (row = 0; row < 8; row++) {         /* planes 2 and 3 */
      R8(0x2118) = 0;
      R8(0x2119) = 0;
    }
  }
}

/* Move the marker. Same window rule and same reason as place_cursor: OAM takes
 * writes only while blanking, and there is no vblank to have them in.
 *
 * Sprite Y is one less than the scanline the sprite appears on, which is why
 * place_cursor writes cy - 1; MM_Y0 is 214 so nothing here can underflow. */
static void place_marker(void) {
  uint16_t mx, my;
  if (!mm_on) return;
  mx = (uint16_t)(MM_X0 + scale((uint16_t)(px >> 3), mm_travx,
                                (uint16_t)(maxpx >> 3)));
  my = (uint16_t)(MM_Y0 + scale((uint16_t)(py >> 3), mm_travy,
                                (uint16_t)(maxpy >> 3)));
  win_open();
  R8(0x2102) = 0x02;                        /* OAM word 2 = sprite 1, byte 4 */
  R8(0x2103) = 0x00;
  R8(0x2104) = (uint8_t)mx;
  R8(0x2104) = (uint8_t)(my - 1);
  R8(0x2104) = 0x01;                        /* sprite tile 1 */
  R8(0x2104) = 0x30;                        /* priority 3, palette 0 */
  R8(0x2104) = (uint8_t)(mx + 8);           /* sprite 2, straight after it */
  R8(0x2104) = (uint8_t)(my - 1);
  R8(0x2104) = 0x02;                        /* sprite tile 2 */
  R8(0x2104) = 0x30;
  win_close();
}

/* The outer frame: eight 8bpp tiles, a one-pixel rectangle MM_W x MM_H whose
 * bottom edge is the last displayed scanline.
 *
 * Called from setup_ppu, under the forced blank main() is still holding, so
 * there is no window to wait for.
 *
 * 8bpp tile data is four PAIRS of bitplanes, eight rows each, low plane in the
 * low byte — the same order tile.s produces and the same order the test decodes
 * in. A solid-colour shape needs no per-pixel work: every plane byte is either
 * the row's mask or zero, depending on one bit of the colour index. */
static void load_minimap(void) {
  uint16_t tr, tc, row, pl;
  uint8_t mask[8];
  R8(0x2115) = 0x80;
  vaddr(VRAM_MM >> 1);
  for (tr = 0; tr < MM_TR; tr++) {
    for (tc = 0; tc < MM_TW; tc++) {
      for (row = 0; row < 8; row++) {
        /* Which row of the box this tile row is, if any. The first tile row
         * starts above MM_Y0, so its top rows are outside the box. */
        uint16_t sy = (uint16_t)(((MM_R0 + tr) << 3) + row);
        uint8_t m = 0;
        if (sy >= MM_Y0 && sy < MM_Y0 + MM_H) {
          if (sy == MM_Y0 || sy == MM_Y0 + MM_H - 1) {
            m = 0xFF;                       /* top and bottom edges */
          } else {
            if (tc == 0) m = 0x80;          /* left edge */
            if (tc == MM_TW - 1) m = (uint8_t)(m | 0x01);
          }
        }
        mask[row] = m;
      }
      for (pl = 0; pl < 8; pl = (uint16_t)(pl + 2))
        for (row = 0; row < 8; row++) {
          R8(0x2118) = (MM_INK & (1 << pl)) ? mask[row] : 0;
          R8(0x2119) = (MM_INK & (1 << (pl + 1))) ? mask[row] : 0;
        }
    }
  }
}

static void hide_sprites(void) {
  int i;
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  for (i = 0; i < 128; i++) {
    R8(0x2104) = 0;
    R8(0x2104) = 0xF0;
    R8(0x2104) = 0;
    R8(0x2104) = 0;
  }
}

/* ------------------------------------------------------------- the tilemap
 *
 * Which tile map cell (r, c) shows. WITHOUT panning this was the identity —
 * cell (r, c) shows tile r*32+c — and that identity is exactly what let the
 * rest of the app treat a shadow offset and a screen position as the same
 * thing. They are not the same thing any more.
 *
 * The shadow holds server pixel (sx, sy) at (sx mod 256, sy mod 192) for ever,
 * so the tile holding the top-left of the window is whichever tile the pan
 * origin lands in, and the row and column both wrap. Rows past the picture keep
 * pointing at the all-zero tile, which is what keeps scanlines 192-223 black
 * and therefore what keeps the write window usable. */
static uint16_t map_entry(uint16_t r, uint16_t c) {
  uint16_t ty, tx;
  if (r >= TILES_H) {
    /* The minimap's own cells, and only while there is something to pan over.
     * Everything else below the picture stays on the all-zero tile — the write
     * window's blank covers scanlines 192 to about 213, so whatever those rows
     * show is torn in and out several times a second, and black tears
     * invisibly. See "the minimap" at the top. */
    if (mm_on && r >= MM_R0 && r < MM_R0 + MM_TR &&
        c >= MM_C0 && c < MM_C0 + MM_TW)
      return (uint16_t)(TILE_MM + ((r - MM_R0) << MM_TSH) + (c - MM_C0));
    return TILE_BLANK;
  }
  ty = (uint16_t)((pym >> 3) + r);
  while (ty >= TILES_H) ty = (uint16_t)(ty - TILES_H);
  tx = (uint16_t)((pxm >> 3) + c);
  while (tx >= TILES_W) tx = (uint16_t)(tx - TILES_W);
  return (uint16_t)((ty << 5) + tx);
}

/* Rewrite the picture rows of the tilemap for the current pan origin.
 *
 * THIS IS THE WHOLE OF A PAN, as far as everything already on screen is
 * concerned. 1.5 KB of tilemap instead of 48 KB of tile data, and not one pixel
 * of the shadow moves.
 *
 * The staging buffer is BATCH x 32 words, which is eight tilemap rows exactly,
 * and tilemap rows are contiguous in VRAM — so a band is one 512-byte DMA and
 * the whole map is three of them. Building a band costs about half a frame
 * outside the window; the DMA costs about a scanline and a half inside it. */
static void write_tilemap(void) {
  uint16_t r0, r, c;
  uint16_t *out;
  for (r0 = 0; r0 < TILES_H; r0 = (uint16_t)(r0 + BATCH)) {
    out = &stage[0][0];
    for (r = 0; r < BATCH; r++)
      for (c = 0; c < 32; c++) *out++ = map_entry((uint16_t)(r0 + r), c);
    /* Re-assert VMAIN inside the same guard flush_batch uses, and for the same
     * reason: anything that ran in between may have left the increment mode
     * somewhere else, and a tilemap written with the address advancing after
     * the LOW byte is a screen of scattered garbage. */
    R8(0x2115) = 0x80;
    win_open();
    dma_tiles((uint16_t)(((VRAM_MAP >> 1) + (r0 << 5))), &stage[0][0],
              (uint16_t)(BATCH * 32u));
    win_close();
  }
}

static void setup_ppu(void) {
  uint16_t r, c;

  /* One tile of solid black, for the four displayed rows below the picture.
   *
   * They used to show tile 0 — the top-left tile of the desktop — repeated
   * thirty-two times across the bottom of the screen, which was its own small
   * eyesore. Making the strip genuinely black also makes it USEFUL: it is where
   * every VRAM and OAM write in this app now happens, under a forced blank that
   * covers only these lines. See "the write window" above. */
  vaddr(VRAM_BLANK >> 1);
  for (r = 0; r < 32; r++) {
    R8(0x2118) = 0;
    R8(0x2119) = 0;
  }

  /* The minimap's frame, before the map that points at it. */
  load_minimap();

  /* The whole map, written directly: the display is still under the forced
   * blank main() asserted, so there is no window to wait for. At pan (0,0)
   * map_entry() is the identity this used to spell out inline — except for the
   * strip's minimap cells, which is why the rows below the picture are no
   * longer all the same entry. */
  vaddr(VRAM_MAP >> 1);
  for (r = 0; r < 32; r++) {
    for (c = 0; c < 32; c++) {
      uint16_t t = map_entry(r, c);
      R8(0x2118) = (uint8_t)t;
      R8(0x2119) = (uint8_t)(t >> 8);
    }
  }

  R8(0x2105) = 0x03;                             /* Mode 3, BG1 8bpp */
  R8(0x210D) = 0; R8(0x210D) = 0;
  R8(0x210E) = 0; R8(0x210E) = 0;
  /* BG1SC. The base field counts 1K-WORD units, and VRAM_MAP here is a BYTE
   * address — so it has to be halved first. view.c's version of this line looks
   * identical but its VRAM_MAP is already a word address, and copying the
   * formula without the units gave 0xE0 instead of 0x70: a tilemap base past the
   * end of VRAM. The PPU then read tile INDICES from the wrong place, selected
   * arbitrary tiles, and drew scattered noise over a picture whose tile data was
   * perfectly correct all along.
   *
   * That is why it survived every check I made. My verification decoded tile
   * data in assumed tilemap order, which bypasses the tilemap entirely — so it
   * confirmed the one thing that was never broken. Ricardo's register log found
   * it in a single run.
   *
   * It emits 0x70 now, which is what this note always claimed. Until VRAM_MAP
   * gained its `u` it was emitting 0xF0 — a base of 60 K-words on a 32 K-word
   * VRAM, which wrapped to the same place and worked. See the note on the VRAM
   * addresses for why the constant was negative and what else it cost. */
  R8(0x2107) = (uint8_t)((((uint16_t)(VRAM_MAP >> 1)) >> 10) << 2);
  R8(0x210B) = 0x00;                             /* BG1 tiles at $0000 */
  R8(0x2101) = 0x03;                             /* OBSEL: 8x8, base $C000 */
  R8(0x212C) = 0x11;                             /* BG1 + OBJ */
  R8(0x212D) = 0x00;
  R8(0x2100) = 0x0F;
}

/* The transpose, in tile.s. The shadow is linear bytes, the PPU wants eight
 * bitplanes, and a tile is 8 rows of 8 pixels producing 32 words in the order
 * (planes 0&1) x8, (planes 2&3) x8, (4&5) x8, (6&7) x8 — 512 bit extractions,
 * which is why the C that used to be here cost 0.93 of a frame per tile at -O0
 * and why it is hand-written 65816 now. See tile.s for what it does, what it
 * costs, and the two faster-looking shapes that were costed and lost. */
extern void tile_transpose(const uint8_t *src, uint16_t *out);

/* Rebuild one tile from the shadow into the staging buffer.
 *
 * `t` is a SHADOW tile, not a screen position — the tilemap is what decides
 * where it appears — so this is untouched by panning.
 *
 * It runs OUTSIDE any window, which is why the result goes to WRAM rather than
 * straight to $2118: even at 63k master clocks a tile the transpose is a fifth
 * of a frame, so a tile has never fitted in a window and never will. Staged in
 * WRAM it becomes one DMA, which does.
 *
 * TILES_W is 32 and FB_W is 256, both powers of two, so none of the arithmetic
 * below reaches for the software multiply and divide that a 65816 needs for
 * anything else — check the listing rather than the source if that ever changes,
 * because a `* FB_W` that stops folding into shifts costs more than the
 * transpose it feeds. */
static void build_tile(uint16_t t, uint16_t *out) {
  uint16_t tx = (uint16_t)(t % TILES_W), ty = (uint16_t)(t / TILES_W);
  uint16_t base = (uint16_t)(ty * 8u * FB_W + tx * 8u);
  tile_transpose(&fb[base], out);
}

/* Push a staged batch through one write window. Returns how many tiles landed.
 *
 * A tile is only marked clean once its transfer has been issued inside the
 * window, which is con_flush's rule and there for the same reason: a write the
 * hardware ignores leaves no trace except a stale square on screen for ever, so
 * the flag must follow the transfer rather than the intention. */
static uint16_t flush_batch(uint16_t n) {
  uint16_t i, done = 0;

  /* Re-assert VMAIN on every pass, not once at startup. Anything that runs
   * between two flushes — and sys_yield() reaches the console and the keyboard
   * panel — can leave the VRAM increment mode somewhere else, and the result is
   * scrambled bitplanes on whichever tiles were written afterwards: scattered
   * noise on an otherwise black screen. One register write is cheaper than
   * reasoning about who else might have touched it. */
  R8(0x2115) = 0x80;
  win_open();
  for (i = 0; i < n; i++) {
    if (!win_room()) break;
    dma_tiles((uint16_t)(stage_t[i] * 32u), stage[i], 32);
    dirty[stage_t[i]] = 0;
    done++;
  }
  win_close();
  return done;
}

/* Paint every dirty tile, without ever blanking anything that is on screen.
 *
 * The forced blank this replaces was the flicker. `R8(0x2100) = 0x8F` around the
 * whole flush is correct — VRAM takes writes only while blanking — but
 * flush_dirty runs once per RECEIVED RECTANGLE, and a rectangle is hundreds of
 * tiles at 1.5 frames each, so the WHOLE SCREEN went dark and light again for
 * every one of them. Measured against a server sending a screen as six bands:
 * six episodes of 207 frames, 1242 frames of black alternating with the picture.
 * That is exactly the "alternating between data and black" Ricardo saw.
 *
 * The cost is unchanged — the transpose was always the expensive part — and the
 * blanking now happens where nothing is displayed. */
static void flush_dirty(void) {
  uint16_t from = 0;
  int missed = 0;

  for (;;) {
    uint16_t t, n = 0;
    for (t = from; t < NTILES && n < BATCH; t++) {
      if (!dirty[t]) continue;
      build_tile(t, stage[n]);
      stage_t[n] = t;
      n++;
    }
    from = t;
    if (n && flush_batch(n) < n) missed = 1;
    if (from >= NTILES) {
      /* Normally one linear sweep and done. Tiles a window ran out on are still
       * dirty, so sweep again — and since every window lands at least one tile,
       * every sweep makes progress. */
      if (!missed) return;
      from = 0;
      missed = 0;
    }
  }
}

/* The one blank that is honest: VRAM still holds the console's font when an app
 * takes the PPU, so the tiles are zeroed before the display is turned back on.
 *
 * No transpose and no window to wait for — every tile is the same 64 zero bytes
 * and the display is not on yet — so this is a couple of frames. Painting 768
 * black tiles through flush_dirty instead took 1015 frames of blank screen at
 * startup, and every one of them was spent transposing zeroes. */
static void clear_tiles(void) {
  uint16_t n, i, t;
  for (n = 0; n < BATCH; n++)
    for (i = 0; i < 32; i++) stage[n][i] = 0;
  R8(0x2115) = 0x80;
  for (t = 0; t < NTILES; t = (uint16_t)(t + BATCH))
    dma_tiles((uint16_t)(t * 32u), stage[0], (uint16_t)(BATCH * 32u));
}

/* ------------------------------------------------------------ the torus
 *
 * Console position -> shadow address, for a window whose origin is (px, py).
 * Both wrap, because the shadow holds server pixel (sx, sy) at
 * (sx mod 256, sy mod 192) regardless of where the window is. That is the
 * property that makes a pan free for everything already on screen.
 *
 * shrow() returns a byte offset, not a row number, because FB_W is 256 and the
 * multiply is therefore a shift — and the 65816 has no multiply instruction, so
 * "* FB_W" per row would otherwise be a call. */
static uint16_t shrow(uint16_t vy) {
  uint16_t s = (uint16_t)(vy + pym);
  if (s >= FB_H) s = (uint16_t)(s - FB_H);
  return (uint16_t)(s << 8);
}

static uint16_t shcol(uint16_t vx) {
  uint16_t s = (uint16_t)(vx + pxm);
  if (s >= FB_W) s = (uint16_t)(s - FB_W);
  return s;
}

/* Mark the shadow tiles a console-space rectangle covers. Wraps with the
 * shadow, which is why this cannot just clamp the way it used to. */
static void mark(uint16_t vx, uint16_t vy, uint16_t w, uint16_t h) {
  uint16_t c0 = (uint16_t)(vx >> 3), r0 = (uint16_t)(vy >> 3);
  uint16_t nc = (uint16_t)(((vx + w + 7u) >> 3) - c0);
  uint16_t nr = (uint16_t)(((vy + h + 7u) >> 3) - r0);
  uint16_t co = (uint16_t)(c0 + (pxm >> 3)), ro = (uint16_t)(r0 + (pym >> 3));
  uint16_t r, c;
  for (r = 0; r < nr; r++) {
    uint16_t ty = (uint16_t)(ro + r), base;
    while (ty >= TILES_H) ty = (uint16_t)(ty - TILES_H);
    base = (uint16_t)(ty << 5);
    for (c = 0; c < nc; c++) {
      uint16_t tx = (uint16_t)(co + c);
      while (tx >= TILES_W) tx = (uint16_t)(tx - TILES_W);
      dirty[base + tx] = 1;
    }
  }
}

/* --------------------------------------------------------------- panning */

static void set_pan(uint16_t nx, uint16_t ny) {
  px = nx;
  py = ny;
  pxm = px;
  while (pxm >= FB_W) pxm = (uint16_t)(pxm - FB_W);
  pym = py;
  while (pym >= FB_H) pym = (uint16_t)(pym - FB_H);
}

/* Move the window, one axis at a time.
 *
 * One axis, even when both are pending, so that the exposed area is always a
 * single strip and the bounding box in need_rect() is exact. A diagonal step
 * would make it the whole window, which is the fourteen-second request this
 * exists to avoid; the other axis just lands on the next pass.
 *
 * The pixels do not move. Only the tilemap does, and only the strip that was
 * never on screen has to come down the wire. */
static void apply_pan(void) {
  uint16_t nx = px, ny = py, ox = px, oy = py, d;

  if (pend_dx) {
    int16_t dx = pend_dx;
    pend_dx = 0;
    if (dx > 0) {
      nx = (uint16_t)(px + (uint16_t)dx);
      if (nx > maxpx) nx = maxpx;
    } else {
      uint16_t m = (uint16_t)(-dx);
      nx = (px > m) ? (uint16_t)(px - m) : 0;
    }
  } else if (pend_dy) {
    int16_t dy = pend_dy;
    pend_dy = 0;
    if (dy > 0) {
      ny = (uint16_t)(py + (uint16_t)dy);
      if (ny > maxpy) ny = maxpy;
    } else {
      uint16_t m = (uint16_t)(-dy);
      ny = (py > m) ? (uint16_t)(py - m) : 0;
    }
  }
  if (nx == ox && ny == oy) return;

  set_pan(nx, ny);
  write_tilemap();
  /* The one thing on screen that says where in the desktop this window is. Four
   * bytes of OAM per sprite, in the same window write_tilemap just used. */
  place_marker();

  /* The strip that has never been on this console. Everything else in the new
   * window is already in the shadow, at the address it was always at. */
  if (nx > ox) {
    d = (uint16_t)(nx - ox);
    if (d > FB_W) d = FB_W;
    need_rect((uint16_t)(nx + FB_W - d), ny, d, FB_H);
  } else if (nx < ox) {
    d = (uint16_t)(ox - nx);
    if (d > FB_W) d = FB_W;
    need_rect(nx, ny, d, FB_H);
  } else if (ny > oy) {
    d = (uint16_t)(ny - oy);
    if (d > FB_H) d = FB_H;
    need_rect(nx, (uint16_t)(ny + FB_H - d), FB_W, d);
  } else {
    d = (uint16_t)(oy - ny);
    if (d > FB_H) d = FB_H;
    need_rect(nx, ny, FB_W, d);
  }
}

/* Cursor, panning and quit, serviced while a transfer is in flight.
 *
 * This has to run from inside the read loop, not only between updates. A first
 * screen is 48 KB — half a minute even now, and eighty seconds when this was
 * written — and an application that only checks the keyboard between whole
 * updates ignores you for all of it. The
 * cursor is a sprite, so moving it mid-transfer costs four bytes of OAM and no
 * network traffic at all.
 *
 * A PAN, though, cannot happen mid-message. Rectangles are placed using the pan
 * origin, so moving the origin while one is half-read would put the rest of it
 * in the wrong place — and only sometimes, only on a large rectangle, which is
 * the worst kind of bug to own. So `parsing` gates it and the step is held until
 * the stream is at a message boundary.
 *
 * At a boundary it is applied HERE rather than deferred to the main loop, and
 * that is not an optimisation: a still screen leaves the app blocked in rd()
 * waiting for a message the server has no reason to send, and a pan deferred to
 * a loop that is never reached is a pan that never happens. Applying it here
 * also sends its own request, which is what unblocks the read.
 *
 * Returns 1 if the user asked to leave. */
static int poll_input(void) {
  int moved = 0, panned = 0;
  for (;;) {
    int c = (int)sys_pollkey();
    if (c == (int)SYS_ERR) break;
    if (c == 0x1B) {
      quit = 1;
      return 1;
    }
    if (c == SNOS_KEY_LEFT) {
      cx = (uint16_t)(cx >= 8 ? cx - 8 : 0);
      moved = 1;
    } else if (c == SNOS_KEY_RIGHT) {
      cx = (uint16_t)(cx < FB_W - 8 ? cx + 8 : FB_W - 1);
      moved = 1;
    } else if (c == SNOS_KEY_UP) {
      cy = (uint16_t)(cy >= 8 ? cy - 8 : 0);
      moved = 1;
    } else if (c == SNOS_KEY_DOWN) {
      cy = (uint16_t)(cy < FB_H - 8 ? cy + 8 : FB_H - 1);
      moved = 1;
    } else if (c == 'a' || c == 'A') {
      pend_dx = (int16_t)(pend_dx - PAN_STEP);
      panned = 1;
    } else if (c == 'd' || c == 'D') {
      pend_dx = (int16_t)(pend_dx + PAN_STEP);
      panned = 1;
    } else if (c == 'w' || c == 'W') {
      pend_dy = (int16_t)(pend_dy - PAN_STEP);
      panned = 1;
    } else if (c == 's' || c == 'S') {
      pend_dy = (int16_t)(pend_dy + PAN_STEP);
      panned = 1;
    } else if (c == 'j' || c == 'J') {
      wheel(16);                             /* button 5 — wheel down */
    } else if (c == 'k' || c == 'K') {
      wheel(8);                              /* button 4 — wheel up */
    } else if (c == ' ') {
      /* Press and release at the cursor, in SERVER coordinates. The server
       * moves its own pointer there as a side effect, which is why we do not
       * stream every cursor step: each one would cost a framebuffer update of
       * the area it left. */
      pointer((uint16_t)(px + cx), (uint16_t)(py + cy), 1);
      pointer((uint16_t)(px + cx), (uint16_t)(py + cy), 0);
    }
  }
  /* Drain the queue first, THEN place the sprite, once. Every key of a held
   * repeat would otherwise wait for its own window, and auto-repeat delivers
   * several per frame. */
  if (moved && ready) place_cursor();
  if (panned && ready && !parsing) {
    apply_pan();
    if (rq_pending) send_request();
  }
  return 0;
}

/* ------------------------------------------------------------------ updates */

/* CopyRect, translated through the torus.
 *
 * Both ends have to be inside the window: the shadow holds the window and
 * nothing else, so a source that has scrolled out of view simply is not here to
 * copy from. Whatever cannot be satisfied is handed to need_rect(), which turns
 * it into a small non-incremental request rather than a stale band nobody
 * notices until they read it.
 *
 * The row goes via crow[] rather than being copied in place. Source and
 * destination each wrap at their own point on the torus, so an in-place copy
 * would need overlap reasoning modulo 256; gathering the row and then scattering
 * it removes the question entirely. Row ORDER still matters, and that is the
 * one thing a scratch buffer does not fix — a scroll's source and destination
 * rows overlap, so the direction is chosen from the sign of the shift. */
static void copy_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t sx, uint16_t sy) {
  int32_t dxs = (int32_t)sx - (int32_t)x;
  int32_t dys = (int32_t)sy - (int32_t)y;
  uint16_t vx1 = (uint16_t)(px + FB_W), vy1 = (uint16_t)(py + FB_H);
  uint16_t ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
  int32_t lo, hi, k0, k1;

  /* A: the part of the destination that is on screen at all. */
  ax0 = (x < px) ? px : x;
  ay0 = (y < py) ? py : y;
  ax1 = ((uint16_t)(x + w) > vx1) ? vx1 : (uint16_t)(x + w);
  ay1 = ((uint16_t)(y + h) > vy1) ? vy1 : (uint16_t)(y + h);
  if (ax1 <= ax0 || ay1 <= ay0) return;

  /* B: the part of A whose SOURCE is on screen too. */
  lo = (int32_t)px - dxs;
  hi = (int32_t)vx1 - dxs;
  k0 = (int32_t)ax0; if (k0 < lo) k0 = lo;
  k1 = (int32_t)ax1; if (k1 > hi) k1 = hi;
  bx0 = (uint16_t)k0;
  bx1 = (uint16_t)((k1 > k0) ? k1 : k0);
  lo = (int32_t)py - dys;
  hi = (int32_t)vy1 - dys;
  k0 = (int32_t)ay0; if (k0 < lo) k0 = lo;
  k1 = (int32_t)ay1; if (k1 > hi) k1 = hi;
  by0 = (uint16_t)k0;
  by1 = (uint16_t)((k1 > k0) ? k1 : k0);

  if (bx1 > bx0 && by1 > by0) {
    uint16_t ncols = (uint16_t)(bx1 - bx0), nrows = (uint16_t)(by1 - by0);
    uint16_t sr0 = (uint16_t)((int32_t)by0 + dys);
    uint16_t sc0 = (uint16_t)((int32_t)bx0 + dxs);
    uint16_t i;
    for (i = 0; i < nrows; i++) {
      /* Source above the destination means the top rows are overwritten first,
       * so walk from the bottom. This is the only part of a scroll that a row
       * buffer does not make order-independent. */
      uint16_t j = (dys < 0) ? (uint16_t)(nrows - 1 - i) : i;
      uint16_t srow = shrow((uint16_t)((uint16_t)(sr0 + j) - py));
      uint16_t drow = shrow((uint16_t)((uint16_t)(by0 + j) - py));
      uint16_t ssc = shcol((uint16_t)(sc0 - px));
      uint16_t dsc = shcol((uint16_t)(bx0 - px));
      uint16_t n1, k;
      n1 = (uint16_t)(FB_W - ssc);
      if (n1 > ncols) n1 = ncols;
      for (k = 0; k < n1; k++) crow[k] = fb[srow + ssc + k];
      for (k = n1; k < ncols; k++) crow[k] = fb[srow + (k - n1)];
      n1 = (uint16_t)(FB_W - dsc);
      if (n1 > ncols) n1 = ncols;
      for (k = 0; k < n1; k++) fb[drow + dsc + k] = crow[k];
      for (k = n1; k < ncols; k++) fb[drow + (k - n1)] = crow[k];
    }
    mark((uint16_t)(bx0 - px), (uint16_t)(by0 - py), ncols, nrows);
    /* A minus B, as up to four strips. For a plain vertical scroll — the case
     * that actually happens — exactly one of them is non-empty and the bounding
     * box need_rect() keeps is therefore exact. */
    need_rect(ax0, ay0, (uint16_t)(ax1 - ax0), (uint16_t)(by0 - ay0));
    need_rect(ax0, by1, (uint16_t)(ax1 - ax0), (uint16_t)(ay1 - by1));
    need_rect(ax0, by0, (uint16_t)(bx0 - ax0), (uint16_t)(by1 - by0));
    need_rect(bx1, by0, (uint16_t)(ax1 - bx1), (uint16_t)(by1 - by0));
  } else {
    /* Nothing of the source survives on screen. */
    need_rect(ax0, ay0, (uint16_t)(ax1 - ax0), (uint16_t)(ay1 - ay0));
  }
}

static int read_update(void) {
  uint16_t nrects, i;

  if (!rd(hdr, 3)) return 0;               /* padding + rect count */
  nrects = be16(hdr + 1);

  for (i = 0; i < nrects; i++) {
    uint16_t x, y, w, h, row;
    int32_t enc;
    if (!rd(hdr, 12)) return 0;
    x = be16(hdr);
    y = be16(hdr + 2);
    w = be16(hdr + 4);
    h = be16(hdr + 6);
    enc = (int32_t)(((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                    ((uint32_t)hdr[10] << 8) | hdr[11]);

    /* A rectangle larger than the desktop is not a rectangle we misplaced, it
     * is a stream we have lost sync with — and the arithmetic below would
     * overflow 16 bits trying to place it. */
    if (w > srv_w || h > srv_h || x > srv_w || y > srv_h) {
      say("\nserver sent a rectangle off its own desktop\n");
      return 0;
    }
    if (w == 0 || h == 0) continue;

    if (enc == ENC_RAW) {
      /* Rectangles arrive in SERVER coordinates. Clip to the window, and drop
       * whatever falls outside it — the shadow byte an off-window pixel would
       * land on belongs to a different part of the desktop, so writing it there
       * would corrupt something that IS on screen. */
      uint16_t vx1 = (uint16_t)(px + FB_W), vy1 = (uint16_t)(py + FB_H);
      uint16_t cx0 = (x < px) ? px : x;
      uint16_t cy0 = (y < py) ? py : y;
      uint16_t cx1 = ((uint16_t)(x + w) > vx1) ? vx1 : (uint16_t)(x + w);
      uint16_t cy1 = ((uint16_t)(y + h) > vy1) ? vy1 : (uint16_t)(y + h);
      int any = (cx1 > cx0) && (cy1 > cy0);
      uint16_t pre = any ? (uint16_t)(cx0 - x) : 0;
      uint16_t mid = any ? (uint16_t)(cx1 - cx0) : 0;
      uint16_t post = any ? (uint16_t)((uint16_t)(x + w) - cx1) : 0;

      /* Straight into the shadow, a row at a time. No intermediate buffer:
       * a single rectangle can be 26 KB and there is nowhere to put it. */
      for (row = 0; row < h; row++) {
        uint16_t sy = (uint16_t)(y + row);
        uint16_t base, sc, n1;
        if (!any || sy < cy0 || sy >= cy1) {
          if (!skipn(w)) return 0;
          continue;
        }
        if (pre && !skipn(pre)) return 0;
        base = shrow((uint16_t)(sy - py));
        sc = shcol((uint16_t)(cx0 - px));
        n1 = (uint16_t)(FB_W - sc);
        if (n1 > mid) n1 = mid;
        if (!rd(&fb[base + sc], n1)) return 0;
        if (n1 < mid && !rd(&fb[base], (uint16_t)(mid - n1))) return 0;
        if (post && !skipn(post)) return 0;
      }
      if (any) {
        mark((uint16_t)(cx0 - px), (uint16_t)(cy0 - py), mid,
             (uint16_t)(cy1 - cy0));
        /* Paint it NOW, not after the whole update. A server may split one
         * screen across several messages, so waiting meant painting a fraction
         * of the screen with stale VRAM — the console's own font — showing
         * through the rest. That was the corruption. It also turns an
         * eighty-second first screen into something you can watch arrive. */
        flush_dirty();
      }
    } else if (enc == ENC_COPYRECT) {
      uint16_t sx, sy;
      if (!rd(hdr, 4)) return 0;
      sx = be16(hdr);
      sy = be16(hdr + 2);
      copy_rect(x, y, w, h, sx, sy);
    } else {
      /* We advertised only Raw and CopyRect; anything else means the stream is
       * no longer parseable and guessing would make it worse. */
      say("\nserver used an encoding we did not ask for\n");
      return 0;
    }
  }
  return 1;
}

/* Read one server message, handling the ones that are not updates.
 *
 * `parsing` brackets everything after the type byte: while it is set, a pan is
 * recorded but not applied, because the pan origin is what places every
 * rectangle in the message being read. The one place it is CLEAR and the app is
 * blocked is the read of the type byte itself, which is exactly the boundary a
 * pan is safe at. */
static int pump(void) {
  int ok;
  if (!rd(hdr, 1)) return 0;
  parsing = 1;
  if (hdr[0] == MSG_UPDATE) {
    ok = read_update();
  } else if (hdr[0] == MSG_BELL) {
    ok = 1;
  } else if (hdr[0] == MSG_COLOURMAP) {
    uint16_t n;
    ok = 0;
    if (rd(hdr, 5)) {
      n = be16(hdr + 3);
      ok = 1;
      while (n--) if (!rd(hdr, 6)) { ok = 0; break; }
    }
  } else if (hdr[0] == MSG_CUT) {
    uint32_t n;
    ok = 0;
    if (rd(hdr, 7)) {
      n = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[4] << 16) |
          ((uint32_t)hdr[5] << 8) | hdr[6];
      ok = 1;
      while (n) {
        uint16_t take = (n > sizeof hdr) ? (uint16_t)sizeof hdr : (uint16_t)n;
        if (!rd(hdr, take)) { ok = 0; break; }
        n -= take;
      }
    }
  } else {
    say("\nunknown message from the server\n");
    ok = 0;
  }
  parsing = 0;
  return ok;
}

/* ---------------------------------------------------------------- the loop */

int main(void) {
  uint16_t i;

  /* Assigned here, not at their declarations: an app is linked with
   * --no-auto-libraries, so an initialised static reads back as zero and a
   * non-zero initialiser is silently discarded. */
  cx = FB_W / 2;
  cy = FB_H / 2;
  quit = 0;
  px = 0; py = 0; pxm = 0; pym = 0;
  maxpx = 0; maxpy = 0;
  pend_dx = 0; pend_dy = 0;
  parsing = 0;
  ready = 0;
  rq_pending = 0;
  mm_mw = 0; mm_mh = 0; mm_travx = 0; mm_travy = 0;
  mm_on = 0;

  snos_getargs(args, sizeof args);
  if (args[0] == 0) {
    say("usage: vnc HOST[:PORT]\n");
    return 1;
  }
  if (!sys_net_up()) {
    say("no gateway attached\n");
    return 1;
  }

  /* Default the port here rather than making the user type 5900 every time. */
  {
    int has_port = 0;
    for (i = 0; args[i]; i++)
      if (args[i] == ':') has_port = 1;
    if (!has_port) {
      const char *p = ":5900";
      while (*p && i < SNOS_ARGV_MAX - 1) args[i++] = *p++;
      args[i] = 0;
    }
  }

  say("connecting to ");
  say(args);
  sys_putc('\n');

  ch = sys_net_connect(args);
  if (ch == SYS_ERR) {
    say("no free channel\n");
    return 1;
  }
  {
    uint16_t deadline = (uint16_t)(sys_frames() + 1800u);
    while (sys_net_state(ch) == NET_ST_OPENING) {
      if ((int16_t)(sys_frames() - deadline) >= 0) {
        say("connection timed out\n");
        sys_net_close(ch);
        return 1;
      }
      sys_yield();
    }
  }
  if (sys_net_state(ch) != NET_ST_OPEN) {
    say("could not connect\n");
    sys_net_close(ch);
    return 1;
  }

  if (!handshake()) {
    sys_net_close(ch);
    return 1;
  }

  /* How far the window may travel. Rounded DOWN to a multiple of 8 so that
   * every pan origin stays on a tile boundary even at the far edge — the
   * tilemap cannot express anything else, and a half-tile origin would quietly
   * turn every pan into a full 768-tile rebuild. */
  maxpx = (srv_w > FB_W) ? (uint16_t)((srv_w - FB_W) & 0xFFF8u) : 0;
  maxpy = (srv_h > FB_H) ? (uint16_t)((srv_h - FB_H) & 0xFFF8u) : 0;

  /* The marker is the window's share of the desktop, and its travel is the rest
   * of the frame — so the two together always fill it exactly, whatever the
   * server's size. Settled once, here, because ServerInit is the only place the
   * desktop's size is ever learned.
   *
   * Travel comes from the TRUE share even when the drawn width is clamped to
   * what two sprites can cover: under-filling the frame is a marker that reads
   * slightly small, whereas travelling by the clamped figure would walk it off
   * the right-hand edge. */
  mm_on = (maxpx != 0 || maxpy != 0);
  mm_mw = scale((uint16_t)(FB_W >> 3), MM_W, (uint16_t)(srv_w >> 3));
  mm_mh = scale((uint16_t)(FB_H >> 3), MM_H, (uint16_t)(srv_h >> 3));
  if (mm_mw < 1) mm_mw = 1;
  if (mm_mh < 1) mm_mh = 1;
  mm_travx = (mm_mw < MM_W) ? (uint16_t)(MM_W - mm_mw) : 0;
  mm_travy = (mm_mh < MM_H) ? (uint16_t)(MM_H - mm_mh) : 0;
  if (mm_mw > MM_MARK_W) mm_mw = MM_MARK_W;
  if (mm_mh > MM_MARK_H) mm_mh = MM_MARK_H;

  say("connected: ");
  sys_putu(srv_w);
  sys_putc('x');
  sys_putu(srv_h);
  sys_putc('\n');
  if (maxpx || maxpy)
    say("WASD pans, arrows move the cursor, space clicks\n");
  else
    say("arrows move the cursor, space clicks\n");
  say("J and K scroll the page; ESC quits\n");

  /* Start from a known screen: clear the shadow, and clear VRAM to match it, so
   * anything the server has not sent yet is black rather than whatever the
   * console left in VRAM. "Not arrived" then looks like not arrived. */
  /* One index, no multiply. The nested version of this guarded against a 16-bit
   * overflow that cannot happen — FB_SIZE is 49152 and a uint16_t reaches 65535
   * — and paid for it with a runtime multiply per byte, because the 65816 has no
   * multiply instruction. Forty-nine thousand calls to a software multiply is a
   * hang from the outside: it never reached the next print. */
  {
    uint16_t k;
    for (k = 0; k < FB_SIZE; k++) fb[k] = 0;
  }
  for (i = 0; i < NTILES; i++) dirty[i] = 0;

  say("receiving the first screen - about 30s on this link\n");
  say("it paints as it arrives; ESC quits\n");
  sys_gfx(1);
  R8(0x2100) = 0x8F;
  /* Increment the VRAM address after the HIGH byte, so a low/high pair writes
   * one word. Without this the console's last setting applies, the address
   * advances after the LOW byte, and every word is split across two addresses —
   * which paints the console's own font back at you, scrambled. `view` has
   * always set this; copying its VRAM layout without its VMAIN was the bug. */
  R8(0x2115) = 0x80;
  /* Stop any HDMA before touching the display. The keyboard panel drives HDMA
   * on channels 3/4/5 and rewrites BG registers every scanline, which would
   * shred a Mode 3 picture. NOT the cause of the corruption this was added
   * for — Ricardo's log showed $420C was already 00 — but an app that takes the
   * display should not depend on somebody else's teardown. */
  R8(0x420C) = 0x00;
  load_palette();
  hide_sprites();
  load_cursor();
  load_marker();
  clear_tiles();
  setup_ppu();
  ready = 1;
  /* After setup_ppu, so the display is already on when the cursor lands: OAM
   * writes are only reliable while blanking, and place_cursor waits for a vblank
   * to get that. Under the forced blank above it would work either way; doing it
   * here keeps one rule instead of two. */
  place_cursor();
  /* After place_cursor, which is what clears the OAM high table byte the two of
   * them share — both sprites are 8x8 and neither ever passes x=255, so every
   * bit in it is zero, but it has to be written by somebody. */
  place_marker();

  /* The first screen is the window, in full, and the server has no idea what we
   * already have — so it is the same non-incremental request a pan uses. */
  need_rect(px, py, FB_W, FB_H);

  while (!quit) {
    if (pend_dx || pend_dy) apply_pan();
    if (!send_request()) break;
    if (!pump()) break;
    /* Rectangles are painted as they arrive, inside read_update; this catches
     * anything a CopyRect left dirty, since that path marks without painting. */
    flush_dirty();
    /* Drain the keys that arrived while a screenful was being read. The cursor
     * moves locally with no round trip, which is the whole reason it is a
     * sprite — at 615 B/s a server-drawn cursor would lag by a second. */
    poll_input();
  }

  R8(0x2100) = 0x8F;
  sys_gfx(0);
  sys_net_close(ch);
  say("\n");
  return 0;
}
