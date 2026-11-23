#!/usr/bin/env python3
"""discordbridge: one Discord channel <-> the SNOS IRC client, through a one-client IRC server."""
from __future__ import annotations

import argparse
import asyncio
import io
import logging
import os
import sys
import time
from dataclasses import dataclass
from typing import Awaitable, Callable, Optional

from core import (Blocklist, ChannelInfo, Directory, DiscordMsg, NickBook, PushPolicy, Relay,
                  RelayConfig, Roster, Snapshot, clean_text, irc_privmsg, log, DEFAULT_CONSOLE_NICK)
from fakeguild import DEFAULT_GUILD, HELP, FakeGuild
from ircd import IrcServer, valid_channel

DISCORD_QUEUE = 5  # posts waiting for Discord; more are dropped
STATS_EVERY = 60.0  # seconds between the "dropped" log lines
NICK_LIMIT = 16  # irc.c keeps 16 characters per nick in its user list


@dataclass
class Settings:
    token: str = ""
    channel_id: Optional[int] = None
    irc_channel: str = ""  # optional alias for the default channel
    allow_channels: frozenset[int] = frozenset()
    bind: str = "0.0.0.0"
    port: int = 6667
    max_chars: int = 96
    nick_max: int = 15
    interval: float = 1.5
    queue_size: int = 20
    mute_file: str = "MUTE"
    blocklist: str = "blocklist.txt"
    allow_role: Optional[int] = None
    prefix: str = "**[SNES]**"
    names_max: int = 48
    names_interval: float = 3.0
    names_order_interval: float = 60.0
    fake_guild: Optional[str] = None

    def __repr__(self) -> str:  # never print the token
        return "Settings(...)"


def load_env_file(path: str) -> None:
    """KEY=VALUE lines into os.environ; real environment variables win."""
    try:
        with open(path, encoding="utf-8") as f:
            for raw in f:
                raw = raw.strip()
                if not raw or raw.startswith("#") or "=" not in raw:
                    continue
                k, v = raw.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip().strip("\"'"))
    except OSError:
        pass


def settings_from_env(env: dict[str, str], dry_run: bool = False) -> Settings:
    g = lambda k, d="": env.get("DISCORDBRIDGE_" + k, d).strip()
    s = Settings(
        irc_channel=g("IRC_CHANNEL"),
        bind=g("BIND", "0.0.0.0"),
        port=int(g("PORT", "6667")),
        max_chars=int(g("MAX_CHARS", "96")),
        nick_max=int(g("NICK_MAX", "15")),
        interval=float(g("INTERVAL", "1.5")),
        queue_size=int(g("QUEUE", "20")),
        mute_file=g("MUTE_FILE", "MUTE"),
        blocklist=g("BLOCKLIST", "blocklist.txt"),
        prefix=g("PREFIX", "**[SNES]**"),
        names_max=int(g("NAMES_MAX", "48")),
        names_interval=float(g("NAMES_INTERVAL", "3")),
        names_order_interval=float(g("NAMES_ORDER_INTERVAL", "60")),
    )
    if g("ALLOW_ROLE"):
        s.allow_role = int(g("ALLOW_ROLE"))
    if g("CHANNEL_ID"):
        s.channel_id = int(g("CHANNEL_ID"))
    if g("ALLOW_CHANNELS"):
        s.allow_channels = frozenset(int(x) for x in g("ALLOW_CHANNELS").replace(" ", "").split(",") if x)
    s.token = g("TOKEN")
    tf = g("TOKEN_FILE", "token.txt")
    if not s.token and os.path.exists(tf):
        with open(tf, encoding="utf-8") as f:
            s.token = f.read().strip()
    if s.irc_channel and not valid_channel(s.irc_channel):
        raise SystemExit("DISCORDBRIDGE_IRC_CHANNEL must look like #name (up to 24 characters, no spaces)")
    if not 3 <= s.nick_max <= NICK_LIMIT:
        raise SystemExit(f"DISCORDBRIDGE_NICK_MAX must be between 3 and {NICK_LIMIT}")
    if s.names_max < 1:
        raise SystemExit("DISCORDBRIDGE_NAMES_MAX must be at least 1")
    if not dry_run:
        if not s.token:
            raise SystemExit("no token: set DISCORDBRIDGE_TOKEN or put it in token.txt")
        if s.channel_id is None:
            raise SystemExit("DISCORDBRIDGE_CHANNEL_ID is not set")
    return s


class Bridge:
    """Wires a Discord side (anything with async post(channel_id, text)) to the core and the IRC server."""

    def __init__(self, s: Settings, post: Callable[[int, str], Awaitable[None]],
                 clock: Callable[[], float] = time.monotonic):
        self.s = s
        self.post = post
        self.clock = clock
        self.nicks = NickBook(s.nick_max)
        self.relay = Relay(
            RelayConfig(s.max_chars, s.nick_max, s.interval, s.queue_size, s.mute_file,
                        s.allow_role, s.prefix),
            clock, Blocklist(s.blocklist), DEFAULT_CONSOLE_NICK, nicks=self.nicks)
        self.directory = Directory(s.channel_id, s.irc_channel, s.allow_channels)
        if s.channel_id is not None and s.irc_channel:  # joinable before Discord is ready
            self.directory.update([ChannelInfo(s.channel_id, s.irc_channel.lstrip("#"))], {})
        self.roster = Roster(self.directory, self.nicks)
        self.policy = PushPolicy(s.names_interval, s.names_order_interval)
        self.ircd = IrcServer(
            self.directory, names=self._names_for, count=self.roster.count,
            on_message=self._from_console, on_nick=self._on_nick,
            on_joined=self._on_joined, on_connection=self._on_connection)
        self._out: asyncio.Queue[tuple[int, str]] = asyncio.Queue()
        self._tasks: list[asyncio.Task] = []
        self._names_dirty = False
        self._mapping: list[tuple[str, str]] = []
        self.port = 0

    # ---- names
    def _names_for(self, channel_id: int, nick: str) -> list[str]:
        names = self.roster.names(channel_id, nick, self.s.names_max)
        self.policy.sent(channel_id, names, self.clock())
        return names

    def names_tick(self) -> None:
        """Push a fresh names list to each joined channel whose list changed, rate limited."""
        if not self._names_dirty:
            return
        self._names_dirty = False
        now = self.clock()
        for cid in sorted(self.relay.joined):
            names = self.roster.names(cid, self.relay.console_nick, self.s.names_max)
            if self.policy.due(cid, names, now):
                self.ircd.push_names(cid)
            elif self.policy.changed(cid, names):
                self._names_dirty = True  # changed, but too soon: look again later

    # ---- console events
    def _on_nick(self, nick: str) -> None:
        self.relay.console_nick = nick

    def _on_joined(self, ids: frozenset[int], nick: str) -> None:
        for cid in self.relay.joined - ids:
            self.policy.forget(cid)
        changed = self.relay.joined != ids
        self.relay.joined = set(ids)
        self.relay.console_ready = bool(ids)
        if nick:
            self.relay.console_nick = nick
        if changed:
            self.directory.rebuild(self.ircd.held())  # a part frees the old name of a renamed channel

    def _on_connection(self, up: bool, addr: str) -> None:
        log.info("console %s %s", "connected" if up else "disconnected", addr)

    def _from_console(self, channel_id: int, nick: str, text: str, action: bool) -> None:
        self.relay.console_nick = nick
        out = self.relay.from_console(text, action)
        if out is None:
            log.info("drop (muted) console: %s", text)
            return
        if self._out.qsize() >= DISCORD_QUEUE:
            log.warning("drop (discord backlog) console: %s", text)
            return
        log.info("C->D %s %s", self.directory.name_of(channel_id), out)
        self._out.put_nowait((channel_id, out))

    # ---- discord events
    def sync(self, snap: Snapshot) -> None:
        """The guild changed: new channels, names, permissions or members."""
        self.roster.apply(snap, self.relay.console_nick)
        change = self.directory.update(snap.channels, self.ircd.held())
        for cid in change.lost:
            self.ircd.kick(cid, "channel removed or hidden on Discord")
        for _, old, new in change.renamed:
            self.ircd.notice(f"{old} is now {new} on Discord; you keep {old} until you part")
        names = {c.id: clean_text(c.name) for c in self.directory.chans.values()}
        mapping = [(n, names[c.id]) for n, c in self.directory.listing()]
        if mapping != self._mapping:
            self._mapping = mapping
            log.info("bridged channels: %s", ", ".join(f"{n}={d}" for n, d in mapping) or "(none)")
        self._names_dirty = True

    def on_discord_message(self, m: DiscordMsg) -> None:
        if m.author_id and not (m.is_bot or m.is_self) and m.channel_id in self.directory.chans:
            self.roster.speak(m.channel_id, m.author_id, m.display_name)
            self._names_dirty = True
        reason = self.relay.from_discord(m)
        if reason is None:
            return
        # bots and channels the console is not in are routine; they are counted, not logged
        if reason in ("bot", "not joined"):
            log.debug("drop (%s) %s", reason, m.display_name)
        else:
            log.info("drop (%s) %s: %s", reason, m.display_name, clean_text(m.content)[:80])

    # ---- loops
    def pump_once(self) -> bool:
        item = self.relay.pop_due()
        if item is None:
            return False
        nick, text, cid = item
        chan = self.directory.name_of(cid)
        if chan is None or not self.ircd.send_chat(cid, irc_privmsg(chan, nick, text)):
            log.info("drop (no console) %s: %s", nick, text)
            return False
        log.info("D->C %s %s: %s", chan, nick, text)
        return True

    async def _pump(self) -> None:
        while True:
            self.pump_once()
            await asyncio.sleep(0.05)

    async def _names_loop(self) -> None:
        while True:
            self.names_tick()
            await asyncio.sleep(min(0.5, self.s.names_interval / 2))

    async def _stats(self) -> None:
        while True:
            await asyncio.sleep(STATS_EVERY)
            self.log_stats()

    def log_stats(self) -> None:
        n = self.relay.dropped.pop("not joined", 0)
        if n:
            log.info("dropped %d messages from channels the console has not joined", n)

    async def _poster(self) -> None:
        while True:
            cid, text = await self._out.get()
            try:
                await asyncio.wait_for(self.post(cid, text), 20)
            except Exception as e:
                log.warning("discord send failed (%s): %s", type(e).__name__, e)

    async def start(self) -> int:
        self.port = await self.ircd.start(self.s.bind, self.s.port)
        log.info("IRC server listening on %s:%d", self.s.bind, self.port)
        self._tasks = [asyncio.create_task(c) for c in
                       (self._pump(), self._poster(), self._names_loop(), self._stats())]
        return self.port

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        await self.ircd.stop()


async def _dry_stdin(bridge: Bridge, guild: FakeGuild) -> None:
    loop = asyncio.get_running_loop()
    print(HELP, flush=True)
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            return
        line = line.rstrip("\r\n")
        if line.startswith("/"):
            reply = guild.command(line)
            if reply:
                print(reply, flush=True)
            else:
                bridge.sync(guild.snapshot())
            continue
        msg, created = guild.message(line, bridge.directory.id_of)
        if msg is None:
            print("format: [#channel ]name: text", flush=True)
            continue
        if created:
            bridge.sync(guild.snapshot())
        bridge.on_discord_message(msg)


async def run(s: Settings, dry_run: bool) -> None:
    if dry_run:
        with open(s.fake_guild, encoding="utf-8") if s.fake_guild else io.StringIO(DEFAULT_GUILD) as f:
            guild = FakeGuild(f.read())
        if s.channel_id is None:
            s.channel_id = guild.default_id
        guild.default_id = s.channel_id

        async def post(channel_id: int, text: str) -> None:
            name = guild.channels.get(channel_id, ("?", ""))[0]
            print(f"[dry-run] would post to Discord #{name}: {text}", flush=True)
        bridge = Bridge(s, post)
        await bridge.start()
        bridge.sync(guild.snapshot())
        try:
            await _dry_stdin(bridge, guild)
            await asyncio.Event().wait()  # stdin closed; keep serving the console
        finally:
            await bridge.stop()
        return
    from discord_side import DiscordSide  # lazy: needs discord.py
    side = DiscordSide(s)
    bridge = Bridge(s, side.send)
    side.bind(bridge)
    await bridge.start()
    try:
        await side.run(s.token)
    finally:
        await bridge.stop()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Discord server channels <-> SNOS IRC client")
    ap.add_argument("--dry-run", action="store_true", help="no Discord: a fake server; stdin plays messages and events")
    ap.add_argument("--fake-guild", metavar="FILE", help="with --dry-run: the fake server's channels and members")
    ap.add_argument("--env-file", default="discordbridge.env")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(stream=sys.stdout, level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    logging.getLogger("discord").setLevel(logging.WARNING)
    if a.fake_guild and not a.dry_run:
        ap.error("--fake-guild needs --dry-run")
    load_env_file(a.env_file)
    s = settings_from_env(dict(os.environ), a.dry_run)
    s.fake_guild = a.fake_guild
    try:
        asyncio.run(run(s, a.dry_run))
    except KeyboardInterrupt:
        log.info("bye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
