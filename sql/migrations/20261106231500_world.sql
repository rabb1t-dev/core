INSERT INTO `migrations` VALUES ('20261106231500');

-- Run the Gates of Ahn'Qiraj war effort on a 1.12 server, gates closed until the Scarab Gong is rung.
-- The war effort events are capped at patch 1.9, so at a later WowPatch they are disabled on load and
-- the gate doodads (event 83) never spawn, leaving Ahn'Qiraj open from the start.
UPDATE `game_event` SET `patch_max` = 10
WHERE `entry` IN (22, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 83);
UPDATE `game_event` SET `disabled` = 0 WHERE `entry` = 84;

-- Start at WAR_EFFORT_STAGE_GONG_WAIT: troops in Silithus, gong usable, gates shut.
REPLACE INTO `variables` (`index`, `value`) VALUES (30050, 7), (30051, UNIX_TIMESTAMP());
DELETE FROM `variables` WHERE `index` IN (30052, 30053);
