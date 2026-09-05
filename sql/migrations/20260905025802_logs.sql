DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260905025802');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260905025802');
-- Add your query below.

-- Move the log tables off MyISAM, for the reasons given in the characters migration of
-- the same date. Nothing here is worth recovering on its own, but a crashed table still
-- fails every write against it for as long as the server is up.

ALTER TABLE `instance_creature_kills` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `instance_custom_counters` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `instance_wipes` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `logs_trashcharacters` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `logs_warden` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Left for last: this is the table the surrounding procedure just wrote to.
ALTER TABLE `migrations` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
