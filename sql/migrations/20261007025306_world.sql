INSERT INTO `migrations` VALUES ('20261007025306');

-- Altar of Hakkar: after Into the Depths (3446) is turned in, using the altar again shows the
-- Atal'ai statue order as a light over each statue in turn, which is the clue Secret of the Circle
-- (3447) refers to. Since 3447 moved to Marvon Rivetseeker the altar has nothing to offer after the
-- turn-in, so its interact-conditional flag (4) left it unclickable and the clue unreachable.
UPDATE `gameobject_template` SET `script_name` = 'go_altar_of_hakkar', `flags` = `flags` & ~4 WHERE `entry` = 148836;
