DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260809001000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260809001000');
-- Add your query below.


-- Per-class per-spec item scoring priorities. Tunable without a recompile. Spec rather
-- than role, because a fire and a frost mage weight hit differently and the roster already
-- records a spec per member.
CREATE TABLE IF NOT EXISTS `raidguild_stat_weight` (
  `class` tinyint(3) unsigned NOT NULL,
  `spec` varchar(64) NOT NULL,
  `avg_hit` float NOT NULL DEFAULT '0',
  `dps` float NOT NULL DEFAULT '0',
  `speed` float NOT NULL DEFAULT '0',
  `armor` float NOT NULL DEFAULT '0',
  `stam` float NOT NULL DEFAULT '0',
  `spi` float NOT NULL DEFAULT '0',
  `intellect` float NOT NULL DEFAULT '0',
  `str` float NOT NULL DEFAULT '0',
  `agi` float NOT NULL DEFAULT '0',
  `ap` float NOT NULL DEFAULT '0',
  `hit` float NOT NULL DEFAULT '0',
  `crit` float NOT NULL DEFAULT '0',
  `weapon_skill` float NOT NULL DEFAULT '0',
  `defense` float NOT NULL DEFAULT '0',
  `dodge` float NOT NULL DEFAULT '0',
  `parry` float NOT NULL DEFAULT '0',
  `block` float NOT NULL DEFAULT '0',
  `block_value` float NOT NULL DEFAULT '0',
  `ranged_ap` float NOT NULL DEFAULT '0',
  `spdmg` float NOT NULL DEFAULT '0',
  `sppen` float NOT NULL DEFAULT '0',
  `sphit` float NOT NULL DEFAULT '0',
  `spcrit` float NOT NULL DEFAULT '0',
  `spheal` float NOT NULL DEFAULT '0',
  `mp5` float NOT NULL DEFAULT '0',
  `fire_res` float NOT NULL DEFAULT '0',
  `nat_res` float NOT NULL DEFAULT '0',
  `frost_res` float NOT NULL DEFAULT '0',
  PRIMARY KEY (`class`, `spec`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- Column order matches the CREATE TABLE above.
-- Warrior rows are copied from Classic Gear Ranker's Instructions tab (Fury / Tank / PvP).
-- Everything else is authored to published vanilla priorities so the roster has a weight
-- the day scoring ships, rather than waiting for a theorycrafter per class.

INSERT INTO `raidguild_stat_weight`
(`class`,`spec`,`avg_hit`,`dps`,`speed`,`armor`,`stam`,`spi`,`intellect`,`str`,`agi`,`ap`,`hit`,`crit`,`weapon_skill`,`defense`,`dodge`,`parry`,`block`,`block_value`,`ranged_ap`,`spdmg`,`sppen`,`sphit`,`spcrit`,`spheal`,`mp5`,`fire_res`,`nat_res`,`frost_res`)
VALUES
-- Warrior (Classic Gear Ranker)
(1,'fury',             5.5, 14.784,  0,    0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55,     65,      36.84, 0,     0,  0,    0,   0,    0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),
(1,'fury-dw-pve',      5.5, 14.784,  0,    0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55,     65,      36.84, 0,     0,  0,    0,   0,    0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),
(1,'tank',             0,   11.2,   -10,   0.084,  4, 0, 0, 2.28, 2.552848485, 1, 41.66666667, 19.6969697, 17.12121212, 11.424, 28, 29.4, 4.2, 0.28, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0),
(1,'protection-pve',   0,   11.2,   -10,   0.084,  4, 0, 0, 2.28, 2.552848485, 1, 41.66666667, 19.6969697, 17.12121212, 11.424, 28, 29.4, 4.2, 0.28, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0),
(1,'pvp',              11,  7,       0,    0.021,  2, 0, 0, 5,    3.469651515, 2.5, 52.08333333, 61.5530303, 9.753787879, 2.534, 7, 7.35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
(1,'arms-fury-19-twink',5.5,14.784,  0,    0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55, 65, 36.84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),
(1,'arms-29-twink',    5.5, 14.784,  0,    0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55, 65, 36.84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),
(1,'arms-39-twink',    5.5, 14.784,  0,    0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55, 65, 36.84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),
(1,'arms-49-twink',    5.5, 14.784,  0,    0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55, 65, 36.84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),

-- Paladin
(2,'retribution-pve',  4, 12, 0, 0.01, 0.5, 0, 0.2, 5, 2.5, 2, 45, 55, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0.1, 0, 0),
(2,'protection-pve',   0, 8, -8, 0.08, 4, 0, 0.5, 2, 2, 1, 30, 15, 10, 12, 25, 25, 5, 0.5, 0, 0, 0, 0, 0, 0.5, 0, 0.5, 0, 0),
(2,'holy-pve',         0, 0, 0, 0.01, 1, 0.5, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.3, 0, 0, 0, 5, 3, 0.1, 0, 0),

-- Hunter
(3,'mm-sv-pve',        2, 10, 0, 0.01, 0.5, 0, 0.5, 0.5, 5, 2.2, 50, 55, 15, 0, 0, 0, 0, 0, 2.5, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),

-- Rogue
(4,'combat-swords-pve',5, 13, 0, 0.01, 0.5, 0, 0, 1.5, 5, 2.5, 55, 60, 30, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0),

-- Priest
(5,'shadow-pve',       0, 0, 0, 0.01, 1, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 40, 0.5, 2, 0.1, 0, 0),
(5,'holy-pve',         0, 0, 0, 0.01, 1.5, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0, 0, 5, 3, 0.1, 0, 0),

-- Shaman
(7,'elemental-pve',    0, 0, 0, 0.01, 1, 0.5, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 40, 0.5, 2, 0.1, 0, 0),
(7,'resto-pve',        0, 0, 0, 0.01, 1.5, 0.5, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0, 0, 5, 3, 0.1, 0, 0),
(7,'enhancement-pve',  3, 10, 0, 0.01, 1, 0.2, 1, 3, 3, 2, 40, 45, 15, 0, 0, 0, 0, 0, 0, 1, 0, 10, 10, 0.5, 1, 0.1, 0, 0),

-- Mage
(8,'arcane-power-frost-pve', 0, 0, 0, 0.01, 1, 0.5, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 40, 0, 1.5, 0.1, 0, 0.2),
(8,'fire-pve',         0, 0, 0, 0.01, 1, 0.5, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 40, 45, 0, 1.5, 0.2, 0, 0),
(8,'frost-pve',        0, 0, 0, 0.01, 1, 0.5, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 35, 0, 1.5, 0.1, 0, 0.3),

-- Warlock
(9,'ds-ruin-pve',      0, 0, 0, 0.01, 1.5, 0.5, 2.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 40, 0, 1.5, 0.2, 0, 0),
(9,'affliction-pve',   0, 0, 0, 0.01, 1.5, 0.5, 2.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 40, 35, 0, 2, 0.2, 0, 0),

-- Druid
(11,'balance-pve',     0, 0, 0, 0.01, 1, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 40, 0.5, 2, 0.1, 0.2, 0),
(11,'resto-swiftmend-pve', 0, 0, 0, 0.01, 1.5, 1.5, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0, 0, 5, 3, 0.1, 0.1, 0),
(11,'feral-bear-pve',  0, 8, 0, 0.05, 4, 0, 0, 1, 3, 1.5, 30, 20, 10, 8, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.3, 0.2, 0),
(11,'feral-cat-pve',   3, 12, 0, 0.01, 1, 0, 0, 1.5, 5, 2.5, 50, 55, 15, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0.1, 0);


-- End of migration.
END IF;
END??
DELIMITER ;
CALL `add_migration`();
DROP PROCEDURE IF EXISTS add_migration;
