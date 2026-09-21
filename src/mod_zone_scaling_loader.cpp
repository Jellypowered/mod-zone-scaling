/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

// The name is fixed by the core's build system: modules/CMakeLists.txt derives
// it from the directory name with '-' replaced by '_'.
//
// Scripts are registered only once they do something. An AzerothCore script
// constructed with an empty enabledHooks vector opts in to *every* hook of its
// type (see WorldScript::WorldScript), so a placeholder script is not free --
// the Unit and Player scripts arrive with Z-04 and Z-08 respectively, each
// naming the hooks it actually uses.

void AddZoneScalingMgrScripts();
void AddZoneScalingCommandScripts();
void AddZoneScalingCreatureScripts();
void AddZoneScalingCombatScripts();
void AddZoneScalingDisplayScripts();
void AddZoneScalingChronoScripts();
void AddZoneScalingRewardScripts();
void AddZoneScalingLootScripts();
void AddZoneScalingGossipScripts();
void AddZoneScalingViewScripts();

void Addmod_zone_scalingScripts()
{
    AddZoneScalingMgrScripts();
    AddZoneScalingCommandScripts();
    AddZoneScalingCreatureScripts();
    AddZoneScalingCombatScripts();
    AddZoneScalingDisplayScripts();
    AddZoneScalingChronoScripts();
    AddZoneScalingRewardScripts();
    AddZoneScalingLootScripts();
    AddZoneScalingGossipScripts();
    AddZoneScalingViewScripts();
}
