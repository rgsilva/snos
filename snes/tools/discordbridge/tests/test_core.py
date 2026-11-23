import _setup  # noqa
import os
import tempfile
import unittest

from core import (Blocklist, DiscordMsg, Relay, RelayConfig, clean_nick, clean_text, cut,
                  irc_privmsg, neutralize_mentions, to_discord)


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


if __name__ == "__main__":
    unittest.main()
