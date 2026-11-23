"""Many channels and the names list, end to end on localhost with the fake guild."""
import _setup  # noqa
import asyncio
import os
import re
import sys
import tempfile
import unittest

from _console import Console
from core import DiscordMsg
from discordbridge import Bridge, Settings
from fakeguild import FakeGuild
from ircd import split_names

SRV = ":discordbridge.local"


class Clock:
    t = 1000.0

    def __call__(self):
        return self.t


class Base(unittest.IsolatedAsyncioTestCase):
    guild_text: str | None = None
    settings: dict = {}
    fake_clock = False

    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.posts: list[tuple[int, str]] = []

        async def post(cid, text):
            self.posts.append((cid, text))

        s = Settings(channel_id=7, irc_channel="#palestra", bind="127.0.0.1", port=0, interval=0.02,
                     mute_file=os.path.join(self.tmp.name, "MUTE"),
                     blocklist=os.path.join(self.tmp.name, "none.txt"), **self.settings)
        self.clock = Clock()
        self.guild = FakeGuild(self.guild_text) if self.guild_text else FakeGuild()
        self.bridge = Bridge(s, post, self.clock) if self.fake_clock else Bridge(s, post)
        self.port = await self.bridge.start()
        if self.fake_clock:
            self.bridge._tasks[2].cancel()  # the names loop: the tests call names_tick themselves
        self.bridge.sync(self.guild.snapshot())
        self.consoles = []

    async def asyncTearDown(self):
        for c in self.consoles:
            await c.close()
        await self.bridge.stop()
        self.tmp.cleanup()

    async def console(self, *joins, nick="snes"):
        c = await Console(self.port).connect()
        self.consoles.append(c)
        await c.register(nick)
        for j in joins:
            await c.join(j)
        return c

    def say(self, chan, name, text="oi", **kw):
        """A Discord message from a person of the fake guild."""
        mid = self.guild.member_id(name, create=True)
        self.bridge.sync(self.guild.snapshot())
        self.bridge.on_discord_message(
            DiscordMsg(self.guild.channel_id(chan), self.guild.members[mid], text, author_id=mid, **kw))

    def change(self, command):
        self.assertIsNone(self.guild.command(command))
        self.bridge.sync(self.guild.snapshot())

    @staticmethod
    async def quiet(c, t=0.15):
        """The next line from the console, or None if it stays quiet."""
        try:
            return await c.line(t)
        except asyncio.TimeoutError:
            return None


def names_of(lines):
    """The nicks of the 353 lines in `lines`, in order."""
    out = []
    for l in lines:
        if " 353 " in l:
            out += l.split(" :", 1)[1].split()
    return out


class Channels(Base):
    async def test_welcome_text(self):
        c = await Console(self.port).connect()
        self.consoles.append(c)
        lines = await c.register()
        motd = [l for l in lines if " 372 " in l]
        self.assertTrue(any("#palestra" in l for l in motd))
        self.assertTrue(any("/raw LIST" in l for l in motd))
        for l in motd:
            self.assertLessEqual(len(l.split(" :", 1)[1]), 60)

    async def test_list(self):
        c = await self.console()
        c.send("LIST\r\n")
        lines = await c.until(lambda l: " 323 " in l)
        rows = [l for l in lines if " 322 " in l]
        self.assertEqual([l.split()[3] for l in rows], ["#geral", "#off-topic", "#palestra", "#so-staff"])
        self.assertEqual([l.split()[4] for l in rows], ["4", "3", "4", "4"])
        self.assertTrue(rows[0].endswith(":#geral (4) Conversa solta"))
        self.assertTrue(rows[1].endswith(":#off-topic (3) Discord #off-topic"))

    async def test_list_topic_is_ascii_and_short(self):
        self.guild.channels[8] = ("geral", "Ótimo \U0001F600 papo " + "x" * 200)
        self.bridge.sync(self.guild.snapshot())
        c = await self.console()
        c.send("LIST #geral\r\n")
        lines = await c.until(lambda l: " 323 " in l)
        self.assertEqual(len(lines), 2)
        self.assertLessEqual(len(lines[0].split(" :", 1)[1]), len("#geral (4) ") + 40)
        self.assertIn("Otimo papo", lines[0])

    async def test_join_several_and_route_both_ways(self):
        c = await self.console()
        c.send("JOIN #palestra,#geral\r\n")
        lines = await c.until(lambda l: "#geral :End of NAMES" in l)
        self.assertEqual([l for l in lines if " JOIN " in l],
                         [":snes!snes@console JOIN #palestra", ":snes!snes@console JOIN #geral"])
        self.assertEqual(len([l for l in lines if " 332 " in l]), 2)
        self.assertEqual(names_of(lines), ["snes", "Ana", "Bruno", "Carla", "JoaoSilva"] * 2)
        self.assertEqual(self.bridge.relay.joined, {7, 8})
        self.say("geral", "Ana", "oi geral")
        self.say("palestra", "Bruno", "oi palestra")
        self.say("off-topic", "Carla", "ninguem ouve")
        got = []
        for _ in range(2):
            got.append((await c.until(lambda l: "PRIVMSG" in l))[-1])
        self.assertEqual(got, [":Ana!d@discord PRIVMSG #geral :oi geral",
                               ":Bruno!d@discord PRIVMSG #palestra :oi palestra"])
        self.assertIsNone(await self.quiet(c, 0.2))
        c.send("PRIVMSG #geral :ola geral\r\nPRIVMSG #palestra :ola palestra\r\n")
        await asyncio.sleep(0.1)
        self.assertEqual(self.posts, [(8, "ola geral"), (7, "ola palestra")])

    async def test_not_joined_is_dropped_silently_and_counted(self):
        c = await self.console("#palestra")
        for i in range(3):
            self.say("geral", "Ana", f"m{i}")
        self.say("palestra", "Ana", "aqui")
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertTrue(l[-1].endswith(":aqui"))
        self.assertEqual(self.bridge.relay.dropped["not joined"], 3)
        self.bridge.log_stats()
        self.assertEqual(self.bridge.relay.dropped["not joined"], 0)

    async def test_unknown_and_not_joined(self):
        c = await self.console("#palestra")
        c.send("JOIN #nada\r\nPRIVMSG #nada :x\r\nPRIVMSG #geral :x\r\nPART #geral\r\nPART #nada\r\n"
               "TOPIC #nada\r\nMODE #nada\r\nPING :z\r\n")
        lines = await c.until(lambda l: " PONG " in l)
        codes = [l.split()[1] for l in lines if l.startswith(SRV)]
        self.assertEqual(codes, ["403", "403", "404", "442", "403", "403", "403", "PONG"])
        self.assertIn("#geral :Cannot send", lines[2])
        self.assertEqual(self.posts, [])

    async def test_part_stops_one_channel_only(self):
        c = await self.console("#palestra", "#geral")
        c.send("PART #palestra\r\n")
        l = await c.until(lambda l: " PART " in l)
        self.assertEqual(l[-1], ":snes!snes@console PART #palestra")
        self.say("palestra", "Ana", "ninguem")
        self.say("geral", "Ana", "alguem")
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertIn("#geral", l[-1])
        c.send("PRIVMSG #palestra :x\r\n")
        l = await c.until(lambda l: " 404 " in l)
        self.assertEqual(self.bridge.relay.joined, {8})

    async def test_topic_and_names_commands(self):
        c = await self.console("#geral")
        c.send("TOPIC #geral\r\nTOPIC #geral :novo\r\nNAMES #geral\r\nNAMES #palestra\r\nJOIN #GERAL\r\nPING :z\r\n")
        lines = await c.until(lambda l: " PONG " in l)
        self.assertEqual(lines[0], f"{SRV} 332 snes #geral :Conversa solta")
        self.assertIn(" 482 ", lines[1])
        self.assertEqual(names_of(lines), ["snes", "Ana", "Bruno", "Carla", "JoaoSilva"])
        self.assertIn(" 366 snes * :", lines[-2])  # not joined: nothing to list
        self.assertEqual(len([l for l in lines if " JOIN " in l]), 0)  # already in

    async def test_nick_prefix_matches_names(self):
        c = await self.console("#palestra")
        self.say("palestra", "Joao Silva", "oi")
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertTrue(l[-1].startswith(":JoaoSilva!"))
        c.send("NAMES #palestra\r\n")
        l = await c.until(lambda l: " 366 " in l)
        self.assertEqual(names_of(l)[1], "JoaoSilva")  # spoke last, first after the console

    async def test_default_channel_joinable_before_discord_is_ready(self):
        s = Settings(channel_id=7, irc_channel="#palestra", bind="127.0.0.1", port=0)
        b = Bridge(s, lambda *a: None)
        port = await b.start()
        try:
            c = await Console(port).connect()
            await c.register()
            await c.join("#palestra")
            self.assertEqual(b.relay.joined, {7})
            await c.close()
        finally:
            await b.stop()


class Allow(Base):
    settings = {"allow_channels": frozenset({7, 8})}

    async def test_only_allowed_channels(self):
        c = await self.console()
        c.send("LIST\r\nJOIN #off-topic\r\n")
        lines = await c.until(lambda l: " 403 " in l)
        self.assertEqual([l.split()[3] for l in lines if " 322 " in l], ["#geral", "#palestra"])


class Clash(Base):
    guild_text = "channel 20 chat\nchannel 10 Chat\nchannel 5 chat2\nmember 1 Ana\n"

    async def test_names_are_unique_and_by_id(self):
        c = await self.console()
        c.send("LIST\r\n")
        lines = await c.until(lambda l: " 323 " in l)
        self.assertEqual([l.split()[3] for l in lines if " 322 " in l], ["#chat", "#chat2", "#chat3"])
        self.assertEqual(self.bridge.directory.id_of("#chat"), 10)
        self.assertEqual(self.bridge.directory.id_of("#chat2"), 5)
        self.assertEqual(self.bridge.directory.id_of("#chat3"), 20)


class Changes(Base):
    async def test_rename_while_joined_keeps_the_old_name(self):
        c = await self.console("#geral")
        self.change("/renchan geral > bate-papo")
        l = await c.until(lambda l: " NOTICE " in l)
        self.assertEqual(l[-1], f"{SRV} NOTICE snes :#geral is now #bate-papo on Discord; "
                                "you keep #geral until you part")
        self.say("bate-papo", "Ana", "ainda chega")
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertIn("PRIVMSG #geral :", l[-1])
        c.send("PRIVMSG #geral :ainda sai\r\n")
        await asyncio.sleep(0.1)
        self.assertEqual(self.posts, [(8, "ainda sai")])
        c.send("JOIN #bate-papo\r\nPING :z\r\n")
        l = await c.until(lambda l: " PONG " in l)
        self.assertIn(" 403 ", l[0])  # one name per channel for this console
        self.change("/hide Ana #bate-papo")  # more events: no second notice
        c.send("PART #geral\r\nJOIN #bate-papo\r\nPING :z\r\n")
        l = await c.until(lambda l: " PONG " in l)
        self.assertEqual([x for x in l if " JOIN " in x], [":snes!snes@console JOIN #bate-papo"])
        self.assertEqual(names_of(l), ["snes", "Bruno", "Carla", "JoaoSilva"])
        c.send("JOIN #geral\r\n")
        l = await c.until(lambda l: " 403 " in l)

    async def test_deleted_channel_kicks(self):
        c = await self.console("#geral", "#palestra")
        self.change("/delchan geral")
        l = await c.until(lambda l: " KICK " in l)
        self.assertEqual(l[-1], f"{SRV} KICK #geral snes :channel removed or hidden on Discord")
        self.assertEqual(self.bridge.relay.joined, {7})
        self.say("palestra", "Ana", "ainda")
        await c.until(lambda l: "PRIVMSG" in l)
        c.send("PRIVMSG #geral :x\r\n")
        await c.until(lambda l: " 403 " in l)

    async def test_new_channel_appears_in_list(self):
        c = await self.console()
        self.change("/newchan novo | Um novo")
        c.send("LIST\r\n")
        lines = await c.until(lambda l: " 323 " in l)
        self.assertIn("#novo", [l.split()[3] for l in lines if " 322 " in l])
        await c.join("#novo")

    async def test_console_drop_frees_everything(self):
        c = await self.console("#geral", "#palestra")
        c.w.transport.abort()
        await asyncio.sleep(0.1)
        self.assertEqual(self.bridge.relay.joined, set())
        self.assertFalse(self.bridge.relay.console_ready)


class Names(Base):
    fake_clock = True

    async def push(self, c, advance):
        """Advance the clock, tick, and return the lines of the push (or None)."""
        self.clock.t += advance
        self.bridge.names_tick()
        try:
            return await c.until(lambda l: " 366 " in l, 0.2)
        except asyncio.TimeoutError:
            return None

    async def test_push_on_join_leave_rename_and_first_message(self):
        c = await self.console("#palestra")
        self.assertIsNone(await self.push(c, 0))  # nothing changed

        self.change("/join Eva")
        self.assertIsNone(await self.push(c, 1))  # too soon after the join
        got = await self.push(c, 3)
        self.assertEqual(names_of(got), ["snes", "Ana", "Bruno", "Carla", "Eva", "JoaoSilva"])
        self.assertTrue(got[0].startswith(f"{SRV} 353 snes = #palestra :"))
        self.assertEqual(len(got), 2)  # one 353 and the 366, no JOIN or PART

        self.change("/leave Bruno")
        self.assertEqual(names_of(await self.push(c, 3)), ["snes", "Ana", "Carla", "Eva", "JoaoSilva"])

        self.change("/rename Carla > Carlota")
        self.assertEqual(names_of(await self.push(c, 3)), ["snes", "Ana", "Carlota", "Eva", "JoaoSilva"])

        self.change("/hide Ana #palestra")  # loses access
        self.assertEqual(names_of(await self.push(c, 3)), ["snes", "Carlota", "Eva", "JoaoSilva"])

        self.say("palestra", "Newbie", "primeira vez")  # joins the server and speaks
        await c.until(lambda l: "PRIVMSG" in l)
        got = await self.push(c, 3)
        self.assertEqual(names_of(got)[:2], ["snes", "Newbie"])

    async def test_changes_in_one_window_make_one_push(self):
        c = await self.console("#palestra")
        for who in ("Eva", "Fabio", "Gil"):
            self.change(f"/join {who}")
            self.clock.t += 0.5
            self.bridge.names_tick()
        self.assertIsNone(await self.quiet(c, 0.1))
        got = await self.push(c, 3)
        self.assertEqual(names_of(got), ["snes", "Ana", "Bruno", "Carla", "Eva", "Fabio", "Gil", "JoaoSilva"])
        self.assertIsNone(await self.push(c, 100))

    async def test_order_alone_waits_a_minute(self):
        c = await self.console("#palestra")
        self.say("palestra", "Ana", "um")
        await c.until(lambda l: "PRIVMSG" in l)
        self.assertIsNone(await self.push(c, 10))
        self.say("palestra", "Bruno", "dois")
        await c.until(lambda l: "PRIVMSG" in l)
        self.assertIsNone(await self.push(c, 40))
        got = await self.push(c, 20)  # a minute since the console got its list
        self.assertEqual(names_of(got)[:3], ["snes", "Bruno", "Ana"])

    async def test_only_joined_channels_get_pushes(self):
        c = await self.console("#geral")
        self.change("/join Eva")
        self.assertEqual(names_of(await self.push(c, 5))[-2:], ["Eva", "JoaoSilva"])
        c.send("PART #geral\r\n")
        await c.until(lambda l: " PART " in l)
        self.change("/join Fabio")
        self.assertIsNone(await self.push(c, 10))

    async def test_a_name_beyond_the_cap_enters_when_it_speaks(self):
        self.bridge.s.names_max = 3
        c = await self.console("#palestra")
        c.send("NAMES #palestra\r\n")
        self.assertEqual(names_of(await c.until(lambda l: " 366 " in l)), ["snes", "Ana", "Bruno"])
        self.say("palestra", "Carla", "oi")
        await c.until(lambda l: "PRIVMSG" in l)
        self.assertEqual(names_of(await self.push(c, 3)), ["snes", "Carla", "Ana"])

    async def test_cap_and_split_under_400_bytes(self):
        text = "channel 7 palestra\n" + "".join(f"member {i} Participante{i:04d}\n" for i in range(1, 201))
        self.guild = FakeGuild(text)
        self.bridge.sync(self.guild.snapshot())
        c = await self.console()
        await c.join("#palestra")
        c.send("NAMES #palestra\r\n")
        lines = await c.until(lambda l: " 366 " in l)
        self.assertEqual(len(names_of(lines)), 48)  # the default cap, console included
        self.assertGreaterEqual(len([l for l in lines if " 353 " in l]), 2)
        self.assertTrue(all(len(l) < 400 for l in lines))
        self.bridge.s.names_max = 300
        c.send("NAMES #palestra\r\n")
        lines = await c.until(lambda l: " 366 " in l)
        names = names_of(lines)
        self.assertEqual(len(names), 201)
        self.assertEqual(len(set(n.lower() for n in names)), 201)
        self.assertGreaterEqual(len([l for l in lines if " 353 " in l]), 5)
        self.assertTrue(all(len(l) < 400 for l in lines))
        self.assertTrue(re.fullmatch(r".* 366 snes #palestra :End of NAMES list", lines[-1]))

    async def test_a_push_is_never_a_partial_list(self):
        text = "channel 7 palestra\n" + "".join(f"member {i} Participante{i:04d}\n" for i in range(1, 101))
        self.guild = FakeGuild(text)
        self.bridge.s.names_max = 100
        self.bridge.sync(self.guild.snapshot())
        c = await self.console("#palestra")
        self.change("/join Novo")
        self.clock.t += 5
        self.bridge.names_tick()
        self.bridge.on_discord_message(DiscordMsg(7, "x", "y", is_bot=True))  # other traffic between
        lines = await c.until(lambda l: " 366 " in l)
        self.assertGreaterEqual(len(lines), 4)
        self.assertTrue(all(" 353 " in l for l in lines[:-1]))
        self.assertEqual(len(names_of(lines)), 100)

    async def test_stable_nicks_for_a_person(self):
        self.guild = FakeGuild("channel 7 palestra\nmember 1 Ana\nmember 2 ana\nmember 3 Ana!\n"
                               "member 4 snes\nmember 5 \U0001F600\n")
        self.bridge.sync(self.guild.snapshot())
        c = await self.console("#palestra")
        c.send("NAMES #palestra\r\n")
        first = names_of(await c.until(lambda l: " 366 " in l))
        self.assertEqual(sorted(first[1:], key=str.lower), ["Ana", "ana2", "Ana3", "anon", "snes_"])
        self.change("/leave Ana")
        self.change("/join Zeca")
        c.send("NAMES #palestra\r\n")
        again = names_of(await c.until(lambda l: " 366 " in l))
        for nick in ("ana2", "Ana3", "anon", "snes_"):
            self.assertIn(nick, again)
        self.say("palestra", "ana", "sou eu")  # the prefix is the same nick as in the list
        l = await c.until(lambda l: "PRIVMSG" in l)
        self.assertTrue(l[-1].startswith(":ana2!"))

    async def test_members_intent_missing_falls_back_to_speakers(self):
        self.change("/nomembers")
        c = await self.console("#palestra")
        c.send("NAMES #palestra\r\nLIST #palestra\r\n")
        lines = await c.until(lambda l: " 323 " in l)
        self.assertEqual(names_of(lines), ["snes"])
        self.assertTrue(any(l.endswith("(0) Canal da palestra") for l in lines))
        self.say("palestra", "Carla", "oi")
        await c.until(lambda l: "PRIVMSG" in l)
        self.assertEqual(names_of(await self.push(c, 3)), ["snes", "Carla"])
        self.say("geral", "Bruno", "outro canal")
        self.assertEqual(self.bridge.roster.names(7, "snes", 48), ["snes", "Carla"])
        self.change("/members")  # the intent came back
        self.assertEqual(names_of(await self.push(c, 3)), ["snes", "Carla", "Ana", "Bruno", "JoaoSilva"])

    async def test_console_nick_change_is_not_a_push(self):
        c = await self.console("#palestra")
        c.send("NICK ric\r\n")
        await c.until(lambda l: " NICK " in l)
        self.assertIsNone(await self.push(c, 100))
        c.send("NAMES #palestra\r\n")
        self.assertEqual(names_of(await c.until(lambda l: " 366 " in l))[0], "ric")


class FakeGuildFile(unittest.TestCase):
    def test_parse_and_errors(self):
        g = FakeGuild("# c\nchannel 3 um | topo\nchannel 4 dois\nmember 1 Ana Maria | 3\nmember 2 Bia\n")
        snap = g.snapshot()
        self.assertEqual([(c.id, c.name, c.topic, sorted(c.viewers)) for c in snap.channels],
                         [(3, "um", "topo", [1, 2]), (4, "dois", "", [2])])
        self.assertEqual(g.default_id, 3)
        msg, created = g.message("#dois Zé: oi: tudo")
        self.assertEqual((msg.channel_id, msg.display_name, msg.content, created), (4, "Zé", "oi: tudo", True))
        self.assertEqual(g.message("Bia: x")[0].channel_id, 3)
        self.assertIsNone(g.message("#nada Bia: x")[0])
        self.assertIsNone(g.message("sem dois pontos")[0])
        self.assertIn("/join", g.command("/xyz"))
        with self.assertRaises(ValueError):
            FakeGuild("channel x y")
        with self.assertRaises(ValueError):
            FakeGuild("member 1 Ana")


class DryRun(unittest.IsolatedAsyncioTestCase):
    async def test_the_program_with_stdin(self):
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        env = dict(os.environ, DISCORDBRIDGE_BIND="127.0.0.1", DISCORDBRIDGE_PORT="0",
                   DISCORDBRIDGE_INTERVAL="0.02", DISCORDBRIDGE_MUTE_FILE=os.devnull + ".none")
        p = await asyncio.create_subprocess_exec(
            sys.executable, "-u", os.path.join(here, "discordbridge.py"), "--dry-run",
            "--env-file", os.devnull, cwd=here, env=env, stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
        try:
            async def out(want):
                while True:
                    l = (await asyncio.wait_for(p.stdout.readline(), 5)).decode()
                    if want in l:
                        return l
            port = int(re.search(r":(\d+)$", (await out("listening on")).strip()).group(1))
            self.assertIn("#geral=geral", await out("bridged channels"))
            c = await Console(port).connect()
            await c.register()
            await c.join("#geral")
            p.stdin.write(b"#geral Ana: oi do palco\n/join Eva\n")
            await p.stdin.drain()
            l = await c.until(lambda l: "PRIVMSG" in l)
            self.assertEqual(l[-1], ":Ana!d@discord PRIVMSG #geral :oi do palco")
            c.send("PRIVMSG #geral :de volta\r\n")
            self.assertIn("would post to Discord #geral: de volta", await out("would post"))
            await c.close()
        finally:
            p.kill()
            await p.wait()


class Split(unittest.TestCase):
    def test_split(self):
        head = ":discordbridge.local 353 snes = #palestra :"
        names = [f"n{i:013d}" for i in range(100)]
        lines = split_names(head, names)
        self.assertTrue(all(len(l) < 400 for l in lines))
        self.assertEqual(" ".join(l[len(head):] for l in lines).split(), names)
        self.assertEqual(split_names(head, ["a"]), [head + "a"])


if __name__ == "__main__":
    unittest.main()
