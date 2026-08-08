DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260808205000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260808205000');
-- Add your query below.


-- The persistent bot raid guild's roster. Rows here are authored, describing a character
-- that should exist; `guid` and `account` are filled in by `.raidguild provision` once it
-- does. A row with `guid` still zero has never been provisioned, which is the only state
-- the roster and the `characters` table are allowed to disagree in.
--
-- Deliberately not an overload of the existing `playerbot` table: that one has no write
-- path in code and is not consulted by the load path these characters are spawned through.
CREATE TABLE IF NOT EXISTS `raidguild_member` (
  `name` varchar(12) NOT NULL,
  `race` tinyint(3) unsigned NOT NULL,
  `class` tinyint(3) unsigned NOT NULL,
  `gender` tinyint(3) unsigned NOT NULL DEFAULT '0',
  -- CombatBotRoles from SharedDefines.h: 0 invalid, 1 melee dps, 2 ranged dps, 3 tank,
  -- 4 healer. Invalid means let the bot AI work the role out from the spell book.
  `role` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `spec` varchar(64) NOT NULL DEFAULT '',
  -- Which pool the member rolls in when loot is distributed, and where it sits in a raid.
  `loot_group` tinyint(3) unsigned NOT NULL DEFAULT '0',
  `subgroup` tinyint(3) unsigned NOT NULL DEFAULT '0',
  -- Zero until provisioned. The account is this roster's own and has no `realmd` row: the
  -- bot login path never consults one, and it exists only so that two roster members are
  -- never mistaken for the same account being online twice.
  `guid` int(10) unsigned NOT NULL DEFAULT '0',
  `account` int(10) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`name`),
  KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;


-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
