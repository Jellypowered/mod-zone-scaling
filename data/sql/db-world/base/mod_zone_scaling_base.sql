--
-- mod-zone-scaling -- world database schema
--
-- Creates only mod_zone_scaling_* tables and never alters a core table.
--
-- CREATE TABLE IF NOT EXISTS, deliberately, rather than the DROP + CREATE that
-- many AzerothCore base files use. AC's updater re-applies a base file whenever
-- its hash changes, so a DROP here would silently destroy an administrator's
-- zone bands every time this module is updated. These tables hold hand-authored
-- configuration, not reference data, so they must survive.
--
-- The cost is that column changes need an explicit migration under
-- data/sql/db-world/updates/ instead of coming free with the drop.
--

CREATE TABLE IF NOT EXISTS `mod_zone_scaling_zone` (
  `zoneId`   INT UNSIGNED NOT NULL COMMENT 'AreaTable.dbc zone id',
  `minLevel` TINYINT UNSIGNED NOT NULL COMMENT 'band floor; a lower-level player still faces this',
  `maxLevel` TINYINT UNSIGNED NOT NULL COMMENT 'band ceiling; set to 80 to never outgrow the zone',
  `enabled`  TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'per-zone kill switch',
  `comment`  VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'zone name, for humans',
  PRIMARY KEY (`zoneId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-zone-scaling: per-zone level bands';

--
-- Shipped empty on purpose. With ZoneScaling.DefaultBehaviour = off, an empty
-- table means an enabled module still changes nothing until an administrator
-- opts a zone in. Westfall is given as a commented example.
--
-- INSERT INTO `mod_zone_scaling_zone` (`zoneId`, `minLevel`, `maxLevel`, `enabled`, `comment`)
--     VALUES (40, 10, 60, 1, 'Westfall');


CREATE TABLE IF NOT EXISTS `mod_zone_scaling_exclude` (
  `entry`  INT UNSIGNED NOT NULL COMMENT 'creature_template.entry, never scaled',
  `reason` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-zone-scaling: creatures never touched';

--
-- This table is for exceptions the module cannot infer. It does not need rows
-- for world bosses, critters, totems, vehicles, quest givers, vendors or
-- trainers -- those are excluded in code, so that a fresh install is safe
-- before anyone has curated a list.
--
