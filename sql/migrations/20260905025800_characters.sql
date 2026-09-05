DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260905025800');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260905025800');
-- Add your query below.

-- Move every character table off MyISAM.
--
-- MyISAM has no crash recovery. A host that stops without shutting the database down
-- cleanly leaves any table that was open marked as crashed, and every statement against
-- it then fails with error 1194 until someone runs REPAIR TABLE by hand. That is not a
-- theoretical risk here: `character_queststatus` crashed exactly this way, and because
-- the login path treats a failed quest query as "this character has no quests", the only
-- symptom was quests silently vanishing across a logout.
--
-- MyISAM also ignores transactions. `Player::SaveToDB` wraps the character row and all of
-- its satellite tables in one, so on MyISAM a save that fails partway through leaves the
-- character half written with no rollback.
--
-- ROW_FORMAT is set explicitly because a few of these shipped as ROW_FORMAT=FIXED, which
-- InnoDB rejects.

ALTER TABLE `auction` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `characters` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `characters_guid_delete` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `characters_item_delete` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_action` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_battleground_data` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_duplicate_account` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_forgotten_skills` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_gifts` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_homebind` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_honor_cp` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_instance` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_inventory` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_pet` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_queststatus` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_reputation` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_skills` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_social` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_spell` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_spell_cooldown` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_stats` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `character_tutorial` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `corpse` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `game_event_status` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `gm_subsurveys` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `gm_surveys` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `gm_tickets` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `groups` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `group_instance` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `group_member` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `guild` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `guild_eventlog` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `guild_member` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `guild_rank` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `instance` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `instance_reset` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `item_loot` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `item_text` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `mail_items` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `petition` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `petition_sign` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `pet_spell` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `pet_spell_cooldown` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `playerbot` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `saved_variables` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `world` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `worldstates` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Left for last: this is the table the surrounding procedure just wrote to.
ALTER TABLE `migrations` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
