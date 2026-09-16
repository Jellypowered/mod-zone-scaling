--
-- mod-zone-scaling -- add mod_zone_scaling_zone.ignore_chrono
--
-- Same migration shape and the same reason as the direction column: the base
-- file uses CREATE TABLE IF NOT EXISTS so that hand-authored bands survive a
-- module update, so a new column arrives on its own. Idempotent.
--
-- 0 = respect Chrono Mode (the shipped behaviour, so an existing row means
--     exactly what it meant before this column existed)
-- 1 = scale in this zone even for a player who has opted out
--
-- Chrono is a preference about the OPEN WORLD, where fighting at native levels
-- is a legitimate way to play and opting out gives you the realm as Blizzard
-- shipped it. Content imported from a higher level cap has no such reading: an
-- opted-out player meets creatures 3-5 levels above the cap, which is not
-- "unscaled", it is unreachable. A zone that cannot be played unscaled sets
-- this, and the player's choice stops applying there because there was never a
-- second way to play it.
--
-- The Chrono XP bonus is deliberately NOT granted by this flag. That bonus is
-- the reward for choosing to be scaled; paying it to someone who did not choose
-- would be paying for a decision they never made.
--
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE()
               AND TABLE_NAME   = 'mod_zone_scaling_zone'
               AND COLUMN_NAME  = 'ignore_chrono');

SET @sql := IF(@col = 0,
  'ALTER TABLE `mod_zone_scaling_zone`
     ADD COLUMN `ignore_chrono` TINYINT UNSIGNED NOT NULL DEFAULT 0
     COMMENT ''1 = scale here even for players opted out of Chrono Mode'' AFTER `direction`',
  'DO 0');

PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
