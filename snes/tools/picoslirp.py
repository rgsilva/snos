#!/usr/bin/env python3
"""TCP relay for the Pico W SNOS adapter; no third-party dependencies."""
import argparse
import errno
import os
import select
import socket
import struct
import threading
import time
import traceback

import slirp


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        s.close()


def message(typ, payload):
    return struct.pack("<BH", typ, len(payload)) + payload


def session(conn, stop, debug=False):
    incoming, outgoing = bytearray(), bytearray()

    def emit(pkt):
        if debug:
            print(f"IP->pico {len(pkt)}B {pkt.hex()}")
        outgoing.extend(message(1, pkt))

    nat = slirp.Slirp(output=emit, log=print, link_busy=lambda: bool(outgoing))
    outgoing.extend(message(5, socket.inet_aton(local_ip())))
    conn.setblocking(False)
    stats_at = time.monotonic() + 5
    try:
        while not stop.is_set():
            readable, writable, _ = select.select([conn], [conn] if outgoing else [], [], .004)
            if readable:
                data = conn.recv(4096)
                if not data:
                    return
                incoming.extend(data)
            while len(incoming) >= 3:
                typ, length = struct.unpack_from("<BH", incoming)
                if length > slirp.MTU:
                    raise ValueError(f"oversized relay message: {length}")
                if len(incoming) < 3 + length:
                    break
                payload = bytes(incoming[3:3 + length])
                del incoming[:3 + length]
                if typ == 1:
                    if debug:
                        print(f"pico->IP {length}B {payload.hex()}")
                    try:
                        nat.input(payload)
                    except Exception:
                        traceback.print_exc()
                elif typ in (2, 3) and length == 3:
                    port, ch = struct.unpack("<HB", payload)
                    if typ == 2:
                        if nat.forward_add(port, ch):
                            print(f"picoslirp: forwarding port {port} to the console's stack")
                        else:
                            outgoing.extend(message(4, payload))
                    else:
                        nat.forward_del(port, ch)
            try:
                nat.tick()
            except Exception:
                traceback.print_exc()
            if writable and outgoing:
                try:
                    n = conn.send(outgoing)
                    del outgoing[:n]
                except (BlockingIOError, InterruptedError):
                    pass
            if len(outgoing) > 1024 * 1024:
                raise ValueError("Pico stopped draining relay output")
            if os.environ.get("SNOS_GW_STATS") and time.monotonic() >= stats_at:
                print(nat.stats_line())
                stats_at = time.monotonic() + 5
    finally:
        nat.close()


def serve(listener, stop, debug=False):
    listener.settimeout(.1)
    while not stop.is_set():
        try:
            conn, addr = listener.accept()
        except socket.timeout:
            continue
        print(f"picoslirp: Pico connected from {addr[0]}:{addr[1]}")
        with conn:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                session(conn, stop, debug)
            except (OSError, ValueError) as exc:
                print(f"picoslirp: {exc}")
        print("picoslirp: Pico disconnected")


def listen(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((host, port))
    s.listen(1)
    return s


def selftest():
    stop = threading.Event()
    with listen("127.0.0.1", 0) as srv:
        thread = threading.Thread(target=serve, args=(srv, stop), daemon=True)
        thread.start()
        try:
            with socket.create_connection(srv.getsockname(), timeout=2) as pico:
                def receive():
                    def exact(n):
                        data = bytearray()
                        while len(data) < n:
                            part = pico.recv(n - len(data))
                            if not part:
                                raise AssertionError("unexpected EOF")
                            data.extend(part)
                        return bytes(data)
                    typ, length = struct.unpack("<BH", exact(3))
                    return typ, exact(length)

                typ, address = receive()
                assert typ == 5 and len(address) == 4
                with listen("0.0.0.0", 0) as probe:
                    port = probe.getsockname()[1]
                payload = struct.pack("<HB", port, 7)
                request = message(2, payload)
                pico.sendall(request[:2])  # Split the relay header across reads.
                pico.sendall(request[2:])
                deadline = time.monotonic() + 2
                while True:
                    with socket.socket() as probe:
                        try:
                            probe.bind(("0.0.0.0", port))
                        except OSError as exc:
                            assert exc.errno == errno.EADDRINUSE
                            break
                    assert time.monotonic() < deadline, "forward was not bound"
                    time.sleep(.01)
                # A failed bind returns the original port/channel payload.
                with listen("0.0.0.0", 0) as occupied:
                    failure = struct.pack("<HB", occupied.getsockname()[1], 8)
                    pico.sendall(message(2, failure))
                    assert receive() == (4, failure)
                pico.sendall(message(3, payload))
                deadline = time.monotonic() + 2
                while True:
                    with socket.socket() as probe:
                        try:
                            probe.bind(("0.0.0.0", port))
                            break
                        except OSError as exc:
                            assert exc.errno == errno.EADDRINUSE
                    assert time.monotonic() < deadline, "forward was not released"
                    time.sleep(.01)
        finally:
            stop.set()
            thread.join(3)
            assert not thread.is_alive(), "server did not stop"
    print("picoslirp: selftest passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--debug", action="store_true", help="hex-dump IP datagrams")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    stop = threading.Event()
    with listen("0.0.0.0", 3210) as srv:
        print(f"picoslirp: listening on 0.0.0.0:3210, LAN address {local_ip()}")
        try:
            serve(srv, stop, args.debug)
        except KeyboardInterrupt:
            stop.set()
            print("\npicoslirp: stopped")


if __name__ == "__main__":
    main()
