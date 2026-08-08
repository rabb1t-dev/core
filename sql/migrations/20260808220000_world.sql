DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260808220000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260808220000');
-- Add your query below.


-- Level 60 talent specs for the builds the shipped data has no template for. Before this there
-- was no fire mage, no arms warrior, no destruction warlock, no dagger rogue, no discipline
-- priest and no cat druid at level 60, which between them are most of a raid's damage and one
-- of its healing specs. A roster that wants a named build for each member could not ask for
-- any of them, and bot spec selection cannot tell two specs apart by role alone -- a fire and
-- a frost mage are both simply ranged damage -- so the missing template was the whole gap.
--
-- Generated and checked by contrib/harness/author_premade_specs.py, which resolves talents by
-- name through Talent.dbc and refuses to emit a build that is not legal. That check is not
-- decorative: specs are applied with LearnSpell rather than LearnTalent, so nothing at runtime
-- verifies tier requirements, prerequisites or the point budget, and an illegal build applies
-- in silence. Two prerequisites in the arms build were found only because of it. Edit the
-- generator and regenerate rather than editing the spell ids here by hand.
--
-- Every build spends all 51 points available at level 60. Entries start at 101 to sit clear of
-- the 53 templates already shipped.

-- fire-pve: mage, 51 points (Fire 32, Arcane 19)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (101, 8, 60, 2, 'fire-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (101, 12351), (101, 12873), (101, 12341), (101, 12360), (101, 11368), (101, 12848), (101, 12400), (101, 11129), (101, 12592), (101, 12577), (101, 12842), (101, 6057), (101, 11242), (101, 12606), (101, 18464), (101, 29076);

-- arms-pve: warrior, 51 points (Arms 31, Fury 20)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (102, 1, 60, 1, 'arms-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (102, 12867), (102, 12815), (102, 12664), (102, 12659), (102, 16463), (102, 12292), (102, 12294), (102, 12714), (102, 12296), (102, 12861), (102, 13048), (102, 12856), (102, 13002), (102, 12679), (102, 16494);

-- destruction-pve: warlock, 51 points (Destruction 32, Affliction 19)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (103, 9, 60, 2, 'destruction-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (103, 17792), (103, 17803), (103, 17836), (103, 17877), (103, 17918), (103, 17958), (103, 17959), (103, 17962), (103, 18134), (103, 18136), (103, 17814), (103, 18178), (103, 18183), (103, 18218), (103, 18288), (103, 18372), (103, 18830);

-- assassination-daggers-pve: rogue, 51 points (Assassination 31, Subtlety 20)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (104, 4, 60, 1, 'assassination-daggers-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (104, 14065), (104, 13980), (104, 14071), (104, 13981), (104, 14075), (104, 14081), (104, 14117), (104, 14137), (104, 14142), (104, 14161), (104, 14159), (104, 14176), (104, 14177), (104, 14179), (104, 14195), (104, 14983);

-- discipline-holy-pve: priest, 51 points (Discipline 21, Holy 30)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (105, 5, 60, 4, 'discipline-holy-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (105, 14774), (105, 14783), (105, 14791), (105, 14769), (105, 14749), (105, 14777), (105, 14751), (105, 14752), (105, 15363), (105, 15011), (105, 15031), (105, 15356), (105, 17191), (105, 15014), (105, 18535), (105, 27811);

-- feral-cat-pve: druid, 51 points (Feral Combat 33, Balance 18)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (106, 11, 60, 1, 'feral-cat-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (106, 16689), (106, 16819), (106, 16835), (106, 16840), (106, 16864), (106, 16906), (106, 16938), (106, 16944), (106, 16951), (106, 16954), (106, 16961), (106, 16968), (106, 16975), (106, 16999), (106, 24866), (106, 24894), (106, 17007), (106, 17249), (106, 16857);

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
