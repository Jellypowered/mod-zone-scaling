/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Per-viewer creature level and armor: the two resolver slots a scaling
 * module fills so that one character can fight *their* version of a creature
 * while the creature object itself is never changed.
 *
 * Derived from the `LocalLevelScaling.h` resolver design in
 * https://github.com/jealous-sound/azerothcore-wotlk-coa/pull/4406
 * ("feat(scaling): Level scaling and the Destiny Weavers", author bozo-1,
 * commit 1c1235abc, 2026-09-20). The realm-wide lift, the quest-reward
 * keep-shares and the Weaver NPCs of that PR are not carried; only the
 * per-viewer level and armor resolvers and the core call sites that consult
 * them. Ported for mod-zone-scaling (Z-14) 2026-09-20.
 *
 * `Unit::getLevelForTarget` is the function the core asks "how high is this
 * unit, relative to me": spell hit and resistance tables, weapon and defence
 * skill (`GetMaxSkillValueForLevel`, `GetUnitMeleeSkill`), the glancing and
 * crushing tables, stealth detection, aggro radius, kill experience and
 * reputation all read it. It is already virtual and already overridden for
 * world bosses. With a level resolver installed, `Creature::getLevelForTarget`
 * answers with the viewer's level for that creature and every one of those
 * follows from one definition. The armor resolver covers the one input a
 * blow is mitigated against that no per-target level reaches:
 * `Unit::CalcArmorReducedDamage`.
 *
 * Zero from either resolver means "no view": an unset owner, a character the
 * module does not scale, or a creature that already stands at their level all
 * keep the stock behaviour. With no module installed the header is inert.
 */

#ifndef AC_CREATURE_VIEW_H
#define AC_CREATURE_VIEW_H

#include <atomic>
#include <cstdint>

class Creature;
class Player;

namespace CreatureView
{
    using LevelResolver = std::uint8_t (*)(Player const* viewer, Creature const* creature);
    using ArmorResolver = std::uint32_t (*)(Player const* viewer, Creature const* creature);

    inline std::atomic<LevelResolver> LevelOwner{nullptr};
    inline std::atomic<ArmorResolver> ArmorOwner{nullptr};

    /// The level the viewer's version of the creature stands at, or 0 when the
    /// viewer sees the authored creature.
    inline std::uint8_t LevelFor(Player const* viewer, Creature const* creature)
    {
        LevelResolver const owner = LevelOwner.load(std::memory_order_relaxed);
        return owner ? owner(viewer, creature) : 0;
    }

    /// The armor the viewer's version of the creature wears, or 0 when the
    /// creature is already their version of it.
    inline std::uint32_t ArmorFor(Player const* viewer, Creature const* creature)
    {
        ArmorResolver const owner = ArmorOwner.load(std::memory_order_relaxed);
        return owner ? owner(viewer, creature) : 0;
    }
}

#endif
