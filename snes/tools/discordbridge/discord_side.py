"""The only module that imports discord.py. Thin: converts events to core types, posts text."""
from __future__ import annotations

import asyncio
import logging
from typing import Optional

import discord  # noqa: imported only when running for real

from core import ChannelInfo, DiscordMsg, Snapshot

log = logging.getLogger("discordbridge")

SYNC_DELAY = 1.0  # seconds; a burst of events becomes one snapshot

NO_MEMBERS = ("Server Members Intent is off: the names list is only people who spoke since the "
              "bridge started. Turn it on in the developer portal: Bot > Privileged Gateway Intents")


class DiscordSide:
    def __init__(self, settings):
        self.s = settings
        self.bridge = None
        self.guild_id: Optional[int] = None
        self.members_ok = True  # the Server Members Intent is granted and the cache is filled
        self._sync_task: Optional[asyncio.Task] = None
        self.client = self._make_client(members=True)

    def _make_client(self, members: bool) -> discord.Client:
        intents = discord.Intents.default()
        intents.message_content = True  # privileged: also enable it in the developer portal
        intents.members = members  # privileged too
        client = discord.Client(intents=intents)
        for name in ("on_ready", "on_message", "on_member_join", "on_member_remove",
                     "on_member_update", "on_user_update", "on_guild_channel_create",
                     "on_guild_channel_update", "on_guild_channel_delete",
                     "on_guild_role_update", "on_guild_role_delete"):
            client.event(getattr(self, name))
        return client

    def bind(self, bridge) -> None:
        self.bridge = bridge

    async def run(self, token: str) -> None:
        try:
            await self._run(token)
        except discord.PrivilegedIntentsRequired:
            log.warning(NO_MEMBERS)  # a second refusal below means Message Content is the one off
            self.members_ok = False
            self.client = self._make_client(members=False)
            await self._run(token)

    async def _run(self, token: str) -> None:
        async with self.client:
            await self.client.start(token)  # reconnects on its own

    # ---- snapshot of the guild
    def snapshot(self) -> Optional[Snapshot]:
        guild = self.client.get_guild(self.guild_id) if self.guild_id else None
        if guild is None or guild.me is None:
            return None  # not cached right now; keep what the bridge has
        complete = self.members_ok and guild.chunked
        chans = []
        for ch in guild.text_channels:  # not voice, forum, category or threads
            if ch.is_news():
                continue
            if self.s.allow_channels and ch.id not in self.s.allow_channels:
                continue
            p = ch.permissions_for(guild.me)
            if not (p.view_channel and p.send_messages):
                continue
            viewers = frozenset(m.id for m in ch.members if not m.bot) if complete else frozenset()
            chans.append(ChannelInfo(ch.id, ch.name, ch.topic or "", viewers))
        members = {m.id: m.display_name for m in guild.members if not m.bot} if complete else {}
        return Snapshot(chans, members, complete)

    def _touch(self, guild: Optional[discord.Guild]) -> None:
        """Something that can change the channels or the names happened; snapshot soon."""
        if self.bridge is None or self.guild_id is None:
            return
        if guild is not None and guild.id != self.guild_id:
            return
        if self._sync_task is None or self._sync_task.done():
            self._sync_task = asyncio.get_running_loop().create_task(self._sync_soon())

    async def _sync_soon(self) -> None:
        await asyncio.sleep(SYNC_DELAY)
        try:
            self._sync()
        except Exception as e:
            log.warning("snapshot failed (%s): %s", type(e).__name__, e)

    def _sync(self) -> None:
        snap = self.snapshot()
        if snap is None:
            return
        if not snap.complete and self.members_ok:
            self.members_ok = False
            log.warning(NO_MEMBERS)
        self.bridge.sync(snap)

    # ---- events
    async def on_ready(self) -> None:
        ch = self.client.get_channel(self.s.channel_id)
        if ch is None:
            log.warning("Discord ready, but channel %s is not visible to the bot", self.s.channel_id)
            return
        guild = ch.guild
        self.guild_id = guild.id
        if self.members_ok and not guild.chunked:
            try:
                await guild.chunk()
            except Exception as e:
                log.warning("could not load the member list (%s): %s", type(e).__name__, e)
                self.members_ok = False
        log.info("Discord ready: server %r", guild.name)
        self._sync()

    async def on_member_join(self, member: discord.Member) -> None:
        self._touch(member.guild)

    async def on_member_remove(self, member: discord.Member) -> None:
        self._touch(member.guild)

    async def on_member_update(self, before: discord.Member, after: discord.Member) -> None:
        if before.roles != after.roles or before.display_name != after.display_name:
            self._touch(after.guild)

    async def on_user_update(self, before: discord.User, after: discord.User) -> None:
        if before.global_name != after.global_name or before.name != after.name:
            self._touch(None)

    async def on_guild_channel_create(self, channel) -> None:
        self._touch(channel.guild)

    async def on_guild_channel_update(self, before, after) -> None:
        self._touch(after.guild)

    async def on_guild_channel_delete(self, channel) -> None:
        self._touch(channel.guild)

    async def on_guild_role_update(self, before: discord.Role, after: discord.Role) -> None:
        if before.permissions != after.permissions:
            self._touch(after.guild)

    async def on_guild_role_delete(self, role: discord.Role) -> None:
        self._touch(role.guild)

    async def on_message(self, message: "discord.Message") -> None:
        if message.guild is None or message.guild.id != self.guild_id:
            return
        if message.type not in (discord.MessageType.default, discord.MessageType.reply):
            return
        a = message.author
        images = sum(1 for x in message.attachments if (x.content_type or "").startswith("image/"))
        self.bridge.on_discord_message(DiscordMsg(
            channel_id=message.channel.id,
            display_name=a.display_name,
            content=message.clean_content,
            is_bot=a.bot or message.webhook_id is not None,
            is_self=self.client.user is not None and a.id == self.client.user.id,
            role_ids=frozenset(r.id for r in getattr(a, "roles", [])),
            images=images,
            files=len(message.attachments) - images,
            stickers=len(message.stickers),
            embeds=len(message.embeds),
            author_id=a.id,
        ))

    async def send(self, channel_id: int, text: str) -> None:
        ch = self.client.get_channel(channel_id)
        if ch is None:
            ch = await self.client.fetch_channel(channel_id)
        await ch.send(text, allowed_mentions=discord.AllowedMentions.none())
