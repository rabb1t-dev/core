DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260808234500');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260808234500');
-- Add your query below.


-- Give a premade spec an order to spend its talent points in, so one template serves every
-- level instead of only the level it was authored for.
--
-- A template was a set of finished talents with no sequence anywhere, which meant it could only
-- ever be applied whole. Templates exist at levels 19, 29, 39, 49 and 60 and nowhere else, so
-- asking for four level 45 bots got the level 39 twink build: selection falls back to the
-- highest template below the level, GiveLevel only fires when the character is lower than the
-- template, and the result is a level 45 character spending a 30 point build out of the 36
-- points it has. Six points vanished, into a spec tuned for level 39 battlegrounds, and nothing
-- said a word.
--
-- `spend_order` numbers the rows of a spec from 1, one row per talent point rather than one per
-- talent, so the first N rows are exactly the build for a character holding N points. A level
-- 45 bot now gets the first 36 points of the real level 60 build, and levelling a roster member
-- later spends the tail without anything being re-authored. Rows left at 0 are an unordered set
-- and keep the old behaviour, which is what the 53 shipped templates still rely on.
--
-- The orders are generated and checked by contrib/harness/author_premade_specs.py. It validates
-- every prefix rather than only the finished build, because a prefix is a real character: an
-- order can be perfectly legal at 60 and still put a talent on an unpaid row at 39.

ALTER TABLE `player_premade_spell`
  ADD COLUMN `spend_order` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0 AFTER `spell`;

DELETE FROM `player_premade_spell` WHERE `entry` BETWEEN 101 AND 106;
DELETE FROM `player_premade_spell_template` WHERE `entry` BETWEEN 101 AND 106;

-- fire-pve: mage, 51 points spent in order (Fire 31, Arcane 18, Frost 2)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (101, 8, 60, 2, 'fire-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES (101, 11069, 1), (101, 12338, 2), (101, 12339, 3), (101, 12340, 4), (101, 12341, 5), (101, 11119, 6), (101, 11120, 7), (101, 12846, 8), (101, 12847, 9), (101, 12848, 10), (101, 11100, 11), (101, 12353, 12), (101, 11366, 13), (101, 11083, 14), (101, 12351, 15), (101, 11095, 16), (101, 12872, 17), (101, 12873, 18), (101, 29074, 19), (101, 29075, 20), (101, 29076, 21), (101, 11115, 22), (101, 11367, 23), (101, 11368, 24), (101, 11113, 25), (101, 11124, 26), (101, 12378, 27), (101, 12398, 28), (101, 12399, 29), (101, 12400, 30), (101, 11129, 31), (101, 11210, 32), (101, 12592, 33), (101, 11222, 34), (101, 12839, 35), (101, 12840, 36), (101, 12841, 37), (101, 12842, 38), (101, 29441, 39), (101, 29444, 40), (101, 29445, 41), (101, 11213, 42), (101, 12574, 43), (101, 12575, 44), (101, 12576, 45), (101, 12577, 46), (101, 18462, 47), (101, 18463, 48), (101, 18464, 49), (101, 29438, 50), (101, 29439, 51);

-- arms-pve: warrior, 51 points spent in order (Arms 31, Fury 20)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (102, 1, 60, 1, 'arms-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES (102, 12282, 1), (102, 12663, 2), (102, 12664, 3), (102, 12286, 4), (102, 12658, 5), (102, 12659, 6), (102, 12295, 7), (102, 12676, 8), (102, 12677, 9), (102, 12678, 10), (102, 12679, 11), (102, 12290, 12), (102, 12963, 13), (102, 12296, 14), (102, 12834, 15), (102, 12849, 16), (102, 12867, 17), (102, 12163, 18), (102, 12711, 19), (102, 12712, 20), (102, 12713, 21), (102, 12714, 22), (102, 16493, 23), (102, 16494, 24), (102, 12700, 25), (102, 12781, 26), (102, 12783, 27), (102, 12784, 28), (102, 12785, 29), (102, 12292, 30), (102, 12294, 31), (102, 12320, 32), (102, 12852, 33), (102, 12853, 34), (102, 12855, 35), (102, 12856, 36), (102, 12322, 37), (102, 12999, 38), (102, 13000, 39), (102, 13001, 40), (102, 13002, 41), (102, 12318, 42), (102, 12857, 43), (102, 12858, 44), (102, 12860, 45), (102, 12861, 46), (102, 12317, 47), (102, 13045, 48), (102, 13046, 49), (102, 13047, 50), (102, 13048, 51);

-- sm-ruin-pve: warlock, 51 points spent in order (Affliction 30, Destruction 21)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (103, 9, 60, 2, 'sm-ruin-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES (103, 18174, 1), (103, 18175, 2), (103, 18176, 3), (103, 18177, 4), (103, 18178, 5), (103, 17810, 6), (103, 17811, 7), (103, 17812, 8), (103, 17813, 9), (103, 17814, 10), (103, 18213, 11), (103, 18372, 12), (103, 18182, 13), (103, 18183, 14), (103, 18827, 15), (103, 18829, 16), (103, 18830, 17), (103, 17783, 18), (103, 18288, 19), (103, 18218, 20), (103, 18219, 21), (103, 18094, 22), (103, 18095, 23), (103, 18265, 24), (103, 18223, 25), (103, 18271, 26), (103, 18272, 27), (103, 18273, 28), (103, 18274, 29), (103, 18275, 30), (103, 17793, 31), (103, 17796, 32), (103, 17801, 33), (103, 17802, 34), (103, 17803, 35), (103, 17788, 36), (103, 17789, 37), (103, 17790, 38), (103, 17791, 39), (103, 17792, 40), (103, 18126, 41), (103, 18127, 42), (103, 18130, 43), (103, 18131, 44), (103, 18132, 45), (103, 18133, 46), (103, 18134, 47), (103, 17877, 48), (103, 17917, 49), (103, 17918, 50), (103, 17959, 51);

-- seal-fate-daggers-pve: rogue, 51 points spent in order (Assassination 30, Combat 16, Subtlety 5)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (104, 4, 60, 1, 'seal-fate-daggers-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES (104, 14162, 1), (104, 14163, 2), (104, 14164, 3), (104, 14138, 4), (104, 14139, 5), (104, 14140, 6), (104, 14141, 7), (104, 14142, 8), (104, 14156, 9), (104, 14160, 10), (104, 14161, 11), (104, 14158, 12), (104, 14159, 13), (104, 14179, 14), (104, 14128, 15), (104, 14132, 16), (104, 14135, 17), (104, 14136, 18), (104, 14137, 19), (104, 14113, 20), (104, 14114, 21), (104, 14115, 22), (104, 14116, 23), (104, 14117, 24), (104, 14177, 25), (104, 14186, 26), (104, 14190, 27), (104, 14193, 28), (104, 14194, 29), (104, 14195, 30), (104, 13712, 31), (104, 13788, 32), (104, 13789, 33), (104, 13790, 34), (104, 13791, 35), (104, 13733, 36), (104, 13865, 37), (104, 13866, 38), (104, 13705, 39), (104, 13832, 40), (104, 13843, 41), (104, 13844, 42), (104, 13845, 43), (104, 13742, 44), (104, 13872, 45), (104, 13715, 46), (104, 14057, 47), (104, 14072, 48), (104, 14073, 49), (104, 14074, 50), (104, 14075, 51);

-- discipline-holy-pve: priest, 51 points spent in order (Holy 30, Discipline 21)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (105, 5, 60, 4, 'discipline-holy-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES (105, 14908, 1), (105, 15020, 2), (105, 17191, 3), (105, 14889, 4), (105, 15008, 5), (105, 15009, 6), (105, 15010, 7), (105, 15011, 8), (105, 18530, 9), (105, 18531, 10), (105, 18533, 11), (105, 18534, 12), (105, 18535, 13), (105, 15237, 14), (105, 14892, 15), (105, 15362, 16), (105, 15363, 17), (105, 14912, 18), (105, 15013, 19), (105, 15014, 20), (105, 14901, 21), (105, 15028, 22), (105, 15029, 23), (105, 15030, 24), (105, 15031, 25), (105, 14898, 26), (105, 15349, 27), (105, 15354, 28), (105, 15355, 29), (105, 15356, 30), (105, 14522, 31), (105, 14788, 32), (105, 14789, 33), (105, 14790, 34), (105, 14791, 35), (105, 14523, 36), (105, 14749, 37), (105, 14767, 38), (105, 14748, 39), (105, 14768, 40), (105, 14769, 41), (105, 14751, 42), (105, 14521, 43), (105, 14776, 44), (105, 14777, 45), (105, 14520, 46), (105, 14780, 47), (105, 14781, 48), (105, 14782, 49), (105, 14783, 50), (105, 14752, 51);

-- feral-cat-pve: druid, 51 points spent in order (Feral Combat 32, Restoration 5, Balance 14)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (106, 11, 60, 1, 'feral-cat-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES (106, 16934, 1), (106, 16935, 2), (106, 16936, 3), (106, 16937, 4), (106, 16938, 5), (106, 16858, 6), (106, 16859, 7), (106, 16860, 8), (106, 16861, 9), (106, 16862, 10), (106, 16929, 11), (106, 17002, 12), (106, 24866, 13), (106, 16942, 14), (106, 16943, 15), (106, 16944, 16), (106, 16966, 17), (106, 16968, 18), (106, 16972, 19), (106, 16974, 20), (106, 16975, 21), (106, 16952, 22), (106, 16954, 23), (106, 16998, 24), (106, 16999, 25), (106, 16857, 26), (106, 17003, 27), (106, 17004, 28), (106, 17005, 29), (106, 17006, 30), (106, 24894, 31), (106, 17007, 32), (106, 17056, 33), (106, 17058, 34), (106, 17059, 35), (106, 17060, 36), (106, 17061, 37), (106, 16689, 38), (106, 17245, 39), (106, 17247, 40), (106, 17248, 41), (106, 17249, 42), (106, 16902, 43), (106, 16903, 44), (106, 16904, 45), (106, 16905, 46), (106, 16906, 47), (106, 16833, 48), (106, 16834, 49), (106, 16835, 50), (106, 16864, 51);


-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
