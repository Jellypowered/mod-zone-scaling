/*
 * This file is part of mod-zone-scaling.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * Z-14: the per-viewer level and armor, answered through the core.
 *
 * Everything before this item was reachable from module hooks: damage in both
 * directions, the four melee skill values, the level field in the update
 * packet. What was not reachable is documented at the top of
 * ZoneScalingAggro.cpp -- `Unit::getLevelForTarget` is a core virtual, and the
 * spell hit and resistance tables, the glancing and crushing gates, stealth
 * detection, aggro radius, kill XP and reputation all read it with no hook in
 * between. So the module corrected white hits and left the rest canonical
 * ("only melee hit chance is corrected", README Limitations, until now).
 *
 * The answer is two resolver slots in the core (`CreatureView.h`, installed
 * by `patches/zs_per_viewer_level_apply.py`): `Creature::getLevelForTarget`
 * asks the level resolver before falling back to its own level, and
 * `Unit::CalcArmorReducedDamage` asks the armor resolver before reading the
 * object's armor. This file installs the module's answers into those slots.
 * Nothing about the creature object changes; two characters still fight one
 * creature at two levels, and now every level-derived roll agrees with the
 * level each of them is shown.
 *
 * The design is from https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/4406
 * ("feat(scaling): Level scaling and the Destiny Weavers", bozo-1, 2026-09-20),
 * whose `LocalLevelScaling.h` resolvers and `destiny_weaver_scaling.cpp`
 * `ViewLevelForCore` / `ViewArmorFor` this reproduces over this module's own
 * effective-level and stat machinery. With thanks.
 *
 * Builds without the core patch: `__has_include` leaves the installer empty
 * and `.zs status` reports the resolvers absent, which keeps the "builds
 * against clean upstream" claim true.
 */

#include "ZoneScaling.h"
#include "Creature.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#if __has_include("CreatureView.h")
#include "CreatureView.h"
#define ZS_HAVE_CREATURE_VIEW 1
#endif

namespace
{
#ifdef ZS_HAVE_CREATURE_VIEW
    // The core calls these from inside getLevelForTarget and armor mitigation,
    // which are hot and re-entrant-sensitive: neither may call back into
    // anything that asks a creature its level for a target. GetViewLevelFor is
    // table arithmetic plus the player's Chrono state, and ComputeStats is a
    // base-stats lookup; neither touches getLevelForTarget.
    uint8 ViewLevel(Player const* viewer, Creature const* creature)
    {
        if (!viewer || !creature || !sZoneScalingMgr->IsCoreViewEnabled())
            return 0;

        Optional<uint8> const effective = sZoneScalingMgr->GetViewLevelFor(creature, viewer);
        return effective ? *effective : 0;
    }

    uint32 ViewArmor(Player const* viewer, Creature const* creature)
    {
        if (!viewer || !creature || !sZoneScalingMgr->IsCoreViewEnabled())
            return 0;

        return sZoneScalingMgr->GetViewArmorFor(creature, viewer);
    }
#endif

    void Install()
    {
#ifdef ZS_HAVE_CREATURE_VIEW
        CreatureView::LevelOwner.store(&ViewLevel, std::memory_order_relaxed);
        CreatureView::ArmorOwner.store(&ViewArmor, std::memory_order_relaxed);
        sZoneScalingMgr->SetCoreViewInstalled(true);
        LOG_INFO("module.zonescaling",
            "core view resolvers installed (Z-14): getLevelForTarget and armor follow the effective level{}",
            sZoneScalingMgr->IsCoreViewEnabled() ? "" : " -- but ZoneScaling.CoreView = 0, so they answer 0");
#else
        sZoneScalingMgr->SetCoreViewInstalled(false);
        LOG_INFO("module.zonescaling",
            "core view resolvers absent: this core has no CreatureView.h (Z-14 patch not applied); "
            "spell hit, resists, glancing/crushing gates and aggro radius stay canonical");
#endif
    }
}

class ZoneScalingViewWorldScript : public WorldScript
{
public:
    ZoneScalingViewWorldScript() : WorldScript("ZoneScalingViewWorldScript",
        { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_SHUTDOWN }) { }

    void OnStartup() override { Install(); }

    void OnShutdown() override
    {
#ifdef ZS_HAVE_CREATURE_VIEW
        CreatureView::LevelOwner.store(nullptr, std::memory_order_relaxed);
        CreatureView::ArmorOwner.store(nullptr, std::memory_order_relaxed);
#endif
    }
};

void AddZoneScalingViewScripts()
{
    new ZoneScalingViewWorldScript();
}
