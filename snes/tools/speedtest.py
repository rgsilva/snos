#!/usr/bin/env python3
r"""An ordinary TCP peer for `net speedtest`, independent of the gateway.

    python3 snes/tools/speedtest.py [--bind 0.0.0.0] [--port 7778]

Each connection starts with `D frames\n` or `U frames\n` (60 Hz, 60..7200).
The server answers `OK\n`, then waits for `GO\n`. D sends raw bytes until
closed; U counts raw bytes for frames/60 seconds after GO, replies with its
decimal count and a newline, and waits for the client to close. Two sockets
run D and U concurrently. No framing, counts or application protocol belong
in the NAT.

The receiver owns the clock: download is timed on the console, upload here.
Only bytes received inside that window count, never bytes sitting in a send
queue at the deadline. The two clocks need no synchronization; their windows
are offset by GO's transit time. Setup and the upload report are untimed.
"""
import argparse
import select
import socket
import socketserver
import time

PORT = 7778
TIMEOUT = 15
PAYLOAD = b"x" * 4096


def read_line(sock):
    """Bound both the line length and the whole handshake, not each byte."""
    end = time.monotonic() + TIMEOUT
    line = bytearray()
    while len(line) < 24:
        left = end - time.monotonic()
        if left <= 0 or not select.select([sock], [], [], left)[0]:
            raise TimeoutError("handshake timed out")
        c = sock.recv(1)
        if not c:
            raise EOFError("peer closed during handshake")
        if c == b"\n":
            return bytes(line)
        line.extend(c)
    raise ValueError("control line too long")


def serve(sock):
    sock.settimeout(TIMEOUT)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    words = read_line(sock).split(b" ")
    if (len(words) != 2 or words[0] not in (b"D", b"U") or
            not words[1].isdigit()):
        raise ValueError("expected D frames or U frames")
    frames = int(words[1])
    if not 60 <= frames <= 7200:
        raise ValueError("duration must be 60..7200 frames")
    sock.sendall(b"OK\n")
    if read_line(sock) != b"GO":
        raise ValueError("expected GO")
    end = time.monotonic() + frames / 60
    count = 0
    if words[0] == b"U":
        while True:
            left = end - time.monotonic()
            if left <= 0 or not select.select([sock], [], [], left)[0]:
                break
            data = sock.recv(4096)
            if not data:
                raise EOFError("upload closed before its deadline")
            count += len(data)
        sock.sendall(f"{count}\n".encode("ascii"))
        # Ignore the tail already in flight when the receive window ended.
        end = time.monotonic() + TIMEOUT
        while True:
            left = end - time.monotonic()
            if left <= 0 or not select.select([sock], [], [], left)[0]:
                break
            try:
                if not sock.recv(4096):
                    break
            except ConnectionResetError:
                break
        return "upload", count

    # A nonblocking send keeps a full socket buffer from hiding the client's
    # close. Downloads stop by close, so queued tail never delays the next phase.
    sock.setblocking(False)
    end += TIMEOUT
    while time.monotonic() < end:
        rd, wr, _ = select.select([sock], [sock], [], 1)
        if rd:
            try:
                if not sock.recv(4096):
                    break
            except ConnectionResetError:
                break
            raise ValueError("unexpected download control data")
        if wr:
            try:
                count += sock.send(PAYLOAD)
            except BlockingIOError:
                pass
            except (BrokenPipeError, ConnectionResetError):
                break
    return "download queued", count


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            direction, count = serve(self.request)
            print(f"{self.client_address[0]}: {direction} {count} bytes", flush=True)
        except (OSError, EOFError, ValueError) as exc:
            print(f"{self.client_address[0]}: {exc}", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()
    with Server((args.bind, args.port), Handler) as server:
        print(f"speedtest listening on {args.bind}:{args.port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
