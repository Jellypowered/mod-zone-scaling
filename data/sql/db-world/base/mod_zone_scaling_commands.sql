--
-- mod-zone-scaling -- command help text
--
-- Populates acore_world.command so that `.help zs`, `.help zs ratio` and the
-- rest answer with something useful instead of "There is no such command".
--
-- This is the one place the module writes to a core table, and it is safe to:
-- `command` is reference data keyed by command name, every row here is
-- namespaced under `zs` (plus the player-facing `chrono`), and the DELETE below
-- is scoped to exactly those names. Nothing else in the table is touched.
--
-- DELETE + INSERT rather than CREATE IF NOT EXISTS, unlike this module's own
-- tables: help text is reference data that should track the code, so being
-- re-applied when the file hash changes is the behaviour we want here.
--
-- Security levels are SEC_PLAYER 0, SEC_MODERATOR 1, SEC_GAMEMASTER 2,
-- SEC_ADMINISTRATOR 3. They must match the ChatCommandTable in
-- src/ZoneScalingCommands.cpp and src/ZoneScalingChrono.cpp -- the table in the
-- code is what actually gates access; a row here that disagrees only misleads
-- whoever reads the help.
--

DELETE FROM `command` WHERE `name` IN (
  'zs',
  'zs status',
  'zs zone',
  'zs info',
  'zs level',
  'zs ratio',
  'zs questxp',
  'zs coin',
  'zs gear',
  'zs aggro',
  'zs sweep',
  'zs excluded',
  'zs stat',
  'zs reload',
  'chrono'
);

INSERT INTO `command` (`name`, `security`, `help`) VALUES

('zs', 2,
'Syntax: .zs $subcommand

Level-scaling module (mod-zone-scaling). With no subcommand, behaves as .zs status.

Subcommands: status, zone, info, level, ratio, questxp, coin, gear, aggro, sweep, excluded, stat, reload.

Every subcommand is read-only except reload. None of them changes a creature: they answer what the module WOULD do, so they can be used on a live realm without disturbing it.'),

('zs status', 2,
'Syntax: .zs status

Show the module''s configuration and its counters since startup: how many zone bands and exclusions are loaded, the default behaviour and direction, which map types scale, and running totals for damage events scaled, level fields patched, XP corrected, loot coin scaled and gear rebanded.

The first half is what the config says; the second half is what has actually happened. A realm where the config looks right and the counters are all zero is not scaling anything.'),

('zs zone', 2,
'Syntax: .zs zone [#zoneId]

Show the scaling band that applies to a zone, and where it came from -- a row in mod_zone_scaling_zone, or the default band, or not at all.

With no argument, reports the zone you are standing in. From the console there is no current zone, so the id is required.'),

('zs info', 2,
'Syntax: .zs info

Show what the module has done to your current target: its canonical level, the level it is presenting to you, whether it is scalable, and whether its stats were recalculated.

In-game only -- it needs a target.'),

('zs level', 2,
'Syntax: .zs level #zoneId #playerLevel [#canonicalLevel]

Show the effective level a creature would present at, for a player of the given level in the given zone.

Answers the question "what would this zone do to me at level N" without needing a character at level N. If canonicalLevel is omitted the band is reported on its own.'),

('zs ratio', 2,
'Syntax: .zs ratio #entry #zoneId #playerLevel [#canonicalLevel]

Show the damage, healing and health multipliers that would apply to creature #entry against a player of the given level in the given zone, and the stat values behind them.

This is the command for arguing with the combat maths. It reports the ratio the module would use, not one recomputed by the command, so it cannot agree with itself while disagreeing with the game.'),

('zs questxp', 2,
'Syntax: .zs questxp #questId #playerLevel

Show the experience a quest would award to a player of the given level, before and after the module''s correction, and the reason if no correction applies.'),

('zs coin', 2,
'Syntax: .zs coin #canonicalLevel #effectiveLevel

Show the coin multiplier between two levels, with worked examples in gold/silver/copper, and the configured clamp.

The multiplier is a ratio of the core''s own quest_money_reward curve -- it is not invented by this module. That curve is roughly twice as generous as creature coin data past level 70; ZoneScaling.Loot.MoneyMax is the lever for that.'),

('zs gear', 2,
'Syntax: .zs gear #itemId #effectiveLevel

Show what a dropped item would be rebanded to at the given effective level, or the reason it would be left alone.

The answer is deliberately not deterministic: the replacement is chosen at random from every candidate of the same slot, subclass and quality at that level, and the command reports how many candidates there were. Running it twice and getting two different items is correct behaviour, not a bug.'),

('zs aggro', 2,
'Syntax: .zs aggro #entry #zoneId #playerLevel [#canonicalLevel]

Show the aggro radius a creature would use against a player of the given level in the given zone, under the current ZoneScaling.AggroRange setting, and what stock AzerothCore would have used.'),

('zs sweep', 2,
'Syntax: .zs sweep #mapId [#zoneId]

Walk every creature template that can spawn on a map (optionally filtered to one zone) and report how many would scale, how many are excluded and why, and the level range involved.

Console-friendly and read-only. This is the breadth check: it finds the creatures a band would touch before any player meets one.'),

('zs excluded', 2,
'Syntax: .zs excluded [#entry]

List the creature entries excluded from scaling, and the reason for each -- a row in mod_zone_scaling_exclude, or a rule the module applies on its own.

With an entry, answers for just that creature, including creatures that are not excluded.'),

('zs stat', 2,
'Syntax: .zs stat [#entry] [#minLevel] [#maxLevel]

Show the raw creature base stats the scaling maths reads, for a creature entry across a level range.

This is the bottom of the stack: if .zs ratio looks wrong, this shows whether the inputs or the arithmetic is at fault.'),

('zs reload', 3,
'Syntax: .zs reload

Reload mod_zone_scaling_zone and mod_zone_scaling_exclude from the world database, without a restart and without touching config file settings.

Config options come from .reload config instead. This command is for the tables only, which is why it is separate: editing a band row is routine, and restarting a realm to pick one up is not.'),

('chrono', 0,
'Syntax: .chrono
       .chrono on
       .chrono off

Chrono Mode: choose whether open-world level scaling applies to you.

With no argument, reports your current setting. Your choice is saved and survives logout.

Only meaningful on a realm where the administrator has made scaling opt-in. On a realm where scaling applies to everyone, or where it is switched off entirely, this command says so rather than pretending to work.');
