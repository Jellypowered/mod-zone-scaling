/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Breadth: where scaling is allowed to apply, and what stays special once it
 * does. Two independent questions that share a milestone because both are about
 * turning the module loose on a whole realm rather than one test zone.
 *
 * WHERE. Bands are keyed on zone id, and a zone id says nothing about whether
 * you are standing in the open world, a five-man, a raid or an arena. So
 * "scale all zones" -- which is what `DefaultBehaviour = clamp` means -- would
 * quietly include every dungeon and raid map on the realm. That is not a
 * degraded version of the feature, it is a different and much worse feature:
 * instanced content is hand-tuned, its difficulty is the point, and a module
 * that silently reflattens Naxxramas to a levelling band has broken something
 * nobody asked it to touch. Each map kind is therefore its own opt-in, and
 * everything instanced is off by default.
 *
 * The classification comes from MapEntry::map_type and deliberately not from
 * the Map:: predicates, because those overlap in a way that is easy to miss:
 * `Map::IsDungeon()` is true for raids as well. Writing the obvious
 *
 *     if (map->IsDungeon())  return _dungeonsEnabled;
 *     if (map->IsRaid())     return _raidsEnabled;
 *
 * gives raids the *dungeon* setting and never reaches the raid one. Resolving
 * to a single five-way value first makes that mistake unrepresentable rather
 * than merely avoidable.
 *
 * WHAT STAYS SPECIAL. Scaling flattens a zone by construction: every creature
 * presents at the player's level. Applied naively that erases the one piece of
 * information a player reads before pulling -- that the rare elite patrolling
 * the ridge is a harder fight than the boars around it. The rank multipliers on
 * health and damage survive scaling on their own (they apply to the canonical
 * and effective stat blocks alike, so they cancel out of neither ratio), but
 * the *level*, which is what the nameplate and con colour show, does not.
 *
 * So an elite or rare presents a few levels above the band instead of inside
 * it. The offset is measured against the band midpoint rather than invented per
 * rank: a creature is "special" exactly to the extent the world data already
 * says it is unusual for its zone, and where the data says nothing the module
 * says nothing either. It is capped, because an uncapped offset walks straight
 * off the end of creature_classlevelstats.
 */

#include "ZoneScaling.h"

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStructure.h"
#include "Map.h"
#include "SharedDefines.h"

#include <algorithm>

char const* ZoneScalingMapKindName(ZoneScalingMapKind kind)
{
    switch (kind)
    {
        case ZS_MAP_WORLD:        return "world";
        case ZS_MAP_DUNGEON:      return "dungeon";
        case ZS_MAP_RAID:         return "raid";
        case ZS_MAP_BATTLEGROUND: return "battleground";
        case ZS_MAP_ARENA:        return "arena";
        default:                  return "unknown";
    }
}

ZoneScalingMapKind ZoneScalingMgr::ResolveMapKind(Map const* map)
{
    if (!map)
        return ZS_MAP_WORLD;

    // Ordered most specific first, and each test is exclusive of the ones above
    // it *given this order* -- IsRaid() before IsNonRaidDungeon() is not
    // strictly required (the two are disjoint) but keeps the reading honest
    // next to the IsDungeon() trap this whole function exists to sidestep.
    if (map->IsBattleArena())
        return ZS_MAP_ARENA;

    if (map->IsBattleground())
        return ZS_MAP_BATTLEGROUND;

    if (map->IsRaid())
        return ZS_MAP_RAID;

    if (map->IsNonRaidDungeon())
        return ZS_MAP_DUNGEON;

    return ZS_MAP_WORLD;
}

bool ZoneScalingMgr::IsMapAllowed(Map const* map) const
{
    // A null map is refused rather than treated as open world. Anything without
    // a map is not somewhere a player can meet it, and defaulting an unknown
    // location to the one kind that is enabled by default is how instanced
    // content gets scaled by accident.
    if (!map)
        return false;

    return IsMapKindEnabled(ResolveMapKind(map));
}

uint8 ZoneScalingMgr::EliteOffsetFor(uint32 zoneId, uint8 canonicalLevel, uint8 rank) const
{
    if (!_preserveEliteOffset || !_eliteOffsetMax || !canonicalLevel)
        return 0;

    // World bosses are excluded from scaling entirely, so they never reach
    // here; the case is listed anyway because a reader checking "what happens
    // to a boss" should find the answer in this function rather than have to
    // know about ZS_EXCL_WORLDBOSS. CREATURE_UNKNOWN is treated as normal.
    switch (rank)
    {
        case CREATURE_ELITE_ELITE:
        case CREATURE_ELITE_RAREELITE:
        case CREATURE_ELITE_RARE:
            break;
        default:
            return 0;
    }

    // Measured against the midpoint, not the band ceiling. The ceiling would
    // report every creature in a normal zone as unremarkable -- almost nothing
    // spawns above its zone's top -- and the offset would be zero everywhere
    // it was wanted. The midpoint asks the question that actually matters: is
    // this creature high for its zone?
    Optional<uint8> const midpoint = GetBandMidpoint(zoneId);
    if (!midpoint || canonicalLevel <= *midpoint)
        return 0;

    return uint8(std::min<uint32>(uint32(canonicalLevel) - uint32(*midpoint),
                                  uint32(_eliteOffsetMax)));
}
