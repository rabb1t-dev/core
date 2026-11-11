INSERT INTO `migrations` VALUES ('20261011130000');

-- Double Argent Dawn reputation from every repeatable turn-in, at the player's request:
-- Scourgestones, the Plaguelands cauldrons, Healthy Dragon Scale, Craftsman's Writs and the
-- Armaments and Guard turn-ins. All of them carry Argent Dawn in the first reward slot.
UPDATE `quest_template` SET `RewRepValue1` = `RewRepValue1` * 2
WHERE `RewRepFaction1` = 529 AND `RewRepValue1` > 0 AND (`SpecialFlags` & 1) = 1;
