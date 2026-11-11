INSERT INTO `migrations` VALUES ('20261011120000');

-- Triple Argent Dawn reputation from kills that gave +5, at the player's request. Only the +5
-- awards change; the standing at which each one stops (Friendly or Honored for most) is kept.
UPDATE `creature_onkill_reputation` SET `RewOnKillRepValue1` = 15
WHERE `RewOnKillRepFaction1` = 529 AND `RewOnKillRepValue1` = 5;
UPDATE `creature_onkill_reputation` SET `RewOnKillRepValue2` = 15
WHERE `RewOnKillRepFaction2` = 529 AND `RewOnKillRepValue2` = 5;
