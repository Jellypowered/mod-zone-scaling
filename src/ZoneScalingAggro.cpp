/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Proximity aggro under scaling -- and the one milestone whose stated goal this
 * module cannot reach. Read this before assuming the code below is a
 * half-effort; it is the most that is reachable, and the shortfall is
 * structural rather than a matter of trying harder.
 *
 * WHAT SHOULD HAPPEN. Aggro radius in this core is
 *
 *     radius = detectionRange - (playerLevel - creatureLevel)
 *
 * clamped to [5, 45] (Creature::GetAggroRange, Creature.cpp:3396 -- note its
 * two level variables are named the wrong way round, `creatureLevel` holding
 * the target's level and `playerLevel` the creature's). Under full scaling the
 * creature presents at the player's own level, so the level term should vanish
 * and every player should be aggroed at exactly `detectionRange`, whoever they
 * are. That is the whole feature.
 *
 * WHY IT CANNOT BE DONE FROM A MODULE. The term is computed from
 * `getLevelForTarget`, and every route to it is closed:
 *
 *   - There is no script hook anywhere on `Creature::GetAggroRange`, on
 *     `Creature::GetAttackDistance` (Creature.cpp:3606, the same formula again
 *     for stealth-alert range), or on `Creature::CanStartAttack` which calls
 *     it. Checked against the full hook lists of AllCreatureScript, UnitScript,
 *     PlayerScript and FormulaScript.
 *   - `Unit::getLevelForTarget` is virtual but is a core class method
 *     (Unit.h:1105, Creature override at Creature.cpp:3158 for world bosses).
 *     A module cannot override it.
 *   - `AllCreatureScript::GetCreatureAI` would let a module substitute an AI
 *     and reimplement the range test, but only by replacing the AI of every
 *     creature in the world, which would displace SmartAI and every scripted
 *     encounter. That is not a trade any published module should make.
 *
 * The only per-observer lever inside the formula is
 * `target->GetTotalAuraModifier(SPELL_AURA_MOD_DETECTED_RANGE)` -- an aura on
 * the *player*, whose amount is therefore the same against every creature they
 * meet. It would be exactly right under `CanonicalLevel = midpoint`, where all
 * creatures in a zone share one level, and wrong everywhere else. Buying that
 * one configuration costs a permanent hidden aura on every player, reapplied on
 * every level and zone change, spilling across zone borders. Evaluated and
 * rejected; the reasoning is in the build plan under Z-06.
 *
 * WHAT IS REACHABLE, AND IS WHAT THIS FILE DOES. `SetDetectionDistance` is
 * public and per creature. A per-creature constant cannot remove the dependence
 * on the *player's* level -- nothing per-creature can, since the radius moves
 * one yard per player level no matter what constant it starts from. What it can
 * do is remove the dependence on the *creature's* level, by absorbing it:
 *
 *     detectionRange := templateRange + (bandMidpoint - creatureLevel)
 *     radius          = templateRange + bandMidpoint - playerLevel
 *
 * The creature's own level drops out. Every creature in a scaled zone then
 * aggroes the same player at the same distance, which is what players actually
 * perceive as correct or broken -- stock behaviour in a scaled zone has a
 * level-8 boar grabbing a level-60 at 5 yards while the level-25 ogre next to
 * it reaches 22, for no reason visible in the world. The residual error against
 * the ideal is `|playerLevel - bandMidpoint|` yards, identical for every
 * creature in the zone, and zero for a player at the middle of the band.
 *
 * This is off by default. It is a partial fix to a problem the module cannot
 * fully solve, and it writes to creature state rather than presenting a view,
 * so it should be an explicit choice. `.zs aggro` prints all three radii --
 * stock, normalized and ideal -- so the trade can be measured before it is
 * taken.
 */

#include "ZoneScaling.h"

#include "Creature.h"
#include "CreatureData.h"
#include "Unit.h"
#include "World.h"

#include <algorithm>

Optional<uint8> ZoneScalingMgr::GetBandMidpoint(uint32 zoneId) const
{
    ZoneScalingBand const* band = GetBand(zoneId);
    if (!band)
        return std::nullopt;

    return uint8((uint32(band->MinLevel) + uint32(band->MaxLevel)) / 2);
}

Optional<float> ZoneScalingMgr::NormalizedDetectionRange(CreatureTemplate const* cinfo,
                                                         uint32 zoneId,
                                                         uint8 canonicalLevel) const
{
    // Deliberately not gated on _aggroMode. This is the predictor as well as the
    // producer: `.zs aggro` has to be able to show what normalization *would*
    // give while the mode is still off, which is the whole point of being able
    // to measure the trade before taking it. The mode check lives in
    // ApplyAggroRange, the only place that writes to a creature.
    if (!cinfo || !canonicalLevel)
        return std::nullopt;

    Optional<uint8> const midpoint = GetBandMidpoint(zoneId);
    if (!midpoint)
        return std::nullopt;

    float const offset = float(int32(*midpoint) - int32(canonicalLevel));
    if (offset == 0.0f)
        return std::nullopt;

    // Clamped at the bottom only, and the asymmetry is the point. Below 1 the
    // creature stops aggroing entirely -- GetAggroRange returns 0.0f outright
    // rather than clamping -- so that floor has to be respected here.
    //
    // Clamping the top at MAX_AGGRO_RADIUS looks equally reasonable and is
    // wrong. The core clamps the *result* of the subtraction, not its input; an
    // offset creature is one whose stored range is deliberately inflated so
    // that subtracting its level difference lands back on the shared answer.
    // Capping the stored value at 45 throws away exactly the headroom the
    // subtraction is about to consume, and the amount thrown away depends on
    // the creature's own level -- which reintroduces the dependence this whole
    // function exists to remove. Measured: with the cap, a level-12 and a
    // level-6 creature in the same band gave 17 and 11 yards against the same
    // player instead of both giving 18.
    //
    // The stored value may therefore exceed 45. That is safe and was checked:
    // Creature::GetAggroRange (Creature.cpp:3414) is the only reader of
    // GetDetectionRange in the entire core, and it caps its own output.
    return std::max(cinfo->detection_range + offset, 1.0f);
}

void ZoneScalingMgr::ApplyAggroRange(Creature* creature, ZoneScalingCreatureState const& state) const
{
    if (_aggroMode != ZS_AGGRO_NORMALIZE || !creature || !state.Scalable)
        return;

    Optional<float> const range =
        NormalizedDetectionRange(creature->GetCreatureTemplate(), state.ZoneId, state.CanonicalLevel);

    if (!range)
        return;

    creature->SetDetectionDistance(*range);
    CountAggroAdjusted();
}

void ZoneScalingMgr::RestoreAggroRange(Creature* creature) const
{
    // Unconditional on the mode, not gated on it. `.reload config` can turn
    // normalization off while creatures adjusted under it are still in the
    // world, and a restore that only ran in normalize mode would strand them.
    // (The converse cannot be fixed the same way: there is no way to enumerate
    // tagged creatures, so switching the mode *on* reaches each creature only
    // when it next respawns or its grid reloads. Documented, not solved.)
    if (!creature)
        return;

    if (CreatureTemplate const* cinfo = creature->GetCreatureTemplate())
        creature->SetDetectionDistance(cinfo->detection_range);
}

float ZoneScalingMgr::PredictAggroRadius(float detectionRange, uint8 playerLevel,
                                         uint8 creatureLevel) const
{
    float const aggroRate = sWorld->getRate(RATE_CREATURE_AGGRO);
    if (aggroRate == 0.0f)
        return 0.0f;

    if (detectionRange < 1.0f)
        return 0.0f;

    int32 levelDiff = int32(playerLevel) - int32(creatureLevel);

    // The core's cap on how far the radius may *grow* when the player is far
    // below the creature: 25 levels, so at most detectionRange + 25.
    if (levelDiff < -25)
        levelDiff = -25;

    float radius = detectionRange - float(levelDiff);

    if (radius > MAX_AGGRO_RADIUS)
        radius = MAX_AGGRO_RADIUS;

    // 5 yards for anything that is not a pet. Pets floor at 10, which this
    // module never has in hand -- pets are resolved to their owner and are
    // never tagged.
    if (radius < 5.0f)
        radius = 5.0f;

    return radius * aggroRate;
}
