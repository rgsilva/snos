"""End to end on localhost. The client side sends exactly what apps/irc/irc.c sends."""
import _setup  # noqa
import asyncio
import os
import re
import tempfile
import unittest

from core import DiscordMsg
from discordbridge import Bridge, Settings

CHAN = "#palestra"


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

    async def join(self):
        # irc.c:596, on 001: sendv("JOIN ", autojoin)
        self.send(f"JOIN {CHAN}\r\n")
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


class E2E(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.mute = os.path.join(self.tmp.name, "MUTE")
        self.posts = []

        async def post(t):
            self.posts.append(t)

        s = Settings(channel_id=7, irc_channel=CHAN, bind="127.0.0.1", port=0, interval=0.05,
                     mute_file=self.mute, blocklist=os.path.join(self.tmp.name, "none.txt"))
        self.bridge = Bridge(s, post)
        self.port = await self.bridge.start()
        self.bridge.ircd.idle_ping, self.bridge.ircd.idle_dead = 30, 30
        self.consoles = []

    async def asyncTearDown(self):
        for c in self.consoles:
            await c.close()
        await self.bridge.stop()
        self.tmp.cleanup()

    async def console(self, **kw):
        c = await Console(self.port).connect()
        self.consoles.append(c)
        if kw.pop("register", True):
            await c.register(**kw)
        return c

    async def joined(self, **kw):
        c = await self.console(**kw)
        await c.join()
        return c

    def discord(self, text, name="ana", **kw):
        self.bridge.on_discord_message(DiscordMsg(7, name, text, **kw))

    async def settle(self, t=0.15):
        await asyncio.sleep(t)

    # ------------------------------------------------------------ registration

    async def test_registration_both_orders(self):
        for order in ("nick-first", "user-first"):
            c = await self.console(order=order)
            self.assertEqual(self.bridge.ircd.nick, "snes")
            await c.close()
            await self.settle()

    async def test_split_writes_and_lf_only(self):
        c = await self.console(split=True)
        c.send("PING :snos\n")  # bare LF is tolerated
        l = await c.until(lambda l: " PONG " in l)
        self.assertTrue(l[-1].endswith(":snos"))

    async def test_join_sequence(self):
        c = await self.console()
        c.send(f"JOIN {CHAN}\r\n")
        lines = await c.until(lambda l: " 366 " in l)
        # irc.c:736-754 needs the echo with our own nick as prefix, then 353 (names_reply, :566) and 366
        self.assertEqual(lines[0], f":snes!snes@console JOIN {CHAN}")
        self.assertTrue(lines[1].startswith(":discordbridge.local 332 snes #palestra :"))
        self.assertEqual(lines[2], ":discordbridge.local 353 snes = #palestra :snes discord")
        self.assertEqual(lines[3], ":discordbridge.local 366 snes #palestra :End of NAMES list")

    async def test_join_other_channel_refused(self):
        c = await self.console()
        c.send("JOIN #other\r\n")
        l = await c.until(lambda l: " 403 " in l)
        self.assertIn("#other :No such channel", l[-1])
        self.assertFalse(self.bridge.ircd.ready)

    async def test_join_before_registration(self):
        c = await self.console(register=False)
        c.send("JOIN #palestra\r\n")
        l = await c.line()
        self.assertIn(" 451 ", l)

    async def test_nick_taken_then_retry(self):
        # irc.c:599-611: on 432/433 it appends '_' and sends NICK again
        c = await self.console(register=False)
        c.send("NICK discord\r\nUSER discord 0 * :SNOS IRC\r\n")
        l = await c.until(lambda l: " 433 " in l)
        c.send("NICK discord_\r\n")
        await c.until(lambda l: " 001 " in l)
        c.send("NICK 9bad\r\n")
        await c.until(lambda l: " 432 " in l)

    # --------------------------------------------------------------- keepalive

    async def test_keepalive_ping(self):
        c = await self.joined()
        c.send("PING :snos\r\n")  # irc.c:1278
        l = await c.until(lambda l: " PONG " in l)
        self.assertEqual(l[-1], ":discordbridge.local PONG discordbridge.local :snos")
        c.send("PONG :x\r\n")

    async def test_dead_socket_timeout(self):
        self.bridge.ircd.idle_ping, self.bridge.ircd.idle_dead = 0.1, 0.1
        c = await self.joined()
        l = await c.until(lambda l: l.startswith("PING"), 1)
        l = await c.until(lambda l: l.startswith("ERROR"), 1)
        await self.settle()
        self.assertIsNone(self.bridge.ircd.conn)
        self.assertFalse(self.bridge.relay.console_ready)

    async def test_pong_keeps_alive(self):
        self.bridge.ircd.idle_ping, self.bridge.ircd.idle_dead = 0.1, 0.15
        c = await self.joined()
        for _ in range(3):
            await c.until(lambda l: l.startswith("PING"), 1)
            c.send("PONG :discordbridge.local\r\n")
        self.assertTrue(self.bridge.ircd.ready)

    # ------------------------------------------------------------ console -> Discord

    async def test_nick_change_privmsg_action(self):
        c = await self.joined()
        c.send("PRIVMSG #palestra :ola palco\r\n")  # irc.c:880 say_to_current
        await self.settle()
        c.send("NICK ric\r\n")  # irc.c:919 /nick
        l = await c.until(lambda l: " NICK " in l)
        self.assertEqual(l[-1], ":snes!snes@console NICK :ric")
        c.send("PRIVMSG #palestra :@everyone oi <@123>\r\n")
        c.send("PRIVMSG #palestra :\x01ACTION acena\x01\r\n")  # /raw; irc.c has no /me
        await self.settle()
        self.assertEqual(self.posts[0], "**[SNES]** ola palco")
        self.assertNotIn("@everyone", self.posts[1])
        self.assertNotIn("<@", self.posts[1])
        self.assertTrue(self.posts[1].startswith("**[SNES]** ric: "))
        self.assertEqual(self.posts[2], "**[SNES]** ric: *acena*")

    async def test_private_message_not_bridged(self):
        c = await self.joined()
        c.send("PRIVMSG bob :psst\r\n")  # irc.c:933 /msg
        l = await c.until(lambda l: " NOTICE " in l)
        self.assertEqual(l[-1], ":discordbridge.local NOTICE snes :private messages are not bridged")
        await self.settle()
        self.assertEqual(self.posts, [])

    async def test_housekeeping_commands(self):
        c = await self.joined()
        for cmd in ("NAMES #palestra", "MODE #palestra", "WHO #palestra", "USERHOST snes", "FOO bar",
                    "TOPIC #palestra", "LIST", "WHOIS snes", "MODE snes", "CAP LS"):
            c.send(cmd + "\r\n")
        c.send("PING :done\r\n")
        l = await c.until(lambda l: " PONG " in l)
        codes = " ".join(l)
        for code in (" 353 ", " 366 ", " 324 ", " 315 ", " 302 ", " 421 ", " 332 ", " 323 ", " 318 ", " 221 "):
            self.assertIn(code, codes)
        self.assertTrue(self.bridge.ircd.ready)

    async def test_garbage_and_long_lines(self):
        c = await self.joined()
        c.send("\x00\xff\xfe\r\n\r\n   \r\n:\r\n: \r\n")
        c.send("PRIVMSG #palestra :" + "x" * 20000 + "\r\n")
        c.send("PING :alive\r\n")
        await c.until(lambda l: " PONG " in l and l.endswith(":alive"))
        self.assertTrue(self.bridge.ircd.ready)

    async def test_part_stops_relay_and_quit_closes(self):
        c = await self.joined()
        c.send(f"PART {CHAN}\r\n")  # irc.c:912
        l = await c.until(lambda l: " PART " in l)
        self.assertEqual(l[-1], f":snes!snes@console PART {CHAN}")
        self.discord("depois do part")
        await self.settle()
        with self.assertRaises(asyncio.TimeoutError):
            await c.line(0.2)
        c.send("QUIT :SNOS\r\n")  # irc.c:871
        await c.until(lambda l: l.startswith("ERROR"))
        self.assertIsNone(self.bridge.ircd.conn)

    # ------------------------------------------------------------ Discord -> console

    async def test_discord_to_console_pure_ascii(self):
        c = await self.joined()
        self.discord("coração \U0001F600 de \u201caço\u201d\nlinha 2", name="João Ñ")
        self.discord("", name="bot", is_bot=True)
        self.discord("outro canal")
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertEqual(l[-1], ":JoaoN!d@discord PRIVMSG #palestra :coracao de \"aco\" / linha 2")

    async def test_rate_and_order(self):
        c = await self.joined()
        for i in range(4):
            self.discord(f"m{i}")
        got = []
        t0 = asyncio.get_running_loop().time()
        for _ in range(4):
            got.append((await c.until(lambda l: "PRIVMSG" in l))[-1].rsplit(":", 1)[1])
        self.assertEqual(got, ["m0", "m1", "m2", "m3"])
        self.assertGreaterEqual(asyncio.get_running_loop().time() - t0, 0.12)  # 3 gaps of 0.05

    async def test_no_console_drops_not_stores(self):
        self.discord("ninguem ouve")
        c = await self.joined()
        with self.assertRaises(asyncio.TimeoutError):
            await c.until(lambda l: "PRIVMSG" in l, 0.3)

    async def test_mute_both_ways(self):
        c = await self.joined()
        open(self.mute, "w").close()
        self.discord("mudo")
        c.send("PRIVMSG #palestra :tambem mudo\r\n")
        with self.assertRaises(asyncio.TimeoutError):
            await c.until(lambda l: "PRIVMSG" in l, 0.3)
        self.assertEqual(self.posts, [])
        os.remove(self.mute)
        self.discord("voltou")
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertTrue(l[-1].endswith(":voltou"))

    async def test_discord_failure_does_not_block_irc(self):
        async def boom(t):
            raise RuntimeError("429")
        self.bridge.post = boom
        c = await self.joined()
        for i in range(10):
            c.send(f"PRIVMSG #palestra :x{i}\r\n")
        c.send("PING :still\r\n")
        await c.until(lambda l: " PONG " in l)

    # ------------------------------------------------------------ connections

    async def test_new_connection_replaces_old(self):
        a = await self.joined()
        b = await self.joined()
        l = await a.until(lambda l: l.startswith("ERROR"))
        self.assertIn("replaced", l[-1])
        self.discord("para o novo")
        l = await b.until(lambda l: "PRIVMSG" in l)
        self.assertTrue(l[-1].endswith(":para o novo"))

    async def test_abrupt_close(self):
        c = await self.joined()
        c.w.transport.abort()
        await self.settle()
        self.assertIsNone(self.bridge.ircd.conn)
        self.assertFalse(self.bridge.relay.console_ready)
        d = await self.joined()
        self.discord("de volta")
        await d.until(lambda l: "PRIVMSG" in l)


class WireGuard(unittest.TestCase):
    def test_non_ascii_never_written(self):
        from ircd import IrcServer

        class W:
            out = b""
            def write(self, b): self.out += b
            def close(self): pass

        from ircd import Conn
        w = W()
        c = Conn(None, w, "x")
        IrcServer("#a")._write(c, "PRIVMSG #a :caf\u00e9 \x03")
        self.assertEqual(w.out, b"PRIVMSG #a :caf? ?\r\n")


if __name__ == "__main__":
    unittest.main()
