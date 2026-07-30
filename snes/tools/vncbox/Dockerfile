# A VNC server sized for a Super Nintendo to look INTO.
#
# The console's Mode 3 picture is 256x192 — its screen minus the 32 scanlines
# the cursor sprite costs, because a full-height 8bpp image leaves no
# 16 KB-aligned home for the sprite tile base. The desktop here is deliberately
# larger than that (see start.sh) and the client pans a 256x192 window over it
# with WASD, one server pixel per console pixel and nothing scaled. Xvnc rather
# than Xvfb + x11vnc because it is the X server and the VNC server in one
# process, with no display plumbing in between.
#
# -SecurityTypes None is the unauthenticated path: a two-byte handshake on the
# console's side, which is all a 65816 should be asked to do. This container is
# a toy on a private network and should never be exposed.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      tigervnc-standalone-server \
      x11-apps xterm x11-utils \
      chromium \
      fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*

ENV DISPLAY=:1
EXPOSE 5900

COPY start.sh /start.sh
RUN chmod +x /start.sh
CMD ["/start.sh"]
