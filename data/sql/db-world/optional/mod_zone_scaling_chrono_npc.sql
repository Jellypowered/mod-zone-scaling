--
-- mod-zone-scaling -- OPTIONAL: an example Chrono Mode NPC
--
-- This file is under data/sql/db-world/optional/ and AzerothCore's updater does
-- NOT apply it automatically. Nothing in this module depends on it. Apply it by
-- hand only if you want the example NPC:
--
--     mysql -u acore -p acore_world < mod_zone_scaling_chrono_npc.sql
--
-- It creates creature_template entry 190000 and npc_text 190000, and spawns
-- nothing. Spawn it wherever you like -- an inn is the natural home, since
-- CanToggleChrono only permits the toggle in a rested area anyway:
--
--     .npc add 190000
--
-- The script is what matters, not this row. `npc_zone_scaling_chrono` will
-- drive ANY creature whose ScriptName is set to it, so an administrator who
-- would rather not add a creature can instead do:
--
--     UPDATE creature_template SET ScriptName = 'npc_zone_scaling_chrono'
--      WHERE entry = <an existing innkeeper>;
--
-- That is arguably the better placement. It is not done here because it would
-- overwrite whatever ScriptName that creature already had, which is not a
-- decision this module gets to make for you.
--
-- ⚠ Entry 190000 is a guess at a free id, not a reservation. AzerothCore has no
-- central registry for module creature entries. Check it against your own world
-- database before applying, and change it here and in creature_template if it
-- collides:
--
--     SELECT entry, name FROM creature_template WHERE entry = 190000;
--

DELETE FROM `creature_template` WHERE `entry` = 190000;
INSERT INTO `creature_template`
  (`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`,
   `minlevel`, `maxlevel`, `faction`, `npcflag`, `speed_walk`, `speed_run`,
   `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`,
   `RegenHealth`, `flags_extra`, `ScriptName`, `VerifiedBuild`)
VALUES
  (190000, 'Keeper of Hours', 'Chrono Mode', 'Speak', 0,
   80, 80, 35, 1, 1, 1.14286,
   0, 1, 2, 7, 138,
   1, 2, 'npc_zone_scaling_chrono', 0);

-- The model lives in creature_template_model, not in a modelid1 column --
-- AzerothCore moved it there and a script written against the old schema fails
-- with "Unknown column 'modelid1'". 19646 is a Bronze Drakonid-style humanoid;
-- any display id works.
DELETE FROM `creature_template_model` WHERE `CreatureID` = 190000;
INSERT INTO `creature_template_model`
  (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES
  (190000, 0, 19646, 1, 1);

-- faction 35 is "friendly to everyone", so the NPC is usable by both factions
-- and cannot be attacked. npcflag 1 is GOSSIP. unit_flags 2 is NON_ATTACKABLE
-- and flags_extra 2 is CREATURE_FLAG_EXTRA_CIVILIAN -- together these keep the
-- NPC out of combat entirely, which matters because the toggle it offers is
-- forbidden in combat.

DELETE FROM `npc_text` WHERE `ID` = 190000;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `Probability0`, `VerifiedBuild`)
VALUES
  (190000,
   'Time runs strangely here. Some travellers find the world rises to meet them, however far they have come; others walk a road that stands exactly as it was made.$B$BI can set your course either way -- but not while you are in the middle of something, and not out in the wild. Find a fire and an innkeeper first.',
   'Time runs strangely here. Some travellers find the world rises to meet them, however far they have come; others walk a road that stands exactly as it was made.$B$BI can set your course either way -- but not while you are in the middle of something, and not out in the wild. Find a fire and an innkeeper first.',
   1, 0);
