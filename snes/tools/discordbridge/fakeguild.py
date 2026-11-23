"""A small fake Discord server for --dry-run and the tests: no discord import, no network."""
from __future__ import annotations

from typing import Callable

from core import ChannelInfo, DiscordMsg, Snapshot

DEFAULT_GUILD = """\
# channel <id> <name> [| topic]      member <id> <display name> [| channel ids that can see it]
channel 7 palestra | Canal da palestra
channel 8 geral | Conversa solta
channel 9 off-topic
channel 10 so-staff | Equipe
member 1 Ana
member 2 Bruno
member 3 Carla
member 4 Joao Silva | 7,8
member 5 Dani | 10
"""

HELP = """\
dry-run: type 'name: text' or '#channel name: text' to play a Discord message
  /join NAME        a person joins the server
  /leave NAME       a person leaves
  /rename OLD > NEW a person changes display name
  /newchan NAME [| topic]   /delchan NAME   /renchan OLD > NEW
  /hide NAME #chan  /show NAME #chan   take a person out of / put a person back in a channel
  /nomembers  /members   simulate the Server Members Intent missing / granted
  /help"""


class FakeGuild:
    def __init__(self, text: str = DEFAULT_GUILD):
        self.channels: dict[int, tuple[str, str]] = {}  # id -> name, topic
        self.members: dict[int, str] = {}
        self.only: dict[int, set[int]] = {}  # member -> the channels it can see; absent: all
        self.complete = True
        self.default_id = 0
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            kind, _, rest = line.partition(" ")
            head, _, tail = rest.partition("|")
            num, _, name = head.strip().partition(" ")
            if kind == "channel" and num.isdigit() and name.strip():
                self.channels[int(num)] = (name.strip(), tail.strip())
            elif kind == "member" and num.isdigit() and name.strip():
                self.members[int(num)] = name.strip()
                if tail.strip():
                    self.only[int(num)] = {int(x) for x in tail.split(",") if x.strip().isdigit()}
            else:
                raise ValueError(f"fake guild: cannot read {raw!r}")
        if not self.channels:
            raise ValueError("fake guild: no channel")
        self.default_id = min(self.channels)

    # ---- state
    def snapshot(self) -> Snapshot:
        chans = []
        for cid, (name, topic) in sorted(self.channels.items()):
            see = frozenset(m for m in self.members if m not in self.only or cid in self.only[m])
            chans.append(ChannelInfo(cid, name, topic, see if self.complete else frozenset()))
        return Snapshot(chans, dict(self.members) if self.complete else {}, self.complete)

    def channel_id(self, ref: str) -> int | None:
        ref = ref.strip().lstrip("#").lower()
        for cid, (name, _) in self.channels.items():
            if ref == str(cid) or ref == name.lower():
                return cid
        return None

    def member_id(self, ref: str, create: bool = False) -> int | None:
        ref = ref.strip()
        for mid, name in self.members.items():
            if ref == str(mid) or ref.lower() == name.lower():
                return mid
        if not create or not ref:
            return None
        mid = max(self.members, default=0) + 1
        self.members[mid] = ref
        return mid

    def message(self, line: str, by_irc_name: Callable[[str], int | None] = lambda n: None) -> tuple[DiscordMsg | None, bool]:
        """Parse '[#channel ]name: text'. Returns (message, a new person was created)."""
        cid = self.default_id
        if line.startswith("#"):
            tok, _, line = line.partition(" ")
            found = self.channel_id(tok)
            if found is None:
                found = by_irc_name(tok)
            if found is None:
                return None, False
            cid = found
        name, sep, text = line.partition(":")
        if not sep or not name.strip():
            return None, False
        known = self.member_id(name) is not None
        mid = self.member_id(name, create=True)
        assert mid is not None
        return DiscordMsg(cid, self.members[mid], text.strip(), author_id=mid), not known

    # ---- commands; each returns a reply text, or None when the guild changed
    def command(self, line: str) -> str | None:
        cmd, _, arg = line.strip().partition(" ")
        arg = arg.strip()
        old, _, new = arg.partition(">")
        old, new = old.strip(), new.strip()
        if cmd == "/join" and arg:
            if self.member_id(arg) is not None:
                return f"{arg} is already here"
            self.member_id(arg, create=True)
        elif cmd == "/leave" and arg:
            mid = self.member_id(arg)
            if mid is None:
                return f"no member {arg}"
            del self.members[mid]
        elif cmd == "/rename" and old and new:
            mid = self.member_id(old)
            if mid is None:
                return f"no member {old}"
            self.members[mid] = new
        elif cmd == "/newchan" and arg:
            name, _, topic = arg.partition("|")
            self.channels[max(self.channels) + 1] = (name.strip(), topic.strip())
        elif cmd == "/delchan" and arg:
            cid = self.channel_id(arg)
            if cid is None or len(self.channels) == 1:
                return f"cannot delete {arg}"
            del self.channels[cid]
        elif cmd == "/renchan" and old and new:
            cid = self.channel_id(old)
            if cid is None:
                return f"no channel {old}"
            self.channels[cid] = (new, self.channels[cid][1])
        elif cmd in ("/hide", "/show") and arg:
            who, _, chan = arg.rpartition(" ")
            mid, cid = self.member_id(who), self.channel_id(chan)
            if mid is None or cid is None:
                return "usage: /hide NAME #channel"
            seen = self.only.setdefault(mid, set(self.channels))
            (seen.discard if cmd == "/hide" else seen.add)(cid)
        elif cmd in ("/nomembers", "/members"):
            self.complete = cmd == "/members"
        else:
            return HELP
        return None
