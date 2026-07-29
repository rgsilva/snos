#!/bin/sh
# Xvnc holds the framebuffer; whatever we start afterwards just draws into it.
Xvnc :1 -geometry 256x192 -depth 24 \
        -SecurityTypes None -rfbport 5900 -AlwaysShared \
        -desktop snos -localhost=0 &
sleep 2

case "$SNOS_APP" in
  chromium)
    # --window-size matches the framebuffer so there is no scrollbar and no
    # scaling. Kiosk so there is no browser chrome eating a 224-line screen.
    exec chromium --no-sandbox --disable-gpu --disable-dev-shm-usage \
         --window-position=0,0 --window-size=256,192 \
         --kiosk --disable-infobars --no-first-run \
         "${SNOS_URL:-https://news.ycombinator.com}"
    ;;
  *)
    xsetroot -solid steelblue
    exec xterm -geometry 32x12 -fa DejaVuSansMono -fs 6 -bg black -fg green
    ;;
esac
