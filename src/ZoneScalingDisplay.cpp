/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Per-observer level display: the milestone that makes the scaling visible.
 *
 * Z-04 made the fight feel right; this makes it *look* right. Without it a
 * level-60 player sees "Level 8" over a creature that is hitting them like a
 * 60, the con colour is grey, and every instinct the client trains -- nameplate
 * colour, tooltip level, "can I pull that" -- points the wrong way.
 *
 * How it is possible at all. AzerothCore builds SMSG_UPDATE_OBJECT per
 * receiver: it assembles one field buffer, caches it per visibility-flag class,
 * and then hands the assembled bytes to `Unit::PatchValuesUpdate` *once per
 * target* so that a handful of fields can be rewritten for that particular
 * observer. Two module-facing hooks sit on that path -- one to have the writer
 * record where a field landed in the buffer, one to rewrite it. That is the
 * entire mechanism, and it is why two players can see two different numbers
 * over the same creature without the creature having two levels.
 *
 * Nothing here is a core modification, and the client is stock: it is simply
 * told a different number, and believes it.
 */

#include "ZoneScaling.h"

#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "UnitScript.h"
#include "UpdateFields.h"

// BuildValuesCachePosPointers lives in UnitUtils.h, which is *not* include
// guarded -- including it directly is a hard error once Unit.h has already
// pulled it in, which Creature.h does. It arrives transitively; do not add it.

#include <list>

namespace
{
    // Same test as the combat path uses, and deliberately so. If display and
    // combat could disagree about whether a creature is scaled, the nameplate
    // would become a lie in exactly the case where a player is most likely to
    // check it -- which is worse than no nameplate scaling at all. In
    // particular StatsValid is required here even though the level field itself
    // needs no stats: a creature whose effective level has no usable
    // classlevelstats row fights at canonical numbers, so it must read as
    // canonical too.
    Creature const* ResolveScaledCreature(Unit const* unit)
    {
        if (!unit || !unit->IsCreature())
            return nullptr;

        Creature const* creature = unit->ToCreature();
        if (creature->GetCharmerOrOwnerPlayerOrPlayerItself())
            return nullptr;

        ZoneScalingCreatureState const* state =
            ZoneScalingMgr::GetState(const_cast<Creature*>(creature));

        return (state && state->Scalable && state->StatsValid) ? creature : nullptr;
    }

    // Every creature within a radius, regardless of entry. The core's
    // WorldObject::GetCreaturesWithEntryInRange looks close but matches on an
    // exact entry and has no "any" form, so the check functor is spelled out.
    struct AnyCreatureInRange
    {
        AnyCreatureInRange(WorldObject const* source, float range) : _source(source), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return _source->IsWithinDist(creature, _range, false);
        }

    private:
        WorldObject const* _source;
        float _range;
    };
}

class ZoneScalingDisplayScript : public UnitScript
{
public:
    ZoneScalingDisplayScript() : UnitScript("ZoneScalingDisplayScript", true,
        {
            UNITHOOK_SHOULD_TRACK_VALUES_UPDATE_POS_BY_INDEX,
            UNITHOOK_ON_PATCH_VALUES_UPDATE
        }) { }

    // Called once per field index per fresh buffer build, for every unit in the
    // world -- easily the hottest thing this module registers. The index test
    // comes first and rejects all but one of ~140 indices with a single compare,
    // before any config read or pointer chase.
    [[nodiscard]] bool ShouldTrackValuesUpdatePosByIndex(Unit const* unit, uint8 /*updateType*/,
                                                         uint16 index) override
    {
        // Z-14: the health and mana bars are patched per observer as well, so
        // the pool a player watches is their version's (from CoA PR #4406).
        bool const pool = index == UNIT_FIELD_MAXHEALTH || index == UNIT_FIELD_HEALTH
                       || index == UNIT_FIELD_MAXPOWER1 || index == UNIT_FIELD_POWER1;
        if (index != UNIT_FIELD_LEVEL && !pool)
            return false;

        if (!sZoneScalingMgr->IsLevelDisplayEnabled())
            return false;

        if (pool && !sZoneScalingMgr->IsPoolDisplayEnabled())
            return false;

        if (!ResolveScaledCreature(unit))
            return false;

        sZoneScalingMgr->CountLevelTracked();
        return true;
    }

    void OnPatchValuesUpdate(Unit const* unit, ByteBuffer& valuesUpdateBuf,
                             BuildValuesCachePosPointers& posPointers, Player* target) override
    {
        if (!target || !sZoneScalingMgr->IsLevelDisplayEnabled())
            return;

        // Absent means the field was not in this update's mask at all, which is
        // the common case: partial updates only carry changed fields. Checking
        // the map first also keeps this hook cheap for every other unit in the
        // world, since it runs for all of them.
        auto const& other = posPointers.other;
        auto const itr = other.find(UNIT_FIELD_LEVEL);
        bool const wantPool = sZoneScalingMgr->IsPoolDisplayEnabled()
            && (other.count(UNIT_FIELD_MAXHEALTH) || other.count(UNIT_FIELD_HEALTH)
                || other.count(UNIT_FIELD_MAXPOWER1) || other.count(UNIT_FIELD_POWER1));
        if (itr == other.end() && !wantPool)
            return;

        Creature const* creature = ResolveScaledCreature(unit);
        if (!creature)
            return;

        // Counted here rather than after the level test, and the distinction is
        // the whole measurement: this is one observer served from one buffer,
        // whether or not the number ends up changing for them. Comparing it to
        // the build count is what makes the cache hit rate visible.
        sZoneScalingMgr->CountLevelServed();

        Optional<uint8> const effective =
            sZoneScalingMgr->GetEffectiveLevelFor(const_cast<Creature*>(creature), target);
        if (!effective)
            return;

        // The position has already had the cache and packet offsets applied by
        // the caller, so it indexes the real buffer.
        if (itr != other.end())
        {
            valuesUpdateBuf.put<uint32>(itr->second, uint32(*effective));
            sZoneScalingMgr->CountLevelPatched();
        }

        if (!wantPool)
            return;

        ZoneScalingMgr::ViewPool pool;
        if (!sZoneScalingMgr->GetViewPool(creature, *effective, pool))
            return;

        auto put = [&](uint16 index, uint32 value)
        {
            auto const p = other.find(index);
            if (p != other.end())
                valuesUpdateBuf.put<uint32>(p->second, value);
        };
        put(UNIT_FIELD_MAXHEALTH, pool.MaxHealth);
        put(UNIT_FIELD_HEALTH, pool.Health);
        if (pool.MaxMana)
        {
            put(UNIT_FIELD_MAXPOWER1, pool.MaxMana);
            put(UNIT_FIELD_POWER1, pool.Mana);
        }
    }
};

// The other half of the milestone, and the less obvious one: a creature only
// resends its level when something marks that field dirty. A player levelling
// up changes nothing about the creature, so without this every mob already on
// screen keeps the number it was sent on spawn -- correct for the player's old
// level, stale for the new one, and stale until the mob leaves view. It looks
// exactly like the patch hook not firing.
//
// Z-08 needs precisely the same thing for precisely the same reason -- toggling
// Chrono Mode changes nothing about any creature either -- so this is a free
// function rather than a method on the level-up script. The alternative was for
// the Chrono command to reimplement the grid search, and two copies of a
// correctness fix is one copy too many.
void ZoneScalingRefreshNearbyLevels(Player* player, char const* why)
{
    if (!player || !player->IsInWorld() || !sZoneScalingMgr->IsLevelDisplayEnabled()
        || !sZoneScalingMgr->IsActive())
        return;

    Map* map = player->GetMap();
    if (!map)
        return;

    // Visibility range, so that everything the player can currently see is
    // covered and nothing further is touched. This is a grid search, which
    // would be indefensible per tick and is unremarkable per level-up -- and a
    // Chrono toggle is rarer than a level-up.
    float const range = map->GetVisibilityRange();

    std::list<Creature*> creatures;
    AnyCreatureInRange check(player, range);
    Acore::CreatureListSearcher<AnyCreatureInRange> searcher(player, creatures, check);
    Cell::VisitObjects(player, searcher, range);

    uint32 refreshed = 0;
    for (Creature* creature : creatures)
    {
        if (!ResolveScaledCreature(creature))
            continue;

        // Marks the field dirty and queues the creature for the next update
        // flush. That flush rebuilds the buffer, which re-runs the track hook
        // and then the patch hook per observer -- so every *other* player
        // watching this creature is re-served their own correct number in the
        // same pass, not this player's.
        //
        // The core's per-tick values cache cannot serve a stale buffer here:
        // it is cleared at the end of each Unit::Update, and level changes
        // resolve during the map update that precedes the update flush.
        // Unit::InvalidateValuesUpdateCache is private, so this ordering is
        // relied on rather than enforced.
        creature->ForceValuesUpdateAtIndex(UNIT_FIELD_LEVEL);
        if (sZoneScalingMgr->IsPoolDisplayEnabled())
        {
            creature->ForceValuesUpdateAtIndex(UNIT_FIELD_MAXHEALTH);
            creature->ForceValuesUpdateAtIndex(UNIT_FIELD_HEALTH);
        }
        ++refreshed;
    }

    if (sZoneScalingMgr->IsDebug())
        LOG_DEBUG("module.zonescaling",
            "level display: {} for player {} (level {}), refreshed {} of {} nearby creature(s) within {:.1f} yd",
            why, player->GetName(), uint32(player->GetLevel()), refreshed, creatures.size(), range);
}

class ZoneScalingDisplayPlayerScript : public PlayerScript
{
public:
    ZoneScalingDisplayPlayerScript() : PlayerScript("ZoneScalingDisplayPlayerScript",
        { PLAYERHOOK_ON_LEVEL_CHANGED }) { }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override
    {
        ZoneScalingRefreshNearbyLevels(player, "level change");
    }
};

void AddZoneScalingDisplayScripts()
{
    new ZoneScalingDisplayScript();
    new ZoneScalingDisplayPlayerScript();
}
