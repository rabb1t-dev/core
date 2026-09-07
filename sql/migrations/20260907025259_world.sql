INSERT INTO `migrations` VALUES ('20260907025259');

-- A healer talent build for level 19.
--
-- Every premade spec below level 60 was a damage build: twinks at 19, 29, 39 and 49, all of them
-- role 1 or 2, with no tank or healer template for any class at any level under 60. A low level bot
-- asked to heal therefore got the damage twink of its class, and a level 20 restoration shaman
-- spent its ten talent points on Concussion and Ancestral Knowledge.
--
-- Only the healer is authored here. A low level warrior tank is left on the arms/fury twink
-- deliberately: at this level the Protection tree buys little that matters, and what makes a bot
-- tank is its role rather than its talents - Defensive Stance, Taunt, Sunder, Revenge, the threat
-- rules and the peel all follow m_role and none of them reads the spec.
--
-- The level 60 build cannot stand in for it, because ApplyPremadeSpecTemplateToPlayer raises a
-- character to an unordered template's level: handing a level 20 bot resto-pve would make it a
-- level 60 bot. So the fix is a template authored at the level.
--
-- Both spell lists are the class spell list of the existing level 19 twink for that class, verbatim,
-- so the spellbook stays exactly what a level 19 character of that class has. Only the two talent
-- rows differ, which is the whole of what was wrong.

DELETE FROM `player_premade_spell_template` WHERE `entry` = 200;
DELETE FROM `player_premade_spell` WHERE `entry` = 200;

-- Restoration shaman, level 19, role 4 (ROLE_HEALER).
INSERT INTO `player_premade_spell_template` (`entry`, `class`, `level`, `role`, `name`) VALUES
(200, 7, 19, 4, 'resto-19');

INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`) VALUES
-- Class spells, as ele-enha-19-twink.
(200,   325, 0),  -- Lightning Shield
(200,   370, 0),  -- Purge
(200,   526, 0),  -- Cure Poison
(200,   548, 0),  -- Lightning Bolt
(200,   913, 0),  -- Healing Wave
(200,  1535, 0),  -- Fire Nova Totem
(200,  2008, 0),  -- Ancestral Spirit
(200,  2484, 0),  -- Earthbind Totem
(200,  2870, 0),  -- Cure Disease
(200,  3599, 0),  -- Searing Totem
(200,  6390, 0),  -- Stoneclaw Totem
(200,  8019, 0),  -- Rockbiter Weapon
(200,  8027, 0),  -- Flametongue Weapon
(200,  8045, 0),  -- Earth Shock, which is also this bot's interrupt
(200,  8052, 0),  -- Flame Shock
(200,  8075, 0),  -- Strength of Earth Totem
(200,  8143, 0),  -- Tremor Totem
(200,  8154, 0),  -- Stoneskin Totem
-- Ten talent points, both tier one Restoration, ids taken from resto-pve so they are the shaman
-- chains rather than the paladin or druid spells that share these names.
(200, 16217, 0),  -- Tidal Focus rank 5: healing spells cost five percent less mana
(200, 16229, 0);  -- Improved Healing Wave rank 5: half a second off the cast
