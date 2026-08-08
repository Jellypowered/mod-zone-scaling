--
-- mod-zone-scaling -- characters database schema
--
-- One row per character that has ever set the flag explicitly. A character
-- with no row takes ZoneScaling.Chrono.Default, which is why this table is
-- shipped empty and stays empty on a realm where nobody has toggled: absence
-- is a meaningful state, not a missing one.
--
-- CREATE TABLE IF NOT EXISTS for the same reason as the world tables: AC's
-- updater re-applies a base file whenever its hash changes, and a DROP here
-- would silently reset every player's choice on every module update. This is
-- player-owned data, so it must survive.
--

CREATE TABLE IF NOT EXISTS `mod_zone_scaling_character` (
  `guid`      INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `enabled`   TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 = scaled (Chrono Mode on), 0 = fights stock',
  `changedAt` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'unix time of the last toggle',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-zone-scaling: per-character Chrono Mode opt-in';

--
-- Deliberately not a foreign key onto `characters`. Character deletion in this
-- core runs through several paths (immediate, deferred, and the GM restore
-- flow), and a constraint that fires from inside one of them turns a module
-- bug into a failed character delete. The row is removed from
-- OnPlayerDeleteFromDB instead, inside the core's own deletion transaction,
-- and an orphan row is harmless: it can only ever be read by a guid that no
-- longer logs in.
--
