DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260905025801');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260905025801');
-- Add your query below.

-- Move the realm tables off MyISAM, for the reasons given in the characters migration of
-- the same date: MyISAM cannot recover from an unclean shutdown, and a crashed `account`
-- or `realmlist` table locks everyone out of the realm until someone repairs it by hand.

ALTER TABLE `account_banned` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `ip_banned` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `realmcharacters` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `realmlist` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
ALTER TABLE `uptime` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Left for last: this is the table the surrounding procedure just wrote to.
ALTER TABLE `migrations` ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
