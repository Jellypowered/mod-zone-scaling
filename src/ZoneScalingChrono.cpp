/*
 * Copyright (C) 2026+ mod-zone-scaling contributors, released under GNU AGPL v3
 * license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Chrono Mode: scaling as a per-player choice rather than a realm state.
 *
 * WHY THIS IS ALMOST FREE
 *
 * Every earlier milestone was built as a per-player question. The damage
 * ratios are per-player, the melee hit table is per-player, and Z-05's level
 * display is per-observer. So "this player is not scaled" already has a
 * representation in the architecture -- ratio 1.0 and the canonical level --
 * and every one of those falls out of a single function returning nothing.
 *
 * ZoneScalingMgr::GetEffectiveLevelFor is that function. It is the sole point
 * at which a (creature, player) pair becomes a scaled pair, shared by the
 * damage hooks, the hit-table hook and the display patch. One test there opts a
 * player out of all four systems at once, and there is deliberately no second
 * test anywhere else: a Chrono check that appeared in three places would
 * eventually disagree with itself, and the case where it disagreed would be a
 * player whose nameplates and damage told different stories.
 *
 * WHAT THE FLAG CANNOT REACH, AND WHY
 *
 * Two things this module does are not per-player and therefore cannot follow
 * the flag. Both are stated in the README rather than hidden:
 *
 *  - `AggroRange = normalize` (Z-06) writes a detection distance onto the
 *    creature. There is one creature and one detection distance; an opted-out
 *    player standing next to an opted-in one meets the same radius. This is the
 *    same wall Z-06 hit -- no hook, no per-observer path -- not a new one.
 *  - `CanonicalLevel = midpoint` rewrites the creature's real level at spawn.
 *    That is world state, not presentation, and by the time any player is
 *    considered it has already happened.
 *
 * Neither changes a fight's difficulty for an opted-out player relative to
 * stock in the way damage does, but both are visible, and "opted out means
 * nothing about you is scaled" would be a slightly false promise.
 *
 * WHERE YOU MAY TOGGLE, AND WHY IT IS GUARDED AT ALL
 *
 * Out of combat and in a rested area, mirroring retail war mode. The guard is
 * not ceremony: without it an opted-in player losing a scaled fight flips the
 * creature trivial mid-pull, and the inverse -- tag something weak, opt in,
 * kill it for the Z-09 bonus -- is the same exploit read backwards. Restricting
 * the toggle to inns and capitals closes both with one rule, and it is a rule
 * players already understand from retail.
 */

#include "ZoneScaling.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Common.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <string>

using namespace Acore::ChatCommands;

ZoneScalingPlayerState* ZoneScalingMgr::GetPlayerState(Player const* player)
{
    // const_cast for the same reason the creature path does it: DataMap::Get is
    // non-const, but reading a player's own flag does not modify the player in
    // any sense that matters to a caller holding a const pointer.
    return player ? const_cast<Player*>(player)->CustomData.Get<ZoneScalingPlayerState>(
                        ZoneScalingPlayerState::Key)
                  : nullptr;
}

bool ZoneScalingMgr::IsScalingActiveFor(Player const* player) const
{
    // The fast exit, and the common one: with Chrono off there is no lookup at
    // all, so a realm that never enables this milestone pays nothing for it in
    // the damage path.
    if (!IsChronoActive())
        return true;

    if (!player)
        return true;

    ZoneScalingPlayerState const* state = GetPlayerState(player);

    // No state, or state that has not resolved its row yet. Falling back to the
    // configured default rather than to false is the important half: `false`
    // here would mean that any ordering problem in the login path -- or any
    // Player object this module never saw log in -- reads as a deliberate
    // opt-out, and a realm-wide opt-out is indistinguishable from the module
    // being broken.
    if (!state || !state->Loaded)
        return _chronoDefault;

    // A character who has never chosen follows the realm's default *now*, not
    // the default as it stood when they logged in. Reading the cached copy
    // instead would mean a `.reload config` moved new logins and left everyone
    // already online on the old value -- one setting, two populations, and a
    // difference that presents as scaling working for some players and not
    // others. A stored choice still wins; that is what Explicit distinguishes.
    if (!state->Explicit)
        return _chronoDefault;

    return state->Enabled;
}

void ZoneScalingMgr::OnPlayerLogin(Player* player) const
{
    if (!player)
        return;

    ZoneScalingPlayerState* state =
        player->CustomData.GetDefault<ZoneScalingPlayerState>(ZoneScalingPlayerState::Key);

    state->Loaded   = true;
    state->Explicit = false;
    state->Enabled  = _chronoDefault;

    // Attach state even when Chrono is inactive, so that `.chrono` can answer
    // coherently and so that switching Chrono on with `.reload config` does not
    // leave already-logged-in players in the unresolved case above. It costs
    // one small allocation per login.
    if (!IsChronoActive())
        return;

    QueryResult result = CharacterDatabase.Query(
        "SELECT enabled FROM mod_zone_scaling_character WHERE guid = {}",
        player->GetGUID().GetCounter());

    if (result)
    {
        state->Enabled  = (*result)[0].Get<uint8>() != 0;
        state->Explicit = true;
    }

    if (_debug)
        LOG_DEBUG("module.zonescaling", "chrono: {} logged in {} ({})",
            player->GetName(), state->Enabled ? "scaled" : "unscaled",
            state->Explicit ? "own row" : "realm default");
}

bool ZoneScalingMgr::SetChrono(Player* player, bool enabled) const
{
    if (!player || !_chronoTablePresent)
        return false;

    ZoneScalingPlayerState* state =
        player->CustomData.GetDefault<ZoneScalingPlayerState>(ZoneScalingPlayerState::Key);

    state->Loaded   = true;
    state->Explicit = true;
    state->Enabled  = enabled;

    // Written immediately rather than at logout. A toggle is a deliberate,
    // infrequent act, and persisting it on the spot means a crash between the
    // toggle and the logout cannot silently discard a player's choice --
    // which they would only discover by being killed by something they
    // expected to be scaled.
    CharacterDatabase.Execute(
        "REPLACE INTO mod_zone_scaling_character (guid, enabled, changedAt) VALUES ({}, {}, {})",
        player->GetGUID().GetCounter(), enabled ? 1 : 0,
        uint32(GameTime::GetGameTime().count()));

    CountChronoToggled();

    // The nameplates of everything already on screen were built against the old
    // answer and will not be rebuilt on their own. Same mechanism, and the same
    // reasoning, as the level-up refresh in Z-05.
    ZoneScalingRefreshNearbyLevels(player, "chrono toggle");

    return true;
}

bool ZoneScalingMgr::CanToggleChrono(Player const* player, std::string& why)
{
    if (!player)
    {
        why = "no player";
        return false;
    }

    if (player->IsInCombat())
    {
        why = "you are in combat";
        return false;
    }

    // Faction rest zones count alongside inns and capitals. They are rested
    // areas by the core's own definition, they are safe, and excluding them
    // would leave some players with no reachable place to toggle at all.
    if (!player->HasRestFlag(REST_FLAG_IN_TAVERN)
        && !player->HasRestFlag(REST_FLAG_IN_CITY)
        && !player->HasRestFlag(REST_FLAG_IN_FACTION_AREA))
    {
        why = "you must be in an inn or a capital city";
        return false;
    }

    return true;
}

class ZoneScalingChronoPlayerScript : public PlayerScript
{
public:
    ZoneScalingChronoPlayerScript() : PlayerScript("ZoneScalingChronoPlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_DELETE_FROM_DB
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        sZoneScalingMgr->OnPlayerLogin(player);
    }

    // Joined to the core's own deletion transaction rather than run as a
    // separate Execute, so the row cannot survive a delete that rolls back --
    // and, more usefully, cannot be orphaned by a delete that succeeds while
    // this module's write fails.
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        if (!sZoneScalingMgr->IsChronoTablePresent())
            return;

        trans->Append("DELETE FROM mod_zone_scaling_character WHERE guid = {}", guid);
    }
};

class chrono_commandscript : public CommandScript
{
public:
    chrono_commandscript() : CommandScript("chrono_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        // SEC_PLAYER and Console::No: this is the module's only player-facing
        // command, and it acts on the caller, so there is nothing for a console
        // with no player attached to act on.
        static ChatCommandTable commandTable =
        {
            { "chrono", HandleChronoCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

private:
    static void ReportState(ChatHandler* handler, Player* player)
    {
        ZoneScalingPlayerState const* state = ZoneScalingMgr::GetPlayerState(player);
        bool const on = sZoneScalingMgr->IsScalingActiveFor(player);

        handler->PSendSysMessage("Chrono Mode is {} for you{}.",
            on ? "|cff20ff20ON|r" : "|cffff2020OFF|r",
            (state && state->Explicit) ? "" : " (the realm default; you have not chosen)");

        handler->PSendSysMessage(on
            ? "Creatures in scaled zones present at your level, and fight you at it."
            : "You fight every creature at its own level, exactly as on an unscaled realm.");

        std::string why;
        if (ZoneScalingMgr::CanToggleChrono(player, why))
            handler->SendSysMessage("You can change this here: |cffffffff.chrono on|r or |cffffffff.chrono off|r.");
        else
            handler->PSendSysMessage("You cannot change it right now -- {}.", why);
    }

    static bool HandleChronoCommand(ChatHandler* handler, Optional<std::string> arg)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!sZoneScalingMgr->IsChronoEnabled())
        {
            // Two distinct realm states, and a player deserves to be told which
            // one they are in: a realm with no scaling at all, and a realm
            // where scaling is compulsory.
            handler->PSendSysMessage(sZoneScalingMgr->IsActive()
                ? "Chrono Mode is not a choice on this realm: level scaling applies to everyone."
                : "Level scaling is not enabled on this realm, so there is nothing to opt in to.");
            return true;
        }

        if (!sZoneScalingMgr->IsChronoTablePresent())
        {
            handler->SendSysMessage("Chrono Mode is misconfigured on this realm and your choice "
                "cannot be saved. Please report this; scaling applies to everyone meanwhile.");
            return true;
        }

        if (!arg)
        {
            ReportState(handler, player);
            return true;
        }

        std::string value = *arg;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });

        bool wanted;
        if (value == "on" || value == "1" || value == "enable")
            wanted = true;
        else if (value == "off" || value == "0" || value == "disable")
            wanted = false;
        else
        {
            handler->SendSysMessage("Usage: |cffffffff.chrono|r, |cffffffff.chrono on|r or |cffffffff.chrono off|r.");
            return true;
        }

        if (sZoneScalingMgr->IsScalingActiveFor(player) == wanted)
        {
            handler->PSendSysMessage("Chrono Mode is already {} for you.", wanted ? "on" : "off");
            return true;
        }

        // A GM may toggle anywhere. This is not a courtesy: the location guard
        // is the part of this milestone most likely to be wrong in the field --
        // a zone whose rest flag never gets set leaves a player unable to change
        // their own mind -- and without a bypass there is no way to fix that
        // for them, or to test the guarded paths deliberately.
        bool const bypass  = handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER;

        std::string why;
        bool const allowed = ZoneScalingMgr::CanToggleChrono(player, why);

        if (!allowed && !bypass)
        {
            handler->PSendSysMessage("You cannot change Chrono Mode right now -- {}.", why);
            handler->SendSysMessage("Return to an inn or a capital city, out of combat, and try again.");
            return true;
        }

        if (!sZoneScalingMgr->SetChrono(player, wanted))
        {
            handler->SendSysMessage("Your choice could not be saved. Nothing was changed.");
            return true;
        }

        handler->PSendSysMessage("Chrono Mode is now {} for you.{}",
            wanted ? "|cff20ff20ON|r" : "|cffff2020OFF|r",
            allowed ? "" : " (location and combat checks bypassed by your access level)");

        handler->SendSysMessage(wanted
            ? "Creatures in scaled zones will now present at your level."
            : "Creatures will now present at their own level to you.");

        LOG_INFO("module.zonescaling", "chrono: {} (guid {}) set Chrono Mode {}{}",
            player->GetName(), player->GetGUID().GetCounter(),
            wanted ? "on" : "off", allowed ? "" : " [GM bypass]");

        return true;
    }
};

void AddZoneScalingChronoScripts()
{
    new ZoneScalingChronoPlayerScript();
    new chrono_commandscript();
}
