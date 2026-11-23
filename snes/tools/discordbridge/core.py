"""discordbridge core: text cleaning, nicks, channels, names, filters, queue. No network, no discord import."""
from __future__ import annotations

import logging
import os
import re
import unicodedata
from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Callable, Deque, Iterable

log = logging.getLogger("discordbridge")

MAX_IRC_LINE = 400  # bytes, without CRLF; the console accepts 512
NICK_MAX = 15
CHAN_MAX = 23  # characters after the "#"; irc.c tab names hold 24
DEFAULT_CONSOLE_NICK = "snes"
FAKE_NICK = "discord"  # reserved: the server refuses it as a console nick

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


def clean_nick(name: str, console_nick: str = DEFAULT_CONSOLE_NICK, limit: int = NICK_MAX) -> str:
    n = re.sub(r"[^A-Za-z0-9_-]", "", fold(name))[:limit]
    if not n:
        n = "anon"
    if n[0].isdigit() or n[0] == "-":
        n = ("_" + n)[:limit]
    reserved = {console_nick.lower(), FAKE_NICK}
    while n.lower() in reserved:
        n = n[: limit - 1] + "_"
    return n


def channel_slug(name: str) -> str:
    """A Discord channel name as the part of an IRC name after '#': never empty."""
    s = re.sub(r"[^a-z0-9_-]", "", fold(name).lower().replace(" ", "-"))
    return s[:CHAN_MAX].strip("-_") or "canal"


class NickBook:
    """One nick per person, unique across the bridge, kept while the bridge runs."""

    def __init__(self, limit: int = NICK_MAX):
        self.limit = limit
        self._by_id: dict[int, tuple[str, str]] = {}  # id -> (cleaned base, nick in use)
        self._used: dict[str, int] = {}  # lower-case nick -> id

    def nick(self, uid: int, name: str, console_nick: str) -> str:
        base = clean_nick(name, console_nick, self.limit)
        cur = self._by_id.get(uid)
        if cur and cur[0] == base and cur[1].lower() != console_nick.lower():
            return cur[1]
        if cur:
            self._used.pop(cur[1].lower(), None)
        cand, n = base, 1
        while cand.lower() in self._used or cand.lower() == console_nick.lower():
            n += 1
            sfx = str(n)
            cand = base[: self.limit - len(sfx)] + sfx
        self._used[cand.lower()] = uid
        self._by_id[uid] = (base, cand)
        return cand


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
    out = " ".join(p for p in (prefix.strip(), who + text) if p)
    return neutralize_mentions(out.strip())[:1900].strip()


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



# -------------------------------------------------------------- directory

@dataclass(frozen=True)
class ChannelInfo:
    id: int
    name: str  # the Discord name
    topic: str = ""
    viewers: frozenset[int] = frozenset()  # human members who can view it


@dataclass
class Snapshot:
    """What the Discord side knows about the guild, at one moment."""
    channels: list[ChannelInfo] = field(default_factory=list)
    members: dict[int, str] = field(default_factory=dict)  # humans: id -> display name
    complete: bool = True  # False: the member list is not available


def irc_topic(ch: ChannelInfo) -> str:
    return clean_text(ch.topic)[:200] or clean_text(f"Discord #{ch.name}")


def derive_names(chans: Iterable[tuple[int, str]], default_id: int | None, alias: str,
                 held: dict[int, str]) -> dict[int, str]:
    """IRC name per channel id. Held names stay, then the alias, then the rest by id."""
    chans = sorted(chans)
    ids = {cid for cid, _ in chans}
    out: dict[int, str] = {}
    used: set[str] = set()

    def take(cid: int, name: str) -> None:
        out[cid] = name
        used.add(name.lower())

    for cid in sorted(held):
        if cid in ids:
            take(cid, held[cid])
    if alias and default_id in ids and default_id not in out and alias.lower() not in used:
        take(default_id, alias)  # type: ignore[arg-type]
    for cid, name in chans:
        if cid in out:
            continue
        base = channel_slug(name)
        cand, n = "#" + base, 1
        while cand.lower() in used:
            n += 1
            sfx = str(n)
            cand = "#" + base[: CHAN_MAX - len(sfx)] + sfx
        take(cid, cand)
    return out


@dataclass
class DirChange:
    lost: list[int] = field(default_factory=list)  # held channels that are gone
    renamed: list[tuple[int, str, str]] = field(default_factory=list)  # id, name kept, new name


class Directory:
    """The channels that are bridged, and the IRC name of each."""

    def __init__(self, default_id: int | None = None, alias: str = "",
                 allow: frozenset[int] = frozenset()):
        self.default_id, self.alias, self.allow = default_id, alias, allow
        self.chans: dict[int, ChannelInfo] = {}
        self._names: dict[int, str] = {}
        self._ids: dict[str, int] = {}
        self._told: dict[int, str] = {}

    def update(self, chans: Iterable[ChannelInfo], held: dict[int, str]) -> DirChange:
        self.chans = {c.id: c for c in chans if not self.allow or c.id in self.allow}
        return self.rebuild(held)

    def rebuild(self, held: dict[int, str]) -> DirChange:
        """Recompute the names. A channel the console is in keeps the name it joined by."""
        pairs = [(c.id, c.name) for c in self.chans.values()]
        keep = {cid: n for cid, n in held.items() if cid in self.chans}
        want = derive_names(pairs, self.default_id, self.alias, {})
        self._names = derive_names(pairs, self.default_id, self.alias, keep)
        self._ids = {n.lower(): cid for cid, n in self._names.items()}
        ch = DirChange(lost=sorted(cid for cid in held if cid not in self.chans))
        for cid, old in sorted(keep.items()):
            if want[cid].lower() != old.lower() and self._told.get(cid) != want[cid]:
                self._told[cid] = want[cid]
                ch.renamed.append((cid, old, want[cid]))
        for cid in list(self._told):
            if cid not in keep:
                del self._told[cid]
        return ch

    def id_of(self, name: str) -> int | None:
        return self._ids.get(name.lower())

    def name_of(self, cid: int) -> str | None:
        return self._names.get(cid)

    def default_name(self) -> str | None:
        return self._names.get(self.default_id) if self.default_id is not None else None

    def listing(self) -> list[tuple[str, ChannelInfo]]:
        return sorted((n, self.chans[cid]) for cid, n in self._names.items())


# ------------------------------------------------------------------ names

class Roster:
    """Who is in each channel, as nicks, in the order the console should see them."""

    def __init__(self, directory: Directory, nicks: NickBook):
        self.directory, self.nicks = directory, nicks
        self.members: dict[int, str] = {}
        self.complete = False
        self.speakers: dict[int, dict[int, str]] = {}  # channel -> id -> name, for the fallback
        self._spoke: dict[tuple[int, int], int] = {}
        self._seq = 0

    def apply(self, snap: Snapshot, console_nick: str) -> None:
        self.members, self.complete = dict(snap.members), snap.complete
        for uid in sorted(self.members):  # same nicks on every run
            self.nicks.nick(uid, self.members[uid], console_nick)

    def speak(self, channel_id: int, uid: int, name: str) -> None:
        self._seq += 1
        self._spoke[(channel_id, uid)] = self._seq
        self.speakers.setdefault(channel_id, {})[uid] = name

    def visible(self, channel_id: int) -> dict[int, str]:
        if not self.complete:
            return self.speakers.get(channel_id, {})
        ch = self.directory.chans.get(channel_id)
        if ch is None:
            return {}
        return {u: self.members[u] for u in ch.viewers if u in self.members}

    def count(self, channel_id: int) -> int:
        return len(self.visible(channel_id))

    def names(self, channel_id: int, console_nick: str, limit: int) -> list[str]:
        """The console first, then who spoke last, then the rest A to Z; at most `limit`."""
        rows = []
        for uid, name in self.visible(channel_id).items():
            seq = self._spoke.get((channel_id, uid), 0)
            nick = self.nicks.nick(uid, name, console_nick)
            rows.append(((0, -seq, "") if seq else (1, 0, nick.lower()), nick))
        rows.sort()
        return [console_nick] + [n for _, n in rows[: max(limit - 1, 0)]]


class PushPolicy:
    """When a fresh names list may be pushed: soon if the people changed, rarely if only the order."""

    def __init__(self, interval: float = 3.0, order_interval: float = 60.0):
        self.interval, self.order_interval = interval, order_interval
        self._sent: dict[int, tuple[list[str], float]] = {}

    def sent(self, channel_id: int, names: list[str], now: float) -> None:
        self._sent[channel_id] = (names[1:], now)  # the console's own nick is not a change

    def forget(self, channel_id: int) -> None:
        self._sent.pop(channel_id, None)

    def changed(self, channel_id: int, names: list[str]) -> bool:
        prev = self._sent.get(channel_id)
        return prev is not None and prev[0] != names[1:]

    def due(self, channel_id: int, names: list[str], now: float) -> bool:
        prev = self._sent.get(channel_id)
        if prev is None or prev[0] == names[1:]:
            return False
        wait = self.order_interval if sorted(prev[0]) == sorted(names[1:]) else self.interval
        return now - prev[1] >= wait


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
    author_id: int = 0


@dataclass
class RelayConfig:
    max_chars: int = 96
    nick_max: int = NICK_MAX
    interval: float = 1.5
    queue_size: int = 20
    mute_file: str = "MUTE"
    allow_role: int | None = None
    prefix: str = ""


@dataclass
class Relay:
    cfg: RelayConfig
    clock: Callable[[], float]
    blocklist: Blocklist = field(default_factory=Blocklist)
    console_nick: str = DEFAULT_CONSOLE_NICK
    console_ready: bool = False
    joined: set[int] = field(default_factory=set)  # channels the console is in
    muted: Callable[[], bool] | None = None
    nicks: NickBook | None = None
    queue: Deque[tuple[str, str, int]] = field(default_factory=deque)  # nick, text, channel id
    dropped: Counter[str] = field(default_factory=Counter)
    _next_at: float = 0.0

    def __post_init__(self) -> None:
        if self.nicks is None:
            self.nicks = NickBook(self.cfg.nick_max)

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
        assert self.nicks is not None
        nick = self.nicks.nick(m.author_id, m.display_name, self.console_nick)
        text = cut(clean_text(m.content), self.cfg.max_chars)
        tags = self.tags(m)
        return nick, (text + " " + tags).strip()

    def from_discord(self, m: DiscordMsg) -> str | None:
        """Queue a message. Returns the drop reason, or None when queued."""
        if m.is_self or m.is_bot:
            return "bot"
        if m.channel_id not in self.joined:
            self.dropped["not joined"] += 1
            return "not joined"
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
            log.info("drop (queue full) oldest: %s: %s", old[0], old[1])
        self.queue.append((nick, text, m.channel_id))
        return None

    def pop_due(self) -> tuple[str, str, int] | None:
        """Next line to send now, or None. Muted or no console empties the queue."""
        if self.queue and (self.is_muted() or not self.console_ready):
            why = "muted" if self.is_muted() else "no console"
            for n, t, _ in self.queue:
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
