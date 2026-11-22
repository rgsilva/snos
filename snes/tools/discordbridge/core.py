"""discordbridge core: text cleaning, nicks, filters, queue. No network, no discord import."""
from __future__ import annotations

import logging
import os
import re
import unicodedata
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Deque, Iterable

log = logging.getLogger("discordbridge")

MAX_IRC_LINE = 400  # bytes, without CRLF; the console accepts 512
NICK_MAX = 15
DEFAULT_CONSOLE_NICK = "snes"
FAKE_NICK = "discord"  # the one fixed entry in the NAMES list

# ------------------------------------------------------------------ text

_CUSTOM_EMOJI = re.compile(r"<a?:(\w+):\d+>")
_MIRC_COLOR = re.compile(r"\x03\d{1,2}(?:,\d{1,2})?|\x04[0-9A-Fa-f]{6}(?:,[0-9A-Fa-f]{6})?")
_NEWLINES = re.compile(r"\r\n|[\r\n  \x0b\x0c\x85]+")
_SPACES = re.compile(r" {2,}")

_TYPO = str.maketrans({
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"',
    "«": '"', "»": '"',
    "–": "-", "—": "-", "‒": "-", "―": "-", "−": "-",
    "…": "...", " ": " ", " ": " ", " ": " ", " ": " ",
    " ": " ", "•": "*", "·": ".",
})
_SPECIAL = str.maketrans({
    "ß": "ss", "ø": "o", "Ø": "O", "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE",
    "ł": "l", "Ł": "L", "đ": "d", "Đ": "D", "ð": "d", "Ð": "D", "þ": "th",
    "Þ": "TH", "ı": "i",
})


def fold(s: str) -> str:
    """Typographic marks and accents to ASCII; everything else non-ASCII is dropped."""
    s = s.translate(_TYPO).translate(_SPECIAL)
    s = unicodedata.normalize("NFKD", s)
    return "".join(c for c in s if c == "\t" or c == "\n" or " " <= c <= "~")


def clean_text(s: str) -> str:
    s = _CUSTOM_EMOJI.sub(lambda m: ":" + m.group(1) + ":", s)
    parts = [p.strip() for p in _NEWLINES.split(s)]
    s = " / ".join(p for p in parts if p)
    s = _MIRC_COLOR.sub("", s)
    s = s.replace("\t", " ")
    s = fold(s)
    s = "".join(c for c in s if " " <= c <= "~")
    return _SPACES.sub(" ", s).strip()


def clean_nick(name: str, console_nick: str = DEFAULT_CONSOLE_NICK) -> str:
    n = re.sub(r"[^A-Za-z0-9_-]", "", fold(name))[:NICK_MAX]
    if not n:
        n = "anon"
    if n[0].isdigit() or n[0] == "-":
        n = ("_" + n)[:NICK_MAX]
    reserved = {console_nick.lower(), FAKE_NICK}
    while n.lower() in reserved:
        n = n[: NICK_MAX - 1] + "_"
    return n


def cut(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[: max(limit - 3, 0)].rstrip() + "..."


def neutralize_mentions(text: str) -> str:
    """Break @everyone, @here and <@...>/<#...> so Discord does not resolve them."""
    zw = "​"
    text = re.sub(r"@(everyone|here)", "@" + zw + r"\1", text, flags=re.I)
    return re.sub(r"<([@#])", "<" + zw + r"\1", text)


def to_discord(prefix: str, text: str, console_nick: str, action: bool,
               default_nick: str = DEFAULT_CONSOLE_NICK) -> str:
    text = "".join(c for c in text if c >= " " and c != "\x7f").strip()
    if action:
        text = "*" + text + "*"
    who = "" if console_nick.lower() == default_nick.lower() else console_nick + ": "
    return neutralize_mentions(f"{prefix} {who}{text}")[:1900]


# ------------------------------------------------------------- moderation

class Blocklist:
    """Words or phrases, one per line, matched case-insensitively as substrings."""

    def __init__(self, path: str | None = None, words: Iterable[str] = ()):
        self.path = path
        self._mtime: float | None = None
        self._words = [w.lower() for w in words if w.strip()]

    def _reload(self) -> None:
        if not self.path:
            return
        try:
            mtime = os.stat(self.path).st_mtime
        except OSError:
            self._words, self._mtime = [], None
            return
        if mtime == self._mtime:
            return
        self._mtime = mtime
        with open(self.path, encoding="utf-8", errors="replace") as f:
            lines = [fold(x).strip().lower() for x in f]
        self._words = [x for x in lines if x and not x.startswith("#")]

    def hit(self, *texts: str) -> str | None:
        self._reload()
        for t in texts:
            low = t.lower()
            for w in self._words:
                if w in low:
                    return w
        return None


# ------------------------------------------------------------------ relay

@dataclass
class DiscordMsg:
    channel_id: int
    display_name: str
    content: str  # already mention-resolved (clean_content)
    is_bot: bool = False
    is_self: bool = False
    role_ids: frozenset[int] = frozenset()
    images: int = 0
    files: int = 0
    stickers: int = 0
    embeds: int = 0


@dataclass
class RelayConfig:
    channel_id: int | None = None
    irc_channel: str = "#palestra"
    max_chars: int = 160
    interval: float = 1.5
    queue_size: int = 20
    mute_file: str = "MUTE"
    allow_role: int | None = None
    prefix: str = "**[SNES]**"


@dataclass
class Relay:
    cfg: RelayConfig
    clock: Callable[[], float]
    blocklist: Blocklist = field(default_factory=Blocklist)
    console_nick: str = DEFAULT_CONSOLE_NICK
    console_ready: bool = False
    muted: Callable[[], bool] | None = None
    queue: Deque[tuple[str, str]] = field(default_factory=deque)
    _next_at: float = 0.0

    def is_muted(self) -> bool:
        if self.muted is not None:
            return self.muted()
        return os.path.exists(self.cfg.mute_file)

    @staticmethod
    def tags(m: DiscordMsg) -> str:
        t = []
        if m.images:
            t.append("[imagem]")
        if m.files:
            t.append("[anexo]")
        if m.stickers:
            t.append("[figurinha]")
        if m.embeds and not (m.images or m.files or m.stickers):
            t.append("[link]")
        return " ".join(t)

    def build(self, m: DiscordMsg) -> tuple[str, str]:
        """(nick, text) for a message, with no drop decisions."""
        nick = clean_nick(m.display_name, self.console_nick)
        text = cut(clean_text(m.content), self.cfg.max_chars)
        tags = self.tags(m)
        return nick, (text + " " + tags).strip()

    def from_discord(self, m: DiscordMsg) -> str | None:
        """Queue a message. Returns the drop reason, or None when queued."""
        if m.is_self or m.is_bot:
            return "bot"
        if self.cfg.channel_id is not None and m.channel_id != self.cfg.channel_id:
            return "other channel"
        if self.cfg.allow_role is not None and self.cfg.allow_role not in m.role_ids:
            return "role"
        if self.is_muted():
            return "muted"
        if not self.console_ready:
            return "no console"
        nick, text = self.build(m)
        if not text:
            return "empty"
        hit = self.blocklist.hit(clean_text(m.content), nick, clean_text(m.display_name))
        if hit:
            return f"blocklist ({hit})"
        if len(self.queue) >= self.cfg.queue_size:
            old = self.queue.popleft()
            log.info("drop (queue full) oldest: %s: %s", *old)
        self.queue.append((nick, text))
        return None

    def pop_due(self) -> tuple[str, str] | None:
        """Next line to send now, or None. Muted or no console empties the queue."""
        if self.queue and (self.is_muted() or not self.console_ready):
            why = "muted" if self.is_muted() else "no console"
            for n, t in self.queue:
                log.info("drop (%s) %s: %s", why, n, t)
            self.queue.clear()
            return None
        now = self.clock()
        if not self.queue or now < self._next_at:
            return None
        self._next_at = now + self.cfg.interval
        return self.queue.popleft()

    def from_console(self, text: str, action: bool = False) -> str | None:
        """Text to post on Discord, or None while muted."""
        if self.is_muted():
            return None
        return to_discord(self.cfg.prefix, text, self.console_nick, action)


def irc_privmsg(channel: str, nick: str, text: str) -> str:
    """The IRC line for one Discord message, never over MAX_IRC_LINE bytes."""
    head = f":{nick}!d@discord PRIVMSG {channel} :"
    room = MAX_IRC_LINE - len(head)
    if len(text) > room:
        text = text[: room - 3] + "..."
    return head + text
