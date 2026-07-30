#!/bin/sh
# The desktop is BIGGER than the console on purpose.
#
# At 256x192 Chromium lays a page out at 256 CSS pixels and the text wraps into
# a column too narrow to read. The console shows a 256x192 window into this and
# pans it with WASD (apps/vnc/vnc.c), so the glyphs stay the size they were and
# four times as much page is reachable.
#
# These two are the tuning knobs, and they are meant to be tuned by feel. Keep
# them EQUAL — chromium's window is the whole desktop, so a mismatch gives
# either a scrollbar or a border of root window. GEOM must also stay a multiple
# of 8 in both axes, or the client's last pan step lands off a tile boundary.
GEOM=512x384

# Xvnc holds the framebuffer; whatever we start afterwards just draws into it.
Xvnc :1 -geometry "$GEOM" -depth 24 \
        -SecurityTypes None -rfbport 5900 -AlwaysShared \
        -desktop snos -localhost=0 &
sleep 2

case "$SNOS_APP" in
  chromium)
    # --window-size matches the framebuffer so there is no scrollbar and no
    # scaling. Kiosk so there is no browser chrome eating the picture.
    # --disable-smooth-scrolling: UNMEASURED, and deliberately labelled as such.
    #
    # What IS measured: one J/K wheel click repaints 2.17 screenfuls (~1670
    # tiles, ~40 s), while a pan moves exactly the 96 tiles its exposed strip
    # contains and the wire runs a flat ~18 s per screenful in both cases. So the
    # client and copy_rect are behaving correctly and something is generating far
    # more damage than a scroll needs.
    #
    # Smooth scrolling is the obvious candidate -- one click becomes an
    # animation, and an RFB client faithfully fetches every intermediate
    # position. There is no point animating sixty frames of motion down a link
    # that needs eighteen seconds a screenful, and anything else that animates on
    # this desktop will cost the same way.
    #
    # But the follow-up measurement was never obtained: three attempts at a
    # scroll benchmark died in the harness, and the failure reproduces with an
    # UNBOUND key, so it is the benchmark and not the app. If a J step is still
    # ~40 s with this flag set, the animation theory is wrong and the next
    # suspect is the server sending the whole window rather than the damaged
    # region.
    exec chromium --no-sandbox --disable-gpu --disable-dev-shm-usage \
         --disable-smooth-scrolling \
         --window-position=0,0 --window-size="$(echo "$GEOM" | tr x ,)" \
         --kiosk --disable-infobars --no-first-run \
         "${SNOS_URL:-https://news.ycombinator.com}"
    ;;
  *)
    xsetroot -solid steelblue
    exec xterm -geometry 64x24 -fa DejaVuSansMono -fs 6 -bg black -fg green
    ;;
esac
