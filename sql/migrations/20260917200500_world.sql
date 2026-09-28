INSERT INTO `migrations` VALUES ('20260917200500');

-- An ordered protection build, so a warrior tank below level 60 stops being handed an arms one.
--
-- SelectPremadeSpecTemplate will only take a template whose level matches the bot exactly, or an
-- ordered one at or above its level, because an ordered spec is spent down to the character's own
-- talent budget while an unordered one only means anything applied whole. The warrior list had no
-- ordered protection entry: 'protection-pve' (entry 14) is unordered and level 60, and every twink
-- template is an arms build. So every tank warrior under 60 fell through to 'arms-pve', which is
-- ordered, and the server said so on every summon:
--
--   ERROR: CombatBot: 'Fuszem' (class 1, level 35) wanted role tank but the only premade spec at
--   or below its level is 'arms-pve', which is a melee build. Talents will be wrong until a tank
--   template is authored for this level.
--
-- The cost was not only survivability. An arms-talented tank generates very little threat, and the
-- bots' own damage is governed by a ceiling measured as a share of the tank's threat -- so a level
-- 35 run had the tank at a median of 8 rage, holding top threat only 65% of the time, while the
-- mage stood idle for 75% of the ticks on which it had a target and enough mana to cast.
--
-- Entry 14 is left alone. It is a coherent level 60 build and still serves that level; it simply
-- cannot serve any other, and it cannot be made ordered as it stands because it lists class
-- abilities alongside its talents and an ordered spec counts every row as one talent point.
--
-- The order below is therefore pure talents, one row per rank, and is arranged so that each entry's
-- tier prerequisite is already paid for by the rows before it. Rage generation and threat come
-- first, which is what the diagnosis above asks for: Shield Specialization and Improved Bloodrage
-- feed the rage bar, then Defiance multiplies the threat everything else produces, then Improved
-- Sunder Armor makes the tank's cheapest threat ability cheaper still.

DELETE FROM `player_premade_spell_template` WHERE `entry` = 201;
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES
(201, 1, 60, 3, 'protection-ordered-pve');

DELETE FROM `player_premade_spell` WHERE `entry` = 201;
INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES
-- Shield Specialization 1-5: rage on every block, and the tank's rage bar is the problem.
(201, 12298,  1), (201, 12724,  2), (201, 12725,  3), (201, 12726,  4), (201, 12727,  5),
-- Improved Bloodrage 1-2: more of the opener's rage, which is when threat matters most.
(201, 12301,  6), (201, 12818,  7),
-- Anticipation 1-5: defence, and the five points that open the next tier.
(201, 12297,  8), (201, 12750,  9), (201, 12751, 10), (201, 12752, 11), (201, 12753, 12),
-- Defiance 1-5: the threat talent. Everything above exists partly to reach this.
(201, 12303, 13), (201, 12788, 14), (201, 12789, 15), (201, 12791, 16), (201, 12792, 17),
-- Improved Sunder Armor 1-3: the tank's staple threat ability, for less rage.
(201, 12308, 18), (201, 12810, 19), (201, 12811, 20),
-- Improved Revenge 1-3.
(201, 12797, 21), (201, 12799, 22), (201, 12800, 23),
-- Improved Shield Block 1-3. Rank ids are not in rank order in the data; these are.
(201, 12945, 24), (201, 12307, 25), (201, 12944, 26),
-- A level 35 tank stops here, with 26 points. The rest is for higher levels.
-- Toughness 1-5.
(201, 12299, 27), (201, 12761, 28), (201, 12762, 29), (201, 12763, 30), (201, 12764, 31),
-- Improved Taunt 1-2.
(201, 12302, 32), (201, 12765, 33),
-- Improved Shield Wall 1-2.
(201, 12312, 34), (201, 12803, 35),
-- Concussion Blow.
(201, 12809, 36),
-- One-Handed Weapon Specialization 1-5.
(201, 16538, 37), (201, 16539, 38), (201, 16540, 39), (201, 16541, 40), (201, 16542, 41),
-- Last Stand.
(201, 12975, 42),
-- Shield Slam.
(201, 23922, 43),
-- Improved Disarm 1-3.
(201, 12313, 44), (201, 12804, 45), (201, 12807, 46),
-- Improved Shield Bash 1-2.
(201, 12311, 47), (201, 12958, 48),
-- Iron Will 1-3, filling the budget to a level 60 warrior's 51 points.
(201, 12300, 49), (201, 12959, 50), (201, 12960, 51);
