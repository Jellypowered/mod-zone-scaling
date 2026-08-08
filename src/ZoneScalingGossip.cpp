/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * A gossip NPC for the Chrono Mode toggle: the same decision as `.chrono`,
 * reached by talking to somebody.
 *
 * WHY THIS EXISTS WHEN THE COMMAND ALREADY DOES
 *
 * `.chrono` is complete and this adds no capability. It exists because the
 * population that most needs the opt-in is the one least likely to know a slash
 * command exists: a player who logs in, finds a level 7 boar hitting like a
 * level 60 one, and has no idea that was a choice somebody made. An innkeeper
 * with a dialogue option is discoverable in a way that a line in a README is
 * not.
 *
 * It is deliberately the *same* decision and not a parallel one. Every branch
 * below delegates to `ZoneScalingMgr` -- `IsChronoActive`, `CanToggleChrono`,
 * `SetChrono` -- so the NPC cannot come to a different conclusion than the
 * command, and a realm that changes the toggle rules changes both at once. The
 * failure this avoids is the one Z-08's header warns about for the scaling test
 * itself: two implementations of one question that disagree in the case nobody
 * tested.
 *
 * HOW A REALM GETS ONE, AND WHY NOTHING HAPPENS BY DEFAULT
 *
 * The script registers under the name `npc_zone_scaling_chrono` and then waits.
 * It affects exactly those creatures whose `creature_template.ScriptName` is
 * that string, and this module's own SQL creates none -- the optional file
 * `data/sql/db-world/optional/mod_zone_scaling_chrono_npc.sql` creates an
 * example NPC at entry 190000, and an administrator who never applies it has an
 * unchanged world. That is the same shape as every other risky thing in this
 * module: present, documented, and off.
 *
 * An administrator who would rather not spawn a new creature can instead set
 * the ScriptName on an existing innkeeper. That works, and is arguably the
 * better placement given the toggle is restricted to rested areas anyway --
 * but it overwrites whatever ScriptName that creature had, so the module does
 * not do it for them.
 *
 * WHERE THE GUARD LIVES
 *
 * Not here. `CanToggleChrono` already answers "may this player toggle right
 * now", with the out-of-combat and rested-area rules and the reason string, and
 * that reason is shown to the player rather than being flattened into a refusal.
 * A player who walks to the NPC and is told "not while in combat" has learned
 * something; one who is told "no" has not.
 */

#include "ZoneScaling.h"

#include "Chat.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "GossipDef.h"
#include "Log.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"

#include <string>

namespace
{
    // Distinct from GOSSIP_ACTION_INFO_DEF's neighbourhood so a stray action id
    // from another script can never be mistaken for one of these.
    constexpr uint32 ActionToggleOn  = 2001;
    constexpr uint32 ActionToggleOff = 2002;
    constexpr uint32 ActionExplain   = 2003;

    // The npc_text row shipped alongside the optional NPC. A realm that attaches
    // the script to an existing creature keeps its own text -- this is only the
    // fallback for the example NPC.
    constexpr uint32 ChronoNpcTextId = 190000;

    void SendChronoMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        bool const scaled = sZoneScalingMgr->IsScalingActiveFor(player);

        // The realm-state branches come first, because on a realm where Chrono
        // is not a choice every option below would be a lie.
        if (!sZoneScalingMgr->IsChronoEnabled())
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                sZoneScalingMgr->IsActive()
                    ? "The timeways are fixed here. Everyone walks the same road."
                    : "The timeways are quiet here. Nothing is being scaled.",
                GOSSIP_SENDER_MAIN, ActionExplain);
            SendGossipMenuFor(player, ChronoNpcTextId, creature);
            return;
        }

        if (!sZoneScalingMgr->IsChronoTablePresent())
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Something is wrong with my ledger. I cannot record your choice.",
                GOSSIP_SENDER_MAIN, ActionExplain);
            SendGossipMenuFor(player, ChronoNpcTextId, creature);
            return;
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            scaled ? "The world currently rises to meet me."
                   : "The world currently stands as it always has.",
            GOSSIP_SENDER_MAIN, ActionExplain);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
            scaled ? "Let the world stand still for me again."
                   : "Let the world rise to meet me.",
            GOSSIP_SENDER_MAIN, scaled ? ActionToggleOff : ActionToggleOn);

        SendGossipMenuFor(player, ChronoNpcTextId, creature);
    }

    // Shared by both toggle actions. `wanted` is the state the player asked for,
    // which is always the opposite of the current one -- the menu only ever
    // offers the flip.
    void ApplyToggle(Player* player, Creature* creature, bool wanted)
    {
        std::string why;
        if (!ZoneScalingMgr::CanToggleChrono(player, why))
        {
            // `why` is the same sentence `.chrono` would have printed. Showing
            // it beats a generic refusal: the rules are learnable.
            ChatHandler(player->GetSession()).SendSysMessage(why);
            CloseGossipMenuFor(player);
            return;
        }

        if (!sZoneScalingMgr->SetChrono(player, wanted))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "Your choice could not be saved. Please report this.");
            CloseGossipMenuFor(player);
            return;
        }

        ChatHandler(player->GetSession()).SendSysMessage(
            wanted ? "The world will rise to meet you."
                   : "The world will stand as it always has.");

        LOG_DEBUG("module.zonescaling", "[zone-scaling] chrono toggled via gossip by {}: {}",
                  player->GetName(), wanted ? "on" : "off");

        // Re-open rather than close, so the player sees the new state
        // reflected in the first line. The toggle is rare and deliberate;
        // confirming it visibly is worth one extra packet.
        SendChronoMenu(player, creature);
    }
}

class ZoneScalingChronoGossip : public CreatureScript
{
public:
    ZoneScalingChronoGossip() : CreatureScript("npc_zone_scaling_chrono") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player || !creature)
            return false;

        SendChronoMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!player || !creature)
            return false;

        switch (action)
        {
            case ActionToggleOn:
                ApplyToggle(player, creature, true);
                break;
            case ActionToggleOff:
                ApplyToggle(player, creature, false);
                break;
            case ActionExplain:
            default:
                // The status line is not a button; selecting it just redraws.
                SendChronoMenu(player, creature);
                break;
        }

        return true;
    }
};

void AddZoneScalingGossipScripts()
{
    new ZoneScalingChronoGossip();
}
