--
-- mod-zone-scaling -- add mod_zone_scaling_zone.direction
--
-- The base file uses CREATE TABLE IF NOT EXISTS so that hand-authored bands
-- survive a module update, which means a new column has to arrive as its own
-- migration. Idempotent: re-running it on a table that already has the column
-- is a no-op rather than an error.
--
-- 0 = inherit ZoneScaling.Direction (the shipped behaviour, so an existing row
--     means exactly what it meant before this column existed)
-- 1 = both  -- this zone scales up AND down regardless of the global setting
-- 2 = up    -- this zone never lowers a creature, regardless of the global
--
-- Added for imported content authored against a higher level cap: its own zones
-- must scale down to the player while the open world stays level-up-only.
--
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
             WHERE TABLE_SCHEMA = DATABASE()
               AND TABLE_NAME   = 'mod_zone_scaling_zone'
               AND COLUMN_NAME  = 'direction');

SET @sql := IF(@col = 0,
  'ALTER TABLE `mod_zone_scaling_zone`
     ADD COLUMN `direction` TINYINT UNSIGNED NOT NULL DEFAULT 0
     COMMENT ''0=inherit global, 1=both, 2=up-only'' AFTER `enabled`',
  'DO 0');

PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
