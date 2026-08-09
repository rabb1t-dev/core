DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260809010000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260809010000');
-- Add your query below.


-- Stat caps, and the four level 60 raid specs that had a talent build but no weight row.
--
-- Caps first. The weight model is linear, which is wrong for the two stats vanilla hard-caps:
-- a miss chance cannot go below zero, so hit past the cap is worth nothing, and weapon skill
-- stops reducing miss and dodge once it reaches the target's defense. Left linear, a warrior
-- keeps valuing hit at 55 a point long after it has stopped doing anything, which is the
-- largest single source of confidently wrong gear choices the engine had.
--
-- A cap is a total, so it cannot be applied to one item. The evaluator applies these to the
-- sum over everything worn.
--
-- The numbers are gear-derived totals against a level 63 target, which is what raid bosses
-- are: 9 percent hit for melee, 16 for spells, and 5 weapon skill, that last being why
-- Edgemaster's Handguards is famous - 300 to 305 is the part that removes the level penalty
-- and everything above it is nearly free of benefit.
--
-- Talent-granted hit is deliberately NOT subtracted, though it belongs in these numbers: a
-- rogue with Precision needs five percent less hit from gear than one without. Subtracting it
-- means reading the authored talent build for each spec and keeping the two in step, so until
-- that is done these caps are loose. Loose is the safe direction. A cap set too high only
-- leaves some hit overvalued, which is the behaviour being replaced; a cap set too low would
-- have members discard hit they actually need.
--
-- Twink rows are capped at 5 rather than 9 because they fight same-level players, where there
-- is no level-based miss to remove.

ALTER TABLE `raidguild_stat_weight`
  ADD COLUMN `hit_cap` float NOT NULL DEFAULT '0' AFTER `frost_res`,
  ADD COLUMN `sphit_cap` float NOT NULL DEFAULT '0' AFTER `hit_cap`,
  ADD COLUMN `weapon_skill_cap` float NOT NULL DEFAULT '0' AFTER `sphit_cap`;

-- Melee and ranged physical specs: both physical caps apply.
UPDATE `raidguild_stat_weight` SET `hit_cap` = 9, `weapon_skill_cap` = 5
WHERE (`class` = 1 AND `spec` IN ('fury','fury-dw-pve','tank','protection-pve'))
   OR (`class` = 2 AND `spec` IN ('retribution-pve','protection-pve'))
   OR (`class` = 3 AND `spec` = 'mm-sv-pve')
   OR (`class` = 4 AND `spec` = 'combat-swords-pve')
   OR (`class` = 11 AND `spec` IN ('feral-bear-pve','feral-cat-pve'));

-- Same, at the player-versus-player cap.
UPDATE `raidguild_stat_weight` SET `hit_cap` = 5, `weapon_skill_cap` = 5
WHERE `class` = 1 AND `spec` IN ('pvp','arms-fury-19-twink','arms-29-twink','arms-39-twink','arms-49-twink');

-- Casters that need spell hit. Healers are absent on purpose: a heal cannot miss, so their
-- sphit weight is already zero and a cap on it would describe nothing.
UPDATE `raidguild_stat_weight` SET `sphit_cap` = 16
WHERE (`class` = 5 AND `spec` = 'shadow-pve')
   OR (`class` = 7 AND `spec` = 'elemental-pve')
   OR (`class` = 8 AND `spec` IN ('arcane-power-frost-pve','fire-pve','frost-pve'))
   OR (`class` = 9 AND `spec` IN ('ds-ruin-pve','affliction-pve'))
   OR (`class` = 11 AND `spec` = 'balance-pve');

-- Enhancement swings a weapon and casts, so it is subject to all three.
UPDATE `raidguild_stat_weight` SET `hit_cap` = 9, `sphit_cap` = 16, `weapon_skill_cap` = 5
WHERE `class` = 7 AND `spec` = 'enhancement-pve';


-- The missing rows. These four specs have an authored, published talent build from
-- 20260808230000 and 20260808234500 but no weight row, so a member on any of them scored its
-- gear against whatever other row for the class was found first. Column order matches the
-- CREATE TABLE in 20260809001000 with the three cap columns appended.
--
-- Values follow the nearest authored spec of the same class and role rather than inventing
-- precision that has not been derived: arms takes the warrior damage row, seal fate takes the
-- rogue one with crit raised because the build's damage comes from critical strikes feeding
-- Seal Fate, discipline-holy takes the priest healing row, and shadow-mastery-ruin takes the
-- warlock damage row.

INSERT INTO `raidguild_stat_weight`
(`class`,`spec`,`avg_hit`,`dps`,`speed`,`armor`,`stam`,`spi`,`intellect`,`str`,`agi`,`ap`,`hit`,`crit`,`weapon_skill`,`defense`,`dodge`,`parry`,`block`,`block_value`,`ranged_ap`,`spdmg`,`sppen`,`sphit`,`spcrit`,`spheal`,`mp5`,`fire_res`,`nat_res`,`frost_res`,`hit_cap`,`sphit_cap`,`weapon_skill_cap`)
VALUES
-- Warrior
(1,'arms-pve',              5.5, 14.784, 0, 0.0042, 0, 0, 0, 5.28, 3.2584, 2.64, 55, 65, 36.84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0, 9, 0, 5),
-- Rogue
(4,'seal-fate-daggers-pve', 5, 13, 0, 0.01, 0.5, 0, 0, 1.5, 5, 2.5, 55, 65, 30, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.1, 0, 0, 9, 0, 5),
-- Priest
(5,'discipline-holy-pve',   0, 0, 0, 0.01, 1.5, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0, 0, 0, 5, 3, 0.1, 0, 0, 0, 0, 0),
-- Warlock
(9,'sm-ruin-pve',           0, 0, 0, 0.01, 1.5, 0.5, 2.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1, 45, 40, 0, 1.5, 0.2, 0, 0, 0, 16, 0);


-- End of migration.
END IF;
END??
DELIMITER ;
CALL `add_migration`();
DROP PROCEDURE IF EXISTS add_migration;
