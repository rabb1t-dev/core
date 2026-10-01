INSERT INTO `migrations` VALUES ('20261001024500');

-- Let Moderator and above spawn and control party bots, without making them Administrators.
--
-- The whole `partybot` tree is compiled in at SEC_ADMINISTRATOR, and that level is a blunt
-- instrument to hand a friend: it also carries bans, kicks, `.server shutdown` and `.server
-- restart`. It does not carry account create, delete or password, which are SEC_CONSOLE and
-- unreachable from a game session at any gmlevel. So the only thing standing between "can use
-- bots" and "can turn the server off" was this table.
--
-- Done as data rather than as a source edit because `command` is the mechanism the server
-- already provides for exactly this, and `.reload command` applies it to a running world with
-- no rebuild and no restart. Note the reload is deferred: HandleReloadCommandCommand only sets
-- m_loadCommandTable, and getCommandTable() re-reads on the next chat command parsed.
--
-- Every subcommand is listed individually, for two reasons:
--
--   Naming the parent does not work. SetDataForCommandInTable resolves the name through
--   FindCommand, and a node that has children but no '' handler comes back as
--   CHAT_COMMAND_UNKNOWN_SUBCOMMAND, which is logged to DBErrors.log and skipped.
--
--   Naming the parent is also unnecessary. ExecuteCommand resolves the text to the leaf and
--   calls isAvailable on that leaf alone; FindCommand itself never consults isAvailable while
--   walking, so the parent's SEC_ADMINISTRATOR does not gate execution. The one visible
--   consequence is cosmetic: `.help` builds its list through ShowHelpForSubCommands, which does
--   test the parent, so a Moderator will not see `partybot` offered even though every
--   subcommand runs. Tell them the verbs rather than expecting discovery.
--
-- flags = 2 is COMMAND_FLAGS_CRITICAL, which logs the command and the caller's selection to
-- gm_critical.log once the handler succeeds. It is set on the verbs that create, destroy or
-- strip a bot, and left at 0 on the control verbs: hold, cometome, attackstart and the marking
-- commands are issued continuously during a fight and would bury the audit trail in noise.
--
-- Two deliberate consequences worth recording, because neither is obvious from this file:
--
--   Moderators get the unrestricted path. The "cannot add bots while dead / in combat / inside
--   instances" guards in PartyBotAddRequirementCheck are gated on GetAccessLevel() <=
--   SEC_PLAYER, so they apply to level 0 only. From level 1 up the behaviour is the same one an
--   Administrator already has, which is the point: a dungeon is where a bot group is useful.
--   The hard guards above that check still apply to everyone: no bots while taxi flying, none
--   inside a battleground (it crashes the server on BG end), MAX_RAID_SIZE, and instance caps.
--
--   PartyBot.MaxBots stays at 0. It is global rather than per security level, so any cap set to
--   restrain a guest would equally cap the operator's own full-size raid tests.
--
-- To revert, set security back to 6 rather than deleting these rows. getCommandTable() applies
-- overrides onto the compiled table in place and never restores a default, so a deleted row
-- leaves the lowered level live in memory until the world restarts.

REPLACE INTO `command` (`name`, `security`, `help`, `flags`) VALUES
  ('partybot add',         1, '', 2),
  ('partybot clone',       1, '', 2),
  ('partybot load',        1, '', 2),
  ('partybot remove',      1, '', 2),
  ('partybot removeall',   1, '', 2),
  ('partybot unequip',     1, '', 2),
  ('partybot setrole',     1, '', 0),
  ('partybot attackstart', 1, '', 0),
  ('partybot attackstop',  1, '', 0),
  ('partybot pull',        1, '', 0),
  ('partybot setpuller',   1, '', 0),
  ('partybot hold',        1, '', 0),
  ('partybot release',     1, '', 0),
  ('partybot aoe',         1, '', 0),
  ('partybot caststart',   1, '', 0),
  ('partybot caststop',    1, '', 0),
  ('partybot ccmark',      1, '', 0),
  ('partybot focusmark',   1, '', 0),
  ('partybot clearmarks',  1, '', 0),
  ('partybot cometome',    1, '', 0),
  ('partybot usegobject',  1, '', 0),
  ('partybot pause',       1, '', 0),
  ('partybot unpause',     1, '', 0);
