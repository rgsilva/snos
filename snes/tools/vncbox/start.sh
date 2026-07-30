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
    exec chromium --no-sandbox --disable-gpu --disable-dev-shm-usage \
         --window-position=0,0 --window-size="$(echo "$GEOM" | tr x ,)" \
         --kiosk --disable-infobars --no-first-run \
         "${SNOS_URL:-https://news.ycombinator.com}"
    ;;
  *)
    xsetroot -solid steelblue
    exec xterm -geometry 64x24 -fa DejaVuSansMono -fs 6 -bg black -fg green
    ;;
esac
