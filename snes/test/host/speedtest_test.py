#!/usr/bin/env python3
"""The speedtest server over loopback TCP, including a tail past the deadline."""
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import speedtest


def line(sock):
    out = b""
    while not out.endswith(b"\n"):
        part = sock.recv(1)
        assert part, "peer closed before its reply"
        out += part
    return out


def peer(listener, results):
    sock, _ = listener.accept()
    with sock:
        try:
            results.append(speedtest.serve(sock))
        except (OSError, EOFError, ValueError) as exc:
            results.append(exc)


def connect(command):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    results = []
    worker = threading.Thread(target=peer, args=(listener, results), daemon=True)
    worker.start()
    client = socket.create_connection(listener.getsockname(), timeout=3)
    client.sendall(command)
    return client, listener, worker, results


def finish(session):
    client, listener, worker, results = session
    client.close()
    worker.join(3)
    listener.close()
    assert not worker.is_alive(), "server did not stop after client close"
    assert len(results) == 1
    return results[0]


def main():
    # Separate simultaneous sockets exercise the threaded-server use case;
    # a fixed upload gives an independent count, with a deliberately late tail.
    up = connect(b"U 60\n")
    down = connect(b"D 60\n")
    assert line(up[0]) == line(down[0]) == b"OK\n"
    start = time.monotonic()
    up[0].sendall(b"GO\nx" + b"x" * 1233)
    down[0].sendall(b"GO\n")
    received = b""
    while len(received) < 8192:
        received += down[0].recv(4096)
    assert received == b"x" * len(received)
    assert line(up[0]) == b"1234\n"
    elapsed = time.monotonic() - start
    assert 0.9 <= elapsed < 2.5, elapsed
    up[0].sendall(b"late bytes are not in the count")
    assert finish(up) == ("upload", 1234)
    # An unread download tail produces RST; that is a normal end here.
    assert finish(down)[0] == "download queued"

    # The server keeps the same wall-clock window when no payload arrives.
    empty = connect(b"U 60\n")
    assert line(empty[0]) == b"OK\n"
    empty[0].sendall(b"GO\n")
    assert line(empty[0]) == b"0\n"
    assert finish(empty) == ("upload", 0)

    for command in (b"U 0\n", b"D 7201\n", b"U -60\n", b"X 60\n", b"x" * 24):
        session = connect(command)
        assert session[0].recv(1) == b""
        assert isinstance(finish(session), ValueError)

    speedtest.TIMEOUT = 0.1
    session = connect(b"U ")
    assert session[0].recv(1) == b""
    assert isinstance(finish(session), TimeoutError)
    print("RESULT PASS speedtest-host")


if __name__ == "__main__":
    main()
