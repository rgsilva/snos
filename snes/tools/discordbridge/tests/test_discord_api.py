"""The discord.py names the adapter relies on exist with the shapes it assumes. Skipped without the library."""
import _setup  # noqa
import inspect
import unittest

try:
    import discord
except ImportError:
    discord = None


@unittest.skipIf(discord is None, "discord.py is not installed")
class DiscordApi(unittest.TestCase):
    def test_intents(self):
        i = discord.Intents.default()
        i.members = True
        i.message_content = True
        self.assertTrue(i.members and i.message_content)

    def test_guild_and_channel(self):
        for cls, names in ((discord.Guild, ("chunked", "text_channels", "me", "members")),
                           (discord.TextChannel, ("members", "topic", "name", "guild")),
                           (discord.User, ("global_name", "name")),
                           (discord.Role, ("permissions", "guild"))):
            for n in names:
                self.assertTrue(hasattr(cls, n), f"{cls.__name__}.{n}")
        self.assertEqual(list(inspect.signature(discord.Guild.chunk).parameters), ["self", "cache"])
        self.assertEqual(list(inspect.signature(discord.TextChannel.permissions_for).parameters), ["self", "obj"])
        self.assertEqual(list(inspect.signature(discord.TextChannel.is_news).parameters), ["self"])
        p = discord.Permissions
        self.assertTrue(hasattr(p, "view_channel") and hasattr(p, "send_messages"))
        self.assertTrue(issubclass(discord.PrivilegedIntentsRequired, Exception))

    def test_adapter_builds(self):
        from discord_side import DiscordSide
        from discordbridge import Settings
        side = DiscordSide(Settings(channel_id=1))
        self.assertTrue(side.client.intents.members)
        self.assertTrue(side.client.intents.message_content)
        for name in ("on_ready", "on_message", "on_member_join", "on_member_remove", "on_member_update",
                     "on_user_update", "on_guild_channel_create", "on_guild_channel_update",
                     "on_guild_channel_delete", "on_guild_role_update", "on_guild_role_delete"):
            self.assertTrue(inspect.iscoroutinefunction(getattr(side.client, name)), name)
        self.assertIsNone(side.snapshot())  # no guild cached yet
        self.assertFalse(side._make_client(members=False).intents.members)

    def test_event_parameters(self):
        """discord.py calls the handlers with these arguments (see the event reference)."""
        from discord_side import DiscordSide
        want = {"on_member_join": 1, "on_member_remove": 1, "on_member_update": 2, "on_user_update": 2,
                "on_guild_channel_create": 1, "on_guild_channel_update": 2, "on_guild_channel_delete": 1,
                "on_guild_role_update": 2, "on_guild_role_delete": 1, "on_message": 1, "on_ready": 0}
        for name, n in want.items():
            params = list(inspect.signature(getattr(DiscordSide, name)).parameters)
            self.assertEqual(len(params) - 1, n, name)
