INSERT INTO `migrations` VALUES ('20260912044706');

-- Send the Altar of the Deeps teleport to the well outside the caves.
--
-- Spell 8735 (Blackfathom Teleport) is what the altar behind Aku'mai fires. Its stored
-- destination put the player back at the *inside* face of the instance entrance: map 48,
-- -151.89 106.96 -39.87, which is 1.7 yards from areatrigger 257's arrival point. That
-- leaves you still at the bottom of the dungeon with the whole cave approach to walk back
-- out through, which is not what the altar is for.
--
-- The intended destination is the flooded shaft out in Ashenvale, above the water you swim
-- through on the way in. Probing the terrain there shows a round well with its floor at
-- -22 to -29 ringed by ground at about +10, open air the whole way up. Arriving at z 15
-- puts the player just over the rim, above the middle of the water, so they drop the ~30
-- yards into it and walk out up the stairs. Landing in water means no fall damage.
--
-- Coordinates confirmed in game by standing in the water and reading the position back,
-- rather than guessed from the map: the well centre is 4136.83 884.50, map 1.
--
-- To revert: target_map 48, x -151.89, y 106.96, z -39.87, orientation 4.53.

UPDATE `spell_target_position`
SET `target_map` = 1,
    `target_position_x` = 4136.83,
    `target_position_y` = 884.50,
    `target_position_z` = 15.00
WHERE `id` = 8735;
