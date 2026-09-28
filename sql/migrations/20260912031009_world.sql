INSERT INTO `migrations` VALUES ('20260912031009');

-- Make the Altar of the Deeps behind Aku'mai teleport again on a 1.12 realm.
--
-- Upstream data retires the teleport at patch 7: the trap carrying Blackfathom Teleport
-- (gameobject guid 14106, entry 94040) is spawned only for patches 0-6, a second Blessing
-- of Blackfathom trap (guid 14107, entry 94039) is spawned at the same spot from patch 7,
-- and gameobject_template 103016's patch 7 row links the button to that one. Taken at face
-- value the altar at the end of the dungeon becomes a duplicate of the Shrine of Gelihast
-- near the start.
--
-- That is rejected here for two reasons. A buff handed out behind the final boss, with
-- nothing left to fight, is useless, while a ride back to the entrance is the obvious
-- purpose of an altar in that spot. And the teleport is not actually retired in the data
-- that matters: spell_target_position for 8735 covers builds 0-5875 and loads on this
-- realm, putting the destination at -151.89 106.96 -39.87 on map 48, which is the
-- instance entrance.
--
-- So the teleport trap is spawned for the supported patch range and the button is pointed
-- back at it. The patch 7 blessing trap spawn (guid 14107) is deliberately left in place
-- and simply goes unreferenced: nothing links to it and its trigger radius is 0, so it is
-- inert, and leaving it keeps this change easy to undo.
--
-- To revert: gameobject guid 14106 back to patch_max 6, and gameobject_template entry
-- 103016 patch 7 back to data3 94039.

UPDATE `gameobject` SET `patch_max` = 10 WHERE `guid` = 14106 AND `id` = 94040;
UPDATE `gameobject_template` SET `data3` = 94040 WHERE `entry` = 103016 AND `patch` = 7;
