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
    # --disable-smooth-scrolling: MEASURED 2026-07-30, and it helps -- but it is
    # not the whole story, so read the second half of this note before trusting
    # a scroll to be cheap.
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
    # With the flag set, one J step measures 900 frames / 15.00 s / 1200 polls,
    # against ~40 s and 2.17 screenfuls without it. So the animation theory was
    # right and this removes about 60% of the cost.
    #
    # WHAT IS LEFT IS THE REAL CEILING: a full 48 KB screen is 950 frames / 1189
    # polls, so a single scroll step still costs 101% OF A COMPLETE REPAINT.
    # CopyRect is not saving anything, because there is nothing to copy -- a
    # GPU-less Chromium repaints the whole viewport on scroll rather than
    # shifting it, so the server has a full window of genuine damage to send.
    # Scrolling is therefore never going to be cheaper than loading a page here,
    # and no amount of client-side cleverness changes that. Panning, which the
    # console does itself, stays ~7x cheaper and is the better way to move around.
    #
    # (Three earlier attempts at this benchmark died and looked like a crash
    # after the keypress. They were not: they tested for the link "going quiet"
    # using rx_frames and hostframes, which never stop moving because a frame is
    # exchanged every poll whether or not it carries data. Stability has to be
    # measured on the PICTURE -- see vncscroll2.lua in the scratchpad.)
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
