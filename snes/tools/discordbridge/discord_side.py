"""The only module that imports discord.py. Thin: converts events, posts text."""
from __future__ import annotations

import logging

import discord  # noqa: imported only when running for real

from core import DiscordMsg

log = logging.getLogger("discordbridge")


class DiscordSide:
    def __init__(self, settings):
        self.s = settings
        intents = discord.Intents.default()
        intents.message_content = True  # privileged: also enable it in the developer portal
        self.client = discord.Client(intents=intents)
        self.bridge = None
        self.client.event(self.on_ready)
        self.client.event(self.on_message)

    def bind(self, bridge) -> None:
        self.bridge = bridge

    async def run(self, token: str) -> None:
        async with self.client:
            await self.client.start(token)  # reconnects on its own

    async def on_ready(self) -> None:
        ch = self.client.get_channel(self.s.channel_id)
        if ch is None:
            log.warning("Discord ready, but channel %s is not visible to the bot", self.s.channel_id)
            return
        guild = getattr(ch, "guild", None)
        log.info("Discord ready: server %r, channel #%s", getattr(guild, "name", "?"), ch.name)
        topic = f"Discord: {getattr(guild, 'name', '')} #{ch.name}"
        from core import clean_text
        self.bridge.ircd.topic = clean_text(topic)[:200]

    async def on_message(self, message: "discord.Message") -> None:
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
        ))

    async def send(self, text: str) -> None:
        ch = self.client.get_channel(self.s.channel_id)
        if ch is None:
            ch = await self.client.fetch_channel(self.s.channel_id)
        await ch.send(text, allowed_mentions=discord.AllowedMentions.none())
