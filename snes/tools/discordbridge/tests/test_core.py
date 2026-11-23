import _setup  # noqa
import os
import tempfile
import unittest

from core import (Blocklist, ChannelInfo, Directory, DiscordMsg, NickBook, PushPolicy, Relay,
                  RelayConfig, Roster, Snapshot, channel_slug, clean_nick, clean_text, cut,
                  derive_names, irc_privmsg, neutralize_mentions, to_discord)
from ircd import valid_channel


class CleanText(unittest.TestCase):
    CASES = [
        ("ação", "acao"), ("coração", "coracao"), ("É", "E"), ("Straße Øl æon łódź", "Strasse Ol aeon lodz"),
        ("oi \U0001F600 tudo", "oi tudo"), ("\U0001F600", ""), ("<:pepe:123456789> hi", ":pepe: hi"),
        ("<a:dance:99> x", ":dance: x"), ("@Maria ola", "@Maria ola"),
        ("a\nb\n\nc", "a / b / c"), ("a\r\nb", "a / b"), ("a\tb", "a b"), ("a\x00b\x07c\x7f", "abc"),
        ("\x02bold\x0f \x034,5red\x03 ok", "bold red ok"),
        ("zero​width", "zerowidth"), ("rtl‮evil", "rtlevil"),
        ("“aspas” ‘x’ a—b–c…", "\"aspas\" 'x' a-b-c..."),
        ("a  b", "a b"), ("  muito    espaco  ", "muito espaco"),
        ("**bold** _it_ `code`", "**bold** _it_ `code`"), ("ＡＢ", "AB"),
    ]

    def test_table(self):
        for src, want in self.CASES:
            self.assertEqual(clean_text(src), want, repr(src))

    def test_output_is_printable_ascii(self):
        nasty = "".join(chr(i) for i in range(0x3000)) + "\U0001F600\U0001F1E7\U0001F1F7"
        out = clean_text(nasty)
        self.assertTrue(all(" " <= c <= "~" for c in out))

    def test_2000_chars_cut(self):
        out = cut(clean_text("x" * 2000), 160)
        self.assertEqual(len(out), 160)
        self.assertTrue(out.endswith("..."))
        self.assertEqual(cut("short", 160), "short")


class Nick(unittest.TestCase):
    def test_table(self):
        for src, want in [("Ana Maria", "AnaMaria"), ("João", "Joao"), ("\U0001F600\U0001F600", "anon"),
                          ("", "anon"), ("1337", "_1337"), ("-x", "_-x"), ("a" * 40, "a" * 15),
                          ("12345678901234567890", "_12345678901234"), ("A_b-c", "A_b-c")]:
            self.assertEqual(clean_nick(src), want, repr(src))

    def test_console_nick_and_fake_nick(self):
        self.assertEqual(clean_nick("snes"), "snes_")
        self.assertEqual(clean_nick("SNES", "snes"), "SNES_")
        self.assertEqual(clean_nick("snes", "retro"), "snes")
        self.assertEqual(clean_nick("x" * 15, "x" * 15), "x" * 14 + "_")
        self.assertEqual(clean_nick("Discord"), "Discord_")


class ToDiscord(unittest.TestCase):
    def test_mentions_neutralised(self):
        for t in ["@everyone", "@here", "<@123>", "<@&5>", "<#9>", "@EVERYONE"]:
            out = neutralize_mentions(t)
            self.assertNotIn("@everyone", out.lower())
            self.assertNotIn("<@", out)
            self.assertNotIn("<#", out)

    def test_format(self):
        self.assertEqual(to_discord("**[SNES]**", "oi", "snes", False), "**[SNES]** oi")
        self.assertEqual(to_discord("**[SNES]**", "waves", "snes", True), "**[SNES]** *waves*")
        self.assertEqual(to_discord("**[SNES]**", "oi", "ric", False), "**[SNES]** ric: oi")
        self.assertLessEqual(len(to_discord("p", "x" * 5000, "snes", False)), 2000)


def msg(text="oi", name="ana", **kw):
    return DiscordMsg(channel_id=kw.pop("channel_id", 7), display_name=name, content=text, **kw)


class Clock:
    t = 100.0

    def __call__(self):
        return self.t


class RelayTests(unittest.TestCase):
    def make(self, **kw):
        self.clock = Clock()
        self.muted = False
        cfg = RelayConfig(**kw)
        r = Relay(cfg, self.clock, muted=lambda: self.muted)
        r.console_ready = True
        r.joined = {7}
        return r

    def test_drop_reasons(self):
        r = self.make()
        self.assertEqual(r.from_discord(msg(is_bot=True)), "bot")
        self.assertEqual(r.from_discord(msg(is_self=True)), "bot")
        self.assertEqual(r.from_discord(msg(channel_id=8)), "not joined")
        self.assertEqual(r.dropped["not joined"], 1)
        self.assertEqual(r.from_discord(msg("\U0001F600")), "empty")
        self.assertIsNone(r.from_discord(msg("\U0001F600", images=1)))
        r.console_ready = False
        self.assertEqual(r.from_discord(msg()), "no console")
        self.assertEqual(len(r.queue), 1)

    def test_tags(self):
        r = self.make()
        r.from_discord(msg("olha", images=1, files=1))
        r.from_discord(msg("", stickers=1))
        r.from_discord(msg("", embeds=1))
        self.assertEqual([q[1] for q in r.queue], ["olha [imagem] [anexo]", "[figurinha]", "[link]"])

    def test_role(self):
        r = self.make(allow_role=5)
        self.assertEqual(r.from_discord(msg(role_ids=frozenset({1}))), "role")
        self.assertIsNone(r.from_discord(msg(role_ids=frozenset({5}))))

    def test_blocklist(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "b.txt")
            with open(p, "w") as f:
                f.write("# c\nbadword\nMau Nome\n")
            r = self.make()
            r.blocklist = Blocklist(p)
            self.assertTrue(r.from_discord(msg("hi BADWORD there")).startswith("blocklist"))
            self.assertTrue(r.from_discord(msg("hi", name="mau nome")).startswith("blocklist"))
            self.assertTrue(r.from_discord(msg("bádword")).startswith("blocklist"))
            self.assertIsNone(r.from_discord(msg("fine")))

    def test_drop_oldest(self):
        r = self.make(queue_size=3)
        for i in range(5):
            r.from_discord(msg(f"m{i}"))
        self.assertEqual([q[1] for q in r.queue], ["m2", "m3", "m4"])

    def test_interval(self):
        r = self.make(interval=1.5)
        for i in range(3):
            r.from_discord(msg(f"m{i}"))
        self.assertEqual(r.pop_due()[1], "m0")
        self.assertIsNone(r.pop_due())
        self.clock.t += 1.4
        self.assertIsNone(r.pop_due())
        self.clock.t += 0.2
        self.assertEqual(r.pop_due()[1], "m1")
        self.clock.t += 10
        self.assertEqual(r.pop_due()[1], "m2")
        self.assertIsNone(r.pop_due())

    def test_mute(self):
        r = self.make()
        r.from_discord(msg("a"))
        r.from_discord(msg("b"))
        self.muted = True
        self.assertIsNone(r.pop_due())
        self.assertEqual(len(r.queue), 0)
        self.assertEqual(r.from_discord(msg("c")), "muted")
        self.assertIsNone(r.from_console("hi"))
        self.muted = False
        self.assertIsNone(r.from_discord(msg("d")))
        self.assertEqual(r.pop_due()[1], "d")
        self.assertEqual(r.from_console("hi"), "**[SNES]** hi")

    def test_mute_file(self):
        with tempfile.TemporaryDirectory() as d:
            r = Relay(RelayConfig(mute_file=os.path.join(d, "MUTE")), Clock())
            self.assertFalse(r.is_muted())
            open(os.path.join(d, "MUTE"), "w").close()
            self.assertTrue(r.is_muted())

    def test_no_console_clears_queue(self):
        r = self.make()
        r.from_discord(msg())
        r.console_ready = False
        self.assertIsNone(r.pop_due())
        self.assertEqual(len(r.queue), 0)

    def test_console_nick_collision(self):
        r = self.make()
        r.console_nick = "ric"
        r.from_discord(msg(name="Ric"))
        self.assertEqual(r.queue[0][0], "Ric_")

    def test_irc_line_bounded(self):
        line = irc_privmsg("#palestra", "n" * 15, "x" * 900)
        self.assertLessEqual(len(line.encode()), 400)
        self.assertTrue(line.startswith(":" + "n" * 15 + "!d@discord PRIVMSG #palestra :"))


class ChannelNames(unittest.TestCase):
    def test_slug(self):
        for src, want in [("Geral", "geral"), ("off topic", "off-topic"), ("ação-livre", "acao-livre"),
                          ("\U0001F4AC", "canal"), ("", "canal"), ("--x--", "x"), ("a_b-c1", "a_b-c1"),
                          ("x" * 40, "x" * 23), ("a" * 22 + "-b", "a" * 22), ("#oi!", "oi")]:
            self.assertEqual(channel_slug(src), want, repr(src))

    def test_clashes_by_id_order(self):
        got = derive_names([(30, "chat"), (10, "chat"), (20, "chat"), (5, "chat2")], None, "", {})
        self.assertEqual(got, {5: "#chat2", 10: "#chat", 20: "#chat3", 30: "#chat4"})

    def test_suffix_keeps_the_limit(self):
        got = derive_names([(1, "x" * 30), (2, "x" * 30), (3, "x" * 30)], None, "", {})
        self.assertEqual(list(got.values()), ["#" + "x" * 23, "#" + "x" * 22 + "2", "#" + "x" * 22 + "3"])
        self.assertTrue(all(valid_channel(n) for n in got.values()))

    def test_alias_and_held(self):
        got = derive_names([(1, "geral"), (2, "palestra-ao-vivo")], 2, "#palestra", {})
        self.assertEqual(got, {1: "#geral", 2: "#palestra"})
        got = derive_names([(1, "geral"), (2, "novo")], None, "", {2: "#geral"})
        self.assertEqual(got, {2: "#geral", 1: "#geral2"})
        self.assertEqual(derive_names([(1, "a")], 1, "#PAL", {1: "#old"}), {1: "#old"})

    def test_directory(self):
        d = Directory(2, "#palestra", frozenset({1, 2}))
        ch = [ChannelInfo(1, "geral", "Papo"), ChannelInfo(2, "x"), ChannelInfo(3, "fora")]
        self.assertEqual(d.update(ch, {}).lost, [])
        self.assertEqual([n for n, _ in d.listing()], ["#geral", "#palestra"])
        self.assertEqual(d.id_of("#PALESTRA"), 2)
        self.assertEqual(d.default_name(), "#palestra")
        self.assertIsNone(d.id_of("#fora"))

    def test_rename_while_held_is_told_once(self):
        d = Directory()
        d.update([ChannelInfo(1, "geral")], {})
        held = {1: "#geral"}
        ch = d.update([ChannelInfo(1, "bate-papo")], held)
        self.assertEqual(ch.renamed, [(1, "#geral", "#bate-papo")])
        self.assertEqual(d.name_of(1), "#geral")
        self.assertEqual(d.update([ChannelInfo(1, "bate-papo")], held).renamed, [])
        d.rebuild({})
        self.assertEqual(d.name_of(1), "#bate-papo")
        self.assertEqual(d.update([], held).lost, [1])


class Nicks(unittest.TestCase):
    def test_unique_and_stable(self):
        b = NickBook()
        self.assertEqual(b.nick(1, "Ana", "snes"), "Ana")
        self.assertEqual(b.nick(2, "ana", "snes"), "ana2")
        self.assertEqual(b.nick(3, "Ana!", "snes"), "Ana3")
        for _ in range(3):
            self.assertEqual([b.nick(i, n, "snes") for i, n in ((3, "Ana!"), (2, "ana"), (1, "Ana"))],
                             ["Ana3", "ana2", "Ana"])

    def test_limit_and_suffix(self):
        b = NickBook(8)
        self.assertEqual(b.nick(1, "abcdefghij", "snes"), "abcdefgh")
        self.assertEqual(b.nick(2, "abcdefghijk", "snes"), "abcdefg2")

    def test_console_nick_and_renames(self):
        b = NickBook()
        self.assertEqual(b.nick(1, "ric", "snes"), "ric")
        self.assertEqual(b.nick(1, "ric", "RIC"), "ric_")  # the console took it
        self.assertEqual(b.nick(2, "snes", "snes"), "snes_")
        self.assertEqual(b.nick(1, "Rico", "snes"), "Rico")
        self.assertEqual(b.nick(3, "ric", "snes"), "ric")  # freed by the rename
        self.assertEqual(b.nick(4, "\U0001F600", "snes"), "anon")
        self.assertEqual(b.nick(5, "1337", "snes"), "_1337")


def guild(n, chans=((1, "geral"),)):
    names = {i: f"P{i:03d}" for i in range(1, n + 1)}
    return Snapshot([ChannelInfo(c, name, "", frozenset(names)) for c, name in chans], names, True)


class RosterTests(unittest.TestCase):
    def make(self, snap):
        d = Directory()
        d.update(snap.channels, {})
        r = Roster(d, NickBook())
        r.apply(snap, "snes")
        return r

    def test_order_cap_and_recency(self):
        r = self.make(guild(6))
        self.assertEqual(r.names(1, "snes", 48), ["snes", "P001", "P002", "P003", "P004", "P005", "P006"])
        r.speak(1, 4, "P004")
        r.speak(1, 2, "P002")
        r.speak(1, 4, "P004")
        self.assertEqual(r.names(1, "snes", 48), ["snes", "P004", "P002", "P001", "P003", "P005", "P006"])
        self.assertEqual(r.names(1, "snes", 3), ["snes", "P004", "P002"])
        self.assertEqual(len(r.names(1, "snes", 1)), 1)

    def test_only_who_can_view(self):
        snap = guild(3, ((1, "a"), (2, "b")))
        snap.channels[1] = ChannelInfo(2, "b", "", frozenset({2}))
        r = self.make(snap)
        self.assertEqual(r.names(2, "snes", 48), ["snes", "P002"])
        self.assertEqual(r.count(1), 3)
        self.assertEqual(r.names(99, "snes", 48), ["snes"])

    def test_without_members_only_speakers(self):
        snap = guild(3)
        snap.complete = False
        r = self.make(snap)
        self.assertEqual(r.names(1, "snes", 48), ["snes"])
        r.speak(1, 7, "Zed")
        r.speak(1, 8, "Amy")
        self.assertEqual(r.names(1, "snes", 48), ["snes", "Amy", "Zed"])
        self.assertEqual(r.names(2, "snes", 48), ["snes"])


class Policy(unittest.TestCase):
    def test_due(self):
        p = PushPolicy(3, 60)
        a, b = ["snes", "x", "y"], ["snes", "y", "x"]
        self.assertFalse(p.due(1, a, 0))  # never sent: nothing to refresh
        p.sent(1, a, 100)
        self.assertFalse(p.due(1, ["other"] + a[1:], 200))  # the console's nick alone is no change
        self.assertFalse(p.due(1, a, 200))
        self.assertTrue(p.changed(1, b))
        self.assertFalse(p.due(1, b, 159))
        self.assertTrue(p.due(1, b, 160))  # order only: a minute
        c = ["snes", "x", "z"]
        self.assertFalse(p.due(1, c, 102))
        self.assertTrue(p.due(1, c, 103))  # the people changed: a few seconds
        p.forget(1)
        self.assertFalse(p.due(1, c, 500))


if __name__ == "__main__":
    unittest.main()
