DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260808230000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260808230000');
-- Add your query below.


-- Rebuild the six level 60 specs added in 20260808220000 on the published vanilla builds.
--
-- The first version of these was legal and spent all 51 points, which is all the generator
-- could check, but legality is not quality. Several specs bought talents that do nothing in a
-- raid purely to reach the next tier -- Impact on the mage, Deflection on the warrior,
-- Martyrdom on the priest, Improved Nature's Grasp and Improved Thorns on the druid -- because
-- a boss cannot be stunned, does not attack the caster and is immune to the effects those
-- talents improve. Two of the six were the wrong build entirely: the warlock was a deep
-- destruction spec that nobody raids, and the druid had no Restoration points at all and so no
-- Furor, which is the talent the cat rotation is built on.
--
-- What is here now is the build each class actually ran in 1.12, per the surviving guides:
--
--   fire-pve               18/31/2   fire mage, Improved Scorch for Fire Vulnerability
--   arms-pve               31/20/0   two-handed arms, Mortal Strike and Sweeping Strikes
--   sm-ruin-pve            30/0/21   Shadow Mastery and Ruin, keeps the imp for Blood Pact
--   seal-fate-daggers-pve  30/16/5   Seal Fate daggers, Backstab and Opportunity
--   discipline-holy-pve    21/30/0   deep holy raid healing
--   feral-cat-pve          14/32/5   powershifting cat, Furor and Omen of Clarity
--
-- Two of those splits differ by a point from the number the guides quote, and in both cases
-- the guide is quoting something that cannot be built. The fire mage is published as 17/31/3,
-- but Arcane Meditation is on row 3 and so needs 15 points above it, making 18 the smallest
-- Arcane that reaches it. The point comes out of Frost, which is only there for spell hit.
--
-- Regenerate with contrib/harness/author_premade_specs.py rather than editing spell ids here.
-- Entries are deleted and reinserted because they have shipped once already: updating in place
-- would leave the previous spell rows behind and produce a bot holding both builds at once.

DELETE FROM `player_premade_spell` WHERE `entry` BETWEEN 101 AND 106;
DELETE FROM `player_premade_spell_template` WHERE `entry` BETWEEN 101 AND 106;

-- fire-pve: mage, 51 points (Fire 31, Arcane 18, Frost 2)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (101, 8, 60, 2, 'fire-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (101, 12351), (101, 12873), (101, 12341), (101, 12353), (101, 11366), (101, 11113), (101, 11368), (101, 12848), (101, 12400), (101, 11129), (101, 12592), (101, 12577), (101, 12842), (101, 18464), (101, 29076), (101, 29439), (101, 29445);

-- arms-pve: warrior, 51 points (Arms 31, Fury 20)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (102, 1, 60, 1, 'arms-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (102, 12867), (102, 12664), (102, 12659), (102, 12963), (102, 12785), (102, 12292), (102, 12294), (102, 12714), (102, 12296), (102, 12861), (102, 13048), (102, 12856), (102, 13002), (102, 12679), (102, 16494);

-- sm-ruin-pve: warlock, 51 points (Affliction 30, Destruction 21)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (103, 9, 60, 2, 'sm-ruin-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (103, 17792), (103, 17803), (103, 17877), (103, 17918), (103, 17959), (103, 18134), (103, 18127), (103, 17783), (103, 18095), (103, 17814), (103, 18178), (103, 18183), (103, 18219), (103, 18265), (103, 18275), (103, 18288), (103, 18223), (103, 18372), (103, 18830);

-- seal-fate-daggers-pve: rogue, 51 points (Assassination 30, Combat 16, Subtlety 5)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (104, 4, 60, 1, 'seal-fate-daggers-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (104, 13845), (104, 13791), (104, 13866), (104, 13872), (104, 13715), (104, 14075), (104, 14117), (104, 14137), (104, 14142), (104, 14161), (104, 14159), (104, 14164), (104, 14177), (104, 14179), (104, 14195);

-- discipline-holy-pve: priest, 51 points (Discipline 21, Holy 30)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (105, 5, 60, 4, 'discipline-holy-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (105, 14783), (105, 14791), (105, 14769), (105, 14767), (105, 14777), (105, 14751), (105, 14752), (105, 14523), (105, 15363), (105, 15011), (105, 15031), (105, 15356), (105, 17191), (105, 15014), (105, 15237), (105, 18535);

-- feral-cat-pve: druid, 51 points (Feral Combat 32, Balance 14, Restoration 5)
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES (106, 11, 60, 1, 'feral-cat-pve');
INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES (106, 16689), (106, 16835), (106, 16864), (106, 16906), (106, 16929), (106, 16862), (106, 16938), (106, 16944), (106, 16954), (106, 16968), (106, 16975), (106, 16999), (106, 24866), (106, 24894), (106, 17007), (106, 17061), (106, 17249), (106, 16857);


-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
