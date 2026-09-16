/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Per-creature scaling state: when it is attached, refreshed and released.
 *
 * Nothing here changes how a creature behaves. It only decides what each
 * creature's *own* level is and caches the stats that go with it, so that the
 * later milestones have a fixed baseline to compute ratios against.
 */

#include "ZoneScaling.h"

#include "AllCreatureScript.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Log.h"
#include "ScriptMgr.h"

ZoneScalingCreatureState* ZoneScalingMgr::GetState(Creature* creature)
{
    if (!creature)
        return nullptr;

    return creature->CustomData.Get<ZoneScalingCreatureState>(ZoneScalingCreatureState::Key);
}

void ZoneScalingMgr::BuildState(Creature* creature, ZoneScalingCreatureState& state) const
{
    CreatureTemplate const* cinfo = creature->GetCreatureTemplate();

    state.ZoneId         = creature->GetZoneId();
    state.CanonicalLevel = creature->GetLevel();
    state.Exclusion      = GetExclusion(cinfo);
    state.Rank           = cinfo ? cinfo->rank : 0;
    state.MapKind        = ResolveMapKind(creature->FindMap());

    // Scalable means "this creature is a candidate", not "this creature will
    // move". Whether it moves depends on the observing player, and no player
    // is in hand here.
    //
    // The map test is repeated from IsCandidate rather than assumed from it.
    // Scalable is what every downstream stage reads -- damage, hit table, level
    // display, aggro -- so it has to carry the whole answer on its own; relying
    // on the caller having already checked would make the safety of the
    // instanced-content gate depend on which path reached this function.
    state.Scalable = state.Exclusion == ZS_NOT_EXCLUDED
        && IsMapAllowed(creature->FindMap())
        && GetBand(state.ZoneId, creature->FindMap()) != nullptr;

    state.StatsValid = ComputeStats(cinfo, state.CanonicalLevel, state.CanonicalStats);
}

bool ZoneScalingMgr::IsCandidate(Creature* creature) const
{
    if (!IsActive() || !creature)
        return false;

    // Pets, guardians and totems belong to their owner and are scaled through
    // the owner's ratio in Z-04. Tagging them would double-count.
    if (creature->IsPet() || creature->IsSummon())
        return false;

    // Before the exclusion and band lookups, because it is the cheapest of the
    // three and rejects entire maps at once.
    if (!IsMapAllowed(creature->FindMap()))
        return false;

    if (GetExclusion(creature->GetCreatureTemplate()) != ZS_NOT_EXCLUDED)
        return false;

    return GetBand(creature->GetZoneId(), creature->FindMap()) != nullptr;
}

void ZoneScalingMgr::OnAddWorld(Creature* creature)
{
    // Only candidates get a state block. Tagging everything would cost ~40k
    // allocations on this realm to describe creatures nothing will ever scale,
    // and would make the tagged count useless as a bound on what the module is
    // actually carrying. The cost of the narrower rule is that `.zs info` on an
    // untagged creature has to recompute the reason for display, which is fine
    // -- that path is a human typing a command, not a hot one.
    if (!IsCandidate(creature))
        return;

    // Presence of the state block is the tagged/not-tagged test, rather than
    // any sentinel value inside it. GetDefault would be shorter but would give
    // no way to tell a fresh block from a refreshed one, and the counter has
    // to know the difference or a re-entered creature double-counts.
    ZoneScalingCreatureState* state = GetState(creature);
    bool const isNew = !state;

    if (isNew)
    {
        state = new ZoneScalingCreatureState();
        creature->CustomData.Set(ZoneScalingCreatureState::Key, state);
    }

    BuildState(creature, *state);

    if (isNew)
        _taggedCount.fetch_add(1, std::memory_order_relaxed);

    ApplyAggroRange(creature, *state);

    if (_debug)
        LOG_DEBUG("module.zonescaling",
            "tag {} '{}' entry {} zone {} canonical level {} scalable {} stats {}",
            creature->GetGUID().ToString(), creature->GetName(), creature->GetEntry(),
            state->ZoneId, uint32(state->CanonicalLevel),
            state->Scalable ? "yes" : "no", state->StatsValid ? "ok" : "UNUSABLE");
}

void ZoneScalingMgr::OnRemoveWorld(Creature* creature)
{
    if (!creature)
        return;

    // Deliberately not gated on IsActive(). Disabling the module at runtime
    // must not strand state on creatures tagged while it was on, or the count
    // never returns to zero and the leak metric becomes a lie.
    if (!GetState(creature))
        return;

    RestoreAggroRange(creature);

    creature->CustomData.Erase(ZoneScalingCreatureState::Key);
    _taggedCount.fetch_sub(1, std::memory_order_relaxed);
}

void ZoneScalingMgr::OnSelectLevel(Creature* creature)
{
    if (!IsActive() || !creature)
        return;

    // Only refresh what is already tagged. SelectLevel also runs during
    // creation, before the creature is in the world and before OnAddWorld has
    // decided whether it is a candidate at all; tagging from here would attach
    // state to pets and to creatures that are about to fail creation.
    ZoneScalingCreatureState* state = GetState(creature);
    if (!state)
    {
        // A creature being created is not yet in the world, and is expected to
        // land here untagged -- that is the normal case and is not worth a
        // line. One already in the world arriving untagged is not normal, and
        // means a creature is changing level with no cache to update.
        if (_debug && creature->IsInWorld())
            LOG_DEBUG("module.zonescaling",
                "selectlevel on untagged in-world creature {} '{}' entry {} zone {} level {}",
                creature->GetGUID().ToString(), creature->GetName(), creature->GetEntry(),
                creature->GetZoneId(), uint32(creature->GetLevel()));

        return;
    }

    uint8 const previous = state->CanonicalLevel;

    BuildState(creature, *state);

    // Must follow BuildState, and must be here rather than only in OnAddWorld:
    // Creature::UpdateEntry calls InitEntry -- which resets detection range to
    // the template's value (Creature.cpp:689) -- and only then SelectLevel.
    // This hook is the first module-visible point after that overwrite, so it
    // is what makes the aggro offset survive respawns and entry changes.
    ApplyAggroRange(creature, *state);

    // Logged even when the level is unchanged. A re-roll over a narrow range
    // often lands on the same number, so "no relevel line" would otherwise be
    // indistinguishable from "the hook never fired" -- and those two need very
    // different responses.
    if (_debug)
        LOG_DEBUG("module.zonescaling",
            "{} {} '{}' entry {} canonical level {} -> {}",
            previous == state->CanonicalLevel ? "reselect" : "relevel",
            creature->GetGUID().ToString(), creature->GetName(), creature->GetEntry(),
            uint32(previous), uint32(state->CanonicalLevel));
}

void ZoneScalingMgr::OnBeforeSelectLevel(CreatureTemplate const* cinfo, Creature* creature,
                                         uint8& level) const
{
    if (!IsActive() || !creature || !cinfo)
        return;

    // Under 'native' this hook has nothing to say, and it is on the creation
    // path for every creature in the world -- it fired 15,200 times in a few
    // minutes on a realm with one zone banded. So it returns before doing any
    // work, and logs only where it actually overrides something. The bare roll
    // is not logged: the tag line already reports the level that resulted, and
    // the template range it came from is a column in creature_template.
    if (_canonical != ZS_CANON_MIDPOINT)
        return;

    if (GetExclusion(cinfo) != ZS_NOT_EXCLUDED)
        return;

    // Safe this early: Creature::Create relocates before CreateFromProto
    // precisely so the zone id is available here, and says so in a comment.
    // Guard the map anyway -- a creature being created without one would take
    // the worldserver down rather than merely misbehave.
    if (!creature->FindMap())
        return;

    // Its own gate, not inherited from anywhere. This hook rewrites the
    // creature's real level at creation, so it runs before any state block
    // exists and never consults Scalable -- which means the instanced-content
    // check has to be made here in its own right. Missing it would permanently
    // relevel dungeon and raid creatures under CanonicalLevel = midpoint while
    // every other stage correctly left them alone.
    if (!IsMapAllowed(creature->FindMap()))
        return;

    ZoneScalingBand const* band = GetBand(creature->GetZoneId(), creature->FindMap());
    if (!band)
        return;

    // Via the shared helper rather than recomputed here, so this and the Z-06
    // aggro offset can never round differently -- under midpoint the offset
    // must be exactly zero, and that has to hold by construction.
    uint8 const midpoint = *GetBandMidpoint(creature->GetZoneId());

    // Never onto a level with no usable stats -- that produces a 1 HP mob.
    if (!HasBaseStats(cinfo, midpoint))
    {
        if (_debug)
            LOG_DEBUG("module.zonescaling",
                "midpoint {} entry {} REFUSED: level {} has no usable base stats for class {} "
                "expansion {}, left at {}",
                creature->GetGUID().ToString(), creature->GetEntry(), uint32(midpoint),
                uint32(cinfo->unit_class), uint32(cinfo->expansion), uint32(level));

        return;
    }

    if (_debug)
        LOG_DEBUG("module.zonescaling",
            "midpoint {} entry {} template {}-{} rolled {} -> {} (zone {} band {}-{})",
            creature->GetGUID().ToString(), creature->GetEntry(),
            uint32(cinfo->minlevel), uint32(cinfo->maxlevel), uint32(level), uint32(midpoint),
            creature->GetZoneId(), uint32(band->MinLevel), uint32(band->MaxLevel));

    level = midpoint;
}

// Registering an AllCreatureScript is not free and cannot be made free: unlike
// WorldScript this type takes no enabledHooks list, so the registration also
// buys a virtual call per creature per tick for OnAllCreatureUpdate. That hook
// is deliberately not overridden; the inherited empty body is the whole cost.
class ZoneScalingCreatureScript : public AllCreatureScript
{
public:
    ZoneScalingCreatureScript() : AllCreatureScript("ZoneScalingCreatureScript") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        sZoneScalingMgr->OnAddWorld(creature);
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        sZoneScalingMgr->OnRemoveWorld(creature);
    }

    void OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature) override
    {
        sZoneScalingMgr->OnSelectLevel(creature);
    }

    void OnBeforeCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* creature,
                                     uint8& level) override
    {
        sZoneScalingMgr->OnBeforeSelectLevel(cinfo, creature, level);
    }
};

void AddZoneScalingCreatureScripts()
{
    new ZoneScalingCreatureScript();
}
