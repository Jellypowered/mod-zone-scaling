#!/usr/bin/env python3
"""Z-14: teach the core to ask a scaling module for a viewer's creature level and armor.

Run ON THE BOX from the module directory it was copied with:

    python3 ~/azerothcore-wotlk/modules/mod-zone-scaling/patches/zs_per_viewer_level_apply.py

Idempotent: every substitution checks for its own result first. Installs
`patches/CreatureView.h` as `src/server/game/Miscellaneous/CreatureView.h` and
routes seven core call sites through it. The same change as a unified diff is
`zs_per_viewer_level.patch`, for `git apply` on a clean tree; this script is the
form that survives the working tree already carrying other local patches.

Derived from https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/4406
(bozo-1, commit 1c1235abc): the resolver design and each call site below are
that PR's, minus its realm-wide lift, quest keep-shares and Weaver NPCs.
"""
from pathlib import Path
import shutil
import sys

ROOT = Path.home() / "azerothcore-wotlk"
HERE = Path(__file__).resolve().parent
ATTR = "Z-14, from CoA PR #4406 (bozo-1)"


def sub(rel, old, new, count=1):
    p = ROOT / rel
    s = p.read_text(encoding="utf-8")
    if new in s:
        print("already applied:", rel, "|", old.strip().splitlines()[0][:60])
        return
    if s.count(old) != count:
        sys.exit(f"{rel}: expected {count} occurrence(s) of {old.strip()[:70]!r}, found {s.count(old)}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print("patched:", rel, "|", old.strip().splitlines()[0][:60])


def include(rel, after, header="CreatureView.h"):
    line = f'#include "{header}"\n'
    p = ROOT / rel
    s = p.read_text(encoding="utf-8")
    if line in s:
        print("include present:", rel)
        return
    anchor = f'#include "{after}"\n'
    assert s.count(anchor) == 1, (rel, after)
    p.write_text(s.replace(anchor, anchor + line), encoding="utf-8")
    print("include added:", rel)


# 0. The header itself.
dst = ROOT / "src/server/game/Miscellaneous/CreatureView.h"
src = HERE / "CreatureView.h"
if dst.exists() and dst.read_bytes() == src.read_bytes():
    print("header present:", dst)
else:
    shutil.copyfile(src, dst)
    print("header installed:", dst)

# 1. Creature::getLevelForTarget -- the lever everything else hangs off.
include("src/server/game/Entities/Creature/Creature.cpp", "CreatureGroups.h")
sub("src/server/game/Entities/Creature/Creature.cpp",
    "uint8 Creature::getLevelForTarget(WorldObject const* target) const\n"
    "{\n"
    "    if (!isWorldBoss() || !target->ToUnit())\n"
    "        return Unit::getLevelForTarget(target);\n",
    "uint8 Creature::getLevelForTarget(WorldObject const* target) const\n"
    "{\n"
    "    if (!isWorldBoss() || !target || !target->ToUnit())\n"
    "    {\n"
    f"        // {ATTR}: a character a scaling module has given a view of this\n"
    "        // creature fights that version, and the whole core asks this function for\n"
    "        // the level a fight is rolled at -- spell hit and resistance, weapon and\n"
    "        // defence skill, glancing and crushing, detection, aggro radius, kill XP.\n"
    "        if (target)\n"
    "            if (Unit const* opponent = target->ToUnit())\n"
    "                if (Player const* viewer = opponent->GetCharmerOrOwnerPlayerOrPlayerItself())\n"
    "                    if (uint8 const view = CreatureView::LevelFor(viewer, this))\n"
    "                        return view;\n"
    "\n"
    "        return Unit::getLevelForTarget(target);\n"
    "    }\n")

# 2. Unit: armor mitigation, block chance and non-weapon ranged skill.
include("src/server/game/Entities/Unit/Unit.cpp", "CharmInfo.h")
sub("src/server/game/Entities/Unit/Unit.cpp",
    "    float armor = float(victim->GetArmor());\n\n    // Ignore enemy armor by SPELL_AURA_MOD_TARGET_RESISTANCE aura\n",
    "    float armor = float(victim->GetArmor());\n\n"
    f"    // {ATTR}: a creature shown at the viewer's level is defended like that\n"
    "    // level's creature_classlevelstats row, not like the authored one.\n"
    "    if (Creature const* creature = victim->ToCreature())\n"
    "        if (Player const* viewer = attacker ? attacker->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr)\n"
    "            if (uint32 const viewArmor = CreatureView::ArmorFor(viewer, creature))\n"
    "                armor = float(viewArmor);\n\n"
    "    // Ignore enemy armor by SPELL_AURA_MOD_TARGET_RESISTANCE aura\n")
sub("src/server/game/Entities/Unit/Unit.cpp",
    "        blockChance += (int32(GetWeaponSkillValue(attackType)) - int32(victim->GetMaxSkillValueForLevel())) * 0.04f;\n",
    f"        // {ATTR}: both skills relative to the other side, so a view applies.\n"
    "        blockChance += (int32(GetWeaponSkillValue(attackType, victim)) - int32(victim->GetMaxSkillValueForLevel(this))) * 0.04f;\n")
sub("src/server/game/Entities/Unit/Unit.cpp",
    "    if (spellInfo->DmgClass == SPELL_DAMAGE_CLASS_RANGED && !spellInfo->IsRangedWeaponSpell())\n"
    "        attackerWeaponSkill = GetLevel() * 5;\n",
    "    if (spellInfo->DmgClass == SPELL_DAMAGE_CLASS_RANGED && !spellInfo->IsRangedWeaponSpell())\n"
    f"        attackerWeaponSkill = getLevelForTarget(victim) * 5;   // {ATTR}\n")

# 3. Kill rewards: gray is decided per member against the level they fight.
sub("src/server/game/Entities/Player/KillRewarder.cpp",
    "                        if (_victim->GetLevel() > grayLevel && (!_maxNotGrayMember || _maxNotGrayMemberLevel < lvl))\n",
    f"                        // {ATTR}: gray per member, at the level each of them fights the victim.\n"
    "                        if (_victim->getLevelForTarget(member) > grayLevel && (!_maxNotGrayMember || _maxNotGrayMemberLevel < lvl))\n")
sub("src/server/game/Entities/Player/KillRewarder.cpp",
    "            if (highestLevel > referenceLevel && creature->GetLevel() <= Acore::XP::GetGrayLevel(highestLevel))\n",
    "            if (highestLevel > referenceLevel && creature->getLevelForTarget(player) <= Acore::XP::GetGrayLevel(highestLevel))   // "
    + ATTR + "\n")

# 4. Kill XP base gain.
sub("src/server/game/Miscellaneous/Formulas.cpp",
    "        gain = BaseGain(playerLevel, unit->GetLevel(), GetContentLevelsForMapAndZone(unit->GetMapId(), unit->GetZoneId()));\n",
    f"        // {ATTR}: the level the killer fought, not the object's own.\n"
    "        gain = BaseGain(playerLevel, unit->getLevelForTarget(player), GetContentLevelsForMapAndZone(unit->GetMapId(), unit->GetZoneId()));\n")

# 5. Reputation and the "grants XP or honor" gate.
sub("src/server/game/Entities/Player/Player.cpp",
    "        float donerep1 = CalculateReputationGain(REPUTATION_SOURCE_KILL, victim->GetLevel(), static_cast<float>(Rep->RepValue1),",
    "        float donerep1 = CalculateReputationGain(REPUTATION_SOURCE_KILL, victim->getLevelForTarget(this), static_cast<float>(Rep->RepValue1),")
sub("src/server/game/Entities/Player/Player.cpp",
    "        float donerep2 = CalculateReputationGain(REPUTATION_SOURCE_KILL, victim->GetLevel(), static_cast<float>(Rep->RepValue2),",
    "        float donerep2 = CalculateReputationGain(REPUTATION_SOURCE_KILL, victim->getLevelForTarget(this), static_cast<float>(Rep->RepValue2),")
sub("src/server/game/Entities/Player/Player.cpp",
    "bool Player::isHonorOrXPTarget(Unit* victim) const\n{\n    uint8 v_level = victim->GetLevel();\n",
    "bool Player::isHonorOrXPTarget(Unit* victim) const\n{\n"
    f"    // {ATTR}: the level this character fights, so a scaled creature is not gray to them.\n"
    "    uint8 v_level = victim->getLevelForTarget(this);\n")

print("Z-14 core changes applied")
