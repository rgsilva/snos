"""A one-client IRC server: just what apps/irc/irc.c needs, nothing more."""
from __future__ import annotations

import asyncio
import logging
import re
from typing import Awaitable, Callable, Optional

log = logging.getLogger("discordbridge")

NICK_RE = re.compile(r"^[A-Za-z_\[\]\\`^{|}][A-Za-z0-9_\-\[\]\\`^{|}]{0,19}$")
PRINTABLE = re.compile(r"^[\x20-\x7e]*$")
MAX_WIRE = 510
RX_CAP = 4096
CHAN_RE = re.compile(r"^[#&][\x21-\x2b\x2d-\x7e]{1,23}$")  # irc.c tab names: 24 chars


def valid_channel(name: str) -> bool:
    return bool(CHAN_RE.match(name))


class Conn:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, addr: str):
        self.reader, self.writer, self.addr = reader, writer, addr
        self.nick: Optional[str] = None
        self.user: Optional[str] = None
        self.registered = False
        self.joined = False
        self.closed = False


class IrcServer:
    def __init__(
        self,
        channel: str,
        *,
        server_name: str = "discordbridge.local",
        topic: str = "Discord bridge",
        fake_nick: str = "discord",
        on_message: Callable[[str, str, bool], None] = lambda nick, text, action: None,
        on_nick: Callable[[str], None] = lambda nick: None,
        on_ready: Callable[[bool, str], None] = lambda ready, nick: None,
        on_connection: Callable[[bool, str], None] = lambda up, addr: None,
        idle_ping: float = 90.0,
        idle_dead: float = 60.0,
    ):
        self.channel = channel
        self.server_name = server_name
        self.topic = topic
        self.fake_nick = fake_nick
        self.on_message, self.on_nick = on_message, on_nick
        self.on_ready, self.on_connection = on_ready, on_connection
        self.idle_ping, self.idle_dead = idle_ping, idle_dead
        self.conn: Optional[Conn] = None
        self._server: Optional[asyncio.Server] = None

    # -------------------------------------------------------------- lifecycle

    async def start(self, host: str, port: int) -> int:
        self._server = await asyncio.start_server(self._handle, host, port)
        return self._server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        if self.conn:
            self._drop(self.conn, "server shutting down")
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    @property
    def ready(self) -> bool:
        c = self.conn
        return bool(c and c.registered and c.joined and not c.closed)

    @property
    def nick(self) -> Optional[str]:
        return self.conn.nick if self.conn else None

    # ------------------------------------------------------------------ write

    def _write(self, c: Conn, line: str) -> None:
        """The only write path to the console: printable ASCII, CRLF, bounded."""
        if not PRINTABLE.match(line):
            log.error("non-ASCII line blocked on the wire: %r", line[:80])
            line = re.sub(r"[^\x20-\x7e]", "?", line)
        if c.closed:
            return
        try:
            c.writer.write(line[:MAX_WIRE].encode("ascii") + b"\r\n")
        except Exception as e:  # transport already gone
            log.info("write failed: %s", e)

    def _srv(self, c: Conn, code: str, rest: str) -> None:
        self._write(c, f":{self.server_name} {code} {c.nick or '*'} {rest}")

    def send_chat(self, nick: str, line: str) -> bool:
        """Write a ready-made line to the console if it is in the channel."""
        c = self.conn
        if not (c and self.ready):
            return False
        self._write(c, line)
        return True

    def _drop(self, c: Conn, why: str) -> None:
        if c.closed:
            return
        self._write(c, f"ERROR :Closing Link: {why}")
        c.closed = True
        try:
            c.writer.close()
        except Exception:
            pass
        if self.conn is c:
            self.conn = None
            self.on_ready(False, c.nick or "")
            self.on_connection(False, c.addr)

    # ------------------------------------------------------------------ read

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        addr = f"{peer[0]}:{peer[1]}" if peer else "?"
        c = Conn(reader, writer, addr)
        old = self.conn
        if old:
            log.info("console %s replaces %s", addr, old.addr)
            self._drop(old, "replaced by a new connection")
        self.conn = c
        self.on_connection(True, addr)
        buf = b""
        skipping = False
        pinged = False
        try:
            while not c.closed:
                wait = self.idle_dead if pinged else self.idle_ping
                try:
                    data = await asyncio.wait_for(reader.read(2048), wait)
                except asyncio.TimeoutError:
                    if pinged:
                        log.info("console %s timed out", addr)
                        self._drop(c, "ping timeout")
                        break
                    pinged = True
                    self._write(c, f"PING :{self.server_name}")
                    continue
                if not data:
                    break
                pinged = False
                buf += data
                while True:
                    i = buf.find(b"\n")
                    if i < 0:
                        break
                    raw, buf = buf[:i], buf[i + 1:]
                    if skipping:
                        skipping = False
                        continue
                    self._line(c, raw.rstrip(b"\r").decode("latin-1")[:512])
                    if c.closed:
                        break
                if len(buf) > RX_CAP:  # over-long line: keep nothing until its newline
                    buf, skipping = b"", True
        except (ConnectionError, OSError) as e:
            log.info("console %s read error: %s", addr, e)
        finally:
            if not c.closed:
                if self.conn is c:
                    log.info("console %s vanished", addr)
                self._drop(c, "connection closed")

    # --------------------------------------------------------------- protocol

    def _line(self, c: Conn, line: str) -> None:
        line = line.strip(" ")
        if not line:
            return
        if line.startswith(":"):
            line = line.split(" ", 1)[1].lstrip(" ") if " " in line else ""
            if not line:
                return
        head, _, rest = line.partition(" ")
        cmd = head.upper()
        params = self._params(rest)
        fn = getattr(self, "_c_" + cmd, None) if cmd.isalpha() else None
        if fn is None:
            if c.registered:
                self._srv(c, "421", f"{cmd[:30]} :Unknown command")
            return
        if not c.registered and cmd not in ("NICK", "USER", "PASS", "CAP", "PING", "PONG", "QUIT"):
            self._srv(c, "451", ":You have not registered")
            return
        fn(c, params)

    @staticmethod
    def _params(rest: str) -> list[str]:
        out: list[str] = []
        rest = rest.lstrip(" ")
        while rest:
            if rest.startswith(":"):
                out.append(rest[1:])
                break
            p, _, rest = rest.partition(" ")
            out.append(p)
            rest = rest.lstrip(" ")
        return out

    def _taken(self, nick: str) -> bool:
        return nick.lower() == self.fake_nick.lower()

    def _c_PASS(self, c: Conn, p: list[str]) -> None:
        pass

    def _c_CAP(self, c: Conn, p: list[str]) -> None:
        sub = p[0].upper() if p else ""
        if sub == "LS":
            self._write(c, f":{self.server_name} CAP * LS :")
        elif sub == "REQ":
            self._write(c, f":{self.server_name} CAP * NAK :{p[1] if len(p) > 1 else ''}")

    def _c_NICK(self, c: Conn, p: list[str]) -> None:
        if not p or not p[0]:
            self._srv(c, "431", ":No nickname given")
            return
        new = p[0]
        if not NICK_RE.match(new):
            self._srv(c, "432", f"{new[:20] if PRINTABLE.match(new) else '?'} :Erroneous nickname")
            return
        if self._taken(new):
            self._srv(c, "433", f"{new} :Nickname is already in use")
            return
        if c.registered:
            old = c.nick
            self._write(c, f":{old}!{c.user}@console NICK :{new}")
            c.nick = new
            self.on_nick(new)
            return
        c.nick = new
        self._maybe_register(c)

    def _c_USER(self, c: Conn, p: list[str]) -> None:
        if not p:
            self._srv(c, "461", "USER :Not enough parameters")
            return
        if c.registered:
            self._srv(c, "462", ":You may not reregister")
            return
        c.user = (re.sub(r"[^A-Za-z0-9_]", "", p[0])[:10]) or "snes"
        self._maybe_register(c)

    def _maybe_register(self, c: Conn) -> None:
        if c.registered or not c.nick or not c.user:
            return
        c.registered = True
        self._srv(c, "001", f":Welcome to the Discord bridge, {c.nick}")
        self._srv(c, "375", f":- {self.server_name} -")
        self._srv(c, "372", f":- bridging {self.channel} to Discord")
        self._srv(c, "376", ":End of MOTD command")
        self.on_nick(c.nick)

    def _c_PING(self, c: Conn, p: list[str]) -> None:
        if not p:
            self._write(c, f":{self.server_name} 409 {c.nick or '*'} :No origin specified")
            return
        self._write(c, f":{self.server_name} PONG {self.server_name} :{p[0]}")

    def _c_PONG(self, c: Conn, p: list[str]) -> None:
        pass

    def _c_QUIT(self, c: Conn, p: list[str]) -> None:
        self._drop(c, "Quit")

    def _is_chan(self, name: str) -> bool:
        return name.lower() == self.channel.lower()

    def _names(self, c: Conn) -> None:
        self._srv(c, "353", f"= {self.channel} :{c.nick} {self.fake_nick}")
        self._srv(c, "366", f"{self.channel} :End of NAMES list")

    def _c_JOIN(self, c: Conn, p: list[str]) -> None:
        if not p:
            self._srv(c, "461", "JOIN :Not enough parameters")
            return
        for name in p[0].split(","):
            if not name or name == "0":
                continue
            if not self._is_chan(name):
                shown = name[:24] if PRINTABLE.match(name) else "?"
                self._srv(c, "403", f"{shown} :No such channel")
                continue
            if c.joined:
                continue
            c.joined = True
            self._write(c, f":{c.nick}!{c.user}@console JOIN {self.channel}")
            if self.topic:
                self._srv(c, "332", f"{self.channel} :{self.topic}")
            self._names(c)
            log.info("console %s (%s) joined %s", c.addr, c.nick, self.channel)
            self.on_ready(True, c.nick or "")

    def _c_PART(self, c: Conn, p: list[str]) -> None:
        if not p:
            self._srv(c, "461", "PART :Not enough parameters")
            return
        for name in p[0].split(","):
            if not self._is_chan(name):
                shown = name[:24] if PRINTABLE.match(name) else "?"
                self._srv(c, "403", f"{shown} :No such channel")
            elif not c.joined:
                self._srv(c, "442", f"{self.channel} :You're not on that channel")
            else:
                c.joined = False
                self._write(c, f":{c.nick}!{c.user}@console PART {self.channel}")
                self.on_ready(False, c.nick or "")

    def _c_NAMES(self, c: Conn, p: list[str]) -> None:
        if c.joined and (not p or self._is_chan(p[0].split(",")[0])):
            self._names(c)
        else:
            self._srv(c, "366", "* :End of NAMES list")

    def _c_TOPIC(self, c: Conn, p: list[str]) -> None:
        if not p or not self._is_chan(p[0]):
            self._srv(c, "403", f"{(p[0][:24] if p and PRINTABLE.match(p[0]) else '?')} :No such channel")
        elif len(p) > 1:
            self._srv(c, "482", f"{self.channel} :You're not channel operator")
        else:
            self._srv(c, "332", f"{self.channel} :{self.topic}")

    def _c_MODE(self, c: Conn, p: list[str]) -> None:
        if len(p) == 1 and self._is_chan(p[0]):
            self._srv(c, "324", f"{self.channel} +nt")
        elif len(p) == 2 and self._is_chan(p[0]) and p[1] in ("b", "+b"):
            self._srv(c, "368", f"{self.channel} :End of channel ban list")
        elif len(p) == 1 and c.nick and p[0].lower() == c.nick.lower():
            self._srv(c, "221", "+")

    def _c_WHO(self, c: Conn, p: list[str]) -> None:
        self._srv(c, "315", f"{(p[0][:24] if p and PRINTABLE.match(p[0]) else '*')} :End of WHO list")

    def _c_WHOIS(self, c: Conn, p: list[str]) -> None:
        self._srv(c, "318", f"{(p[-1][:20] if p and PRINTABLE.match(p[-1]) else '*')} :End of WHOIS list")

    def _c_USERHOST(self, c: Conn, p: list[str]) -> None:
        self._srv(c, "302", ":")

    def _c_ISON(self, c: Conn, p: list[str]) -> None:
        self._srv(c, "303", ":")

    def _c_LIST(self, c: Conn, p: list[str]) -> None:
        self._srv(c, "323", ":End of LIST")

    def _c_MOTD(self, c: Conn, p: list[str]) -> None:
        self._srv(c, "422", ":MOTD File is missing")

    def _c_AWAY(self, c: Conn, p: list[str]) -> None:
        pass

    def _c_NOTICE(self, c: Conn, p: list[str]) -> None:
        pass  # never answered, as RFC 1459 asks

    def _c_PRIVMSG(self, c: Conn, p: list[str]) -> None:
        if not p:
            self._srv(c, "411", ":No recipient given (PRIVMSG)")
            return
        if len(p) < 2 or not p[1]:
            self._srv(c, "412", ":No text to send")
            return
        target, text = p[0], p[1]
        if not self._is_chan(target):
            self._write(c, f":{self.server_name} NOTICE {c.nick} :private messages are not bridged")
            return
        if not c.joined:
            self._srv(c, "404", f"{self.channel} :Cannot send to channel")
            return
        action = False
        if text.startswith("\x01"):
            body = text.strip("\x01")
            if body.upper().startswith("ACTION"):
                action, text = True, body[6:].strip()
            else:
                return  # other CTCP is not bridged
        if text:
            self.on_message(c.nick or "", text, action)
