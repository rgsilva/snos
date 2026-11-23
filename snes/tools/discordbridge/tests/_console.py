"""A scripted irc.c for the tests."""
import asyncio
import re


class Console:
    """Scripted irc.c. Each helper names the lines of irc.c it mirrors."""

    def __init__(self, port):
        self.port = port
        self.buf = b""

    async def connect(self):
        self.r, self.w = await asyncio.open_connection("127.0.0.1", self.port)
        return self

    def send(self, s):
        self.w.write(s.encode("latin-1"))

    async def register(self, nick="snes", order="nick-first", split=False):
        # irc.c:1235-1236: sendv("NICK ", nick) then sendv("USER ", nick, " 0 * :SNOS IRC"),
        # each through sendv() (irc.c:554-562) which appends "\r\n".
        nick_l, user_l = f"NICK {nick}\r\n", f"USER {nick} 0 * :SNOS IRC\r\n"
        data = (nick_l + user_l) if order == "nick-first" else (user_l + nick_l)
        if split:
            for i in range(0, len(data), 5):
                self.send(data[i:i + 5])
                await self.w.drain()
                await asyncio.sleep(0.005)
        else:
            self.send(data)
        lines = await self.until(lambda l: " 376 " in l)
        assert " 001 " in lines[0], lines
        return lines

    async def join(self, chan="#palestra"):
        # irc.c:596, on 001: sendv("JOIN ", autojoin); /join sends the same
        self.send(f"JOIN {chan}\r\n")
        return await self.until(lambda l: " 366 " in l)

    async def line(self, timeout=2.0):
        while b"\n" not in self.buf:
            d = await asyncio.wait_for(self.r.read(1024), timeout)
            if not d:
                raise EOFError
            self.buf += d
        raw, self.buf = self.buf.split(b"\n", 1)
        assert raw.endswith(b"\r"), raw
        raw = raw[:-1]
        assert re.fullmatch(rb"[\x20-\x7e]*", raw), raw
        assert len(raw) <= 510
        return raw.decode("ascii")

    async def until(self, pred, timeout=2.0):
        seen = []
        while True:
            l = await self.line(timeout)
            seen.append(l)
            if pred(l):
                return seen

    async def close(self):
        self.w.close()
