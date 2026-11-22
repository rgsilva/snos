#!/usr/bin/env python3
"""discordbridge: one Discord channel <-> the SNOS IRC client, through a one-client IRC server."""
from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
import time
from dataclasses import dataclass
from typing import Awaitable, Callable, Optional

from core import (Blocklist, DiscordMsg, Relay, RelayConfig, clean_text,
                  irc_privmsg, log, DEFAULT_CONSOLE_NICK)
from ircd import IrcServer, valid_channel

DISCORD_QUEUE = 5  # posts waiting for Discord; more are dropped


@dataclass
class Settings:
    token: str = ""
    channel_id: Optional[int] = None
    irc_channel: str = "#palestra"
    bind: str = "0.0.0.0"
    port: int = 6667
    max_chars: int = 160
    interval: float = 1.5
    queue_size: int = 20
    mute_file: str = "MUTE"
    blocklist: str = "blocklist.txt"
    allow_role: Optional[int] = None
    prefix: str = "**[SNES]**"

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
        irc_channel=g("IRC_CHANNEL", "#palestra"),
        bind=g("BIND", "0.0.0.0"),
        port=int(g("PORT", "6667")),
        max_chars=int(g("MAX_CHARS", "160")),
        interval=float(g("INTERVAL", "1.5")),
        queue_size=int(g("QUEUE", "20")),
        mute_file=g("MUTE_FILE", "MUTE"),
        blocklist=g("BLOCKLIST", "blocklist.txt"),
        prefix=g("PREFIX", "**[SNES]**"),
    )
    if g("ALLOW_ROLE"):
        s.allow_role = int(g("ALLOW_ROLE"))
    if g("CHANNEL_ID"):
        s.channel_id = int(g("CHANNEL_ID"))
    s.token = g("TOKEN")
    tf = g("TOKEN_FILE", "token.txt")
    if not s.token and os.path.exists(tf):
        with open(tf, encoding="utf-8") as f:
            s.token = f.read().strip()
    if not valid_channel(s.irc_channel):
        raise SystemExit("DISCORDBRIDGE_IRC_CHANNEL must look like #name (up to 24 characters, no spaces)")
    if not dry_run:
        if not s.token:
            raise SystemExit("no token: set DISCORDBRIDGE_TOKEN or put it in token.txt")
        if s.channel_id is None:
            raise SystemExit("DISCORDBRIDGE_CHANNEL_ID is not set")
    return s


class Bridge:
    """Wires a Discord side (anything with async send(text)) to the core and the IRC server."""

    def __init__(self, s: Settings, post: Callable[[str], Awaitable[None]],
                 clock: Callable[[], float] = time.monotonic):
        self.s = s
        self.post = post
        self.relay = Relay(
            RelayConfig(s.channel_id, s.irc_channel, s.max_chars, s.interval,
                        s.queue_size, s.mute_file, s.allow_role, s.prefix),
            clock, Blocklist(s.blocklist), DEFAULT_CONSOLE_NICK)
        self.ircd = IrcServer(
            s.irc_channel,
            on_message=self._from_console, on_nick=self._on_nick,
            on_ready=self._on_ready, on_connection=self._on_connection)
        self._out: asyncio.Queue[str] = asyncio.Queue()
        self._tasks: list[asyncio.Task] = []
        self.port = 0

    # ---- console events
    def _on_nick(self, nick: str) -> None:
        self.relay.console_nick = nick

    def _on_ready(self, ready: bool, nick: str) -> None:
        self.relay.console_ready = ready
        if nick:
            self.relay.console_nick = nick

    def _on_connection(self, up: bool, addr: str) -> None:
        log.info("console %s %s", "connected" if up else "disconnected", addr)

    def _from_console(self, nick: str, text: str, action: bool) -> None:
        self.relay.console_nick = nick
        out = self.relay.from_console(text, action)
        if out is None:
            log.info("drop (muted) console: %s", text)
            return
        if self._out.qsize() >= DISCORD_QUEUE:
            log.warning("drop (discord backlog) console: %s", text)
            return
        log.info("C->D %s", out)
        self._out.put_nowait(out)

    # ---- discord events
    def on_discord_message(self, m: DiscordMsg) -> None:
        reason = self.relay.from_discord(m)
        if reason is None:
            return
        # bots and other channels are routine; keep them out of the stage log
        if reason in ("bot", "other channel"):
            log.debug("drop (%s) %s", reason, m.display_name)
        else:
            log.info("drop (%s) %s: %s", reason, m.display_name, clean_text(m.content)[:80])

    # ---- loops
    def pump_once(self) -> bool:
        item = self.relay.pop_due()
        if item is None:
            return False
        nick, text = item
        if not self.ircd.send_chat(nick, irc_privmsg(self.s.irc_channel, nick, text)):
            log.info("drop (no console) %s: %s", nick, text)
            return False
        log.info("D->C %s: %s", nick, text)
        return True

    async def _pump(self) -> None:
        while True:
            self.pump_once()
            await asyncio.sleep(0.05)

    async def _poster(self) -> None:
        while True:
            text = await self._out.get()
            try:
                await asyncio.wait_for(self.post(text), 20)
            except Exception as e:
                log.warning("discord send failed (%s): %s", type(e).__name__, e)

    async def start(self) -> int:
        self.port = await self.ircd.start(self.s.bind, self.s.port)
        log.info("IRC server listening on %s:%d, channel %s", self.s.bind, self.port, self.s.irc_channel)
        self._tasks = [asyncio.create_task(self._pump()), asyncio.create_task(self._poster())]
        return self.port

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        await self.ircd.stop()


async def _dry_stdin(bridge: Bridge) -> None:
    loop = asyncio.get_running_loop()
    cid = bridge.s.channel_id
    log.info("dry-run: type 'name: text' to play a Discord message")
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            return
        name, sep, text = line.rstrip("\r\n").partition(":")
        if not sep:
            print("format: name: text")
            continue
        bridge.on_discord_message(DiscordMsg(cid, name.strip(), text.strip()))


async def run(s: Settings, dry_run: bool) -> None:
    if dry_run:
        async def post(text: str) -> None:
            print(f"[dry-run] would post to Discord: {text}", flush=True)
        bridge = Bridge(s, post)
        await bridge.start()
        try:
            await _dry_stdin(bridge)
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
    ap = argparse.ArgumentParser(description="Discord channel <-> SNOS IRC client")
    ap.add_argument("--dry-run", action="store_true", help="no Discord: stdin 'name: text' plays messages")
    ap.add_argument("--env-file", default="discordbridge.env")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(stream=sys.stdout, level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    logging.getLogger("discord").setLevel(logging.WARNING)
    load_env_file(a.env_file)
    s = settings_from_env(dict(os.environ), a.dry_run)
    try:
        asyncio.run(run(s, a.dry_run))
    except KeyboardInterrupt:
        log.info("bye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
