/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Chat.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Player.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Database/DBCStores.h"
#include "SharedDefines.h"
#include "Util.h"
#include "RaidGuildMgr.h"

namespace
{
    // Named rather than numbered, because a roster is authored by hand and nobody remembers
    // that a tank is 3. The spellings match the ones `.partybot role` already accepts.
    bool ExtractRole(std::string const& text, CombatBotRoles& role)
    {
        if (text == "tank")
            role = ROLE_TANK;
        else if (text == "healer")
            role = ROLE_HEALER;
        else if (text == "meleedps")
            role = ROLE_MELEE_DPS;
        else if (text == "rangedps")
            role = ROLE_RANGE_DPS;
        else if (text == "auto")
            role = ROLE_INVALID;
        else
            return false;

        return true;
    }

    char const* GetRoleName(CombatBotRoles role)
    {
        switch (role)
        {
            case ROLE_TANK:      return "tank";
            case ROLE_HEALER:    return "healer";
            case ROLE_MELEE_DPS: return "meleedps";
            case ROLE_RANGE_DPS: return "rangedps";
            default:             return "auto";
        }
    }
}

// .raidguild add <name> <race> <class> [gender] [role] [subgroup]
// Authors a roster row. Creates no character; that is what provision is for, and keeping
// the two apart means a roster can be written out in full before anything touches the
// characters table.
bool ChatHandler::HandleRaidGuildAddCommand(char* args)
{
    char* nameStr = ExtractArg(&args);
    uint32 race = 0;
    uint32 classId = 0;

    if (!nameStr || !ExtractUInt32(&args, race) || !ExtractUInt32(&args, classId))
    {
        SendSysMessage("Syntax: .raidguild add <name> <race> <class> [gender] [role] [subgroup]");
        SetSentErrorMessage(true);
        return false;
    }

    RaidGuildMember member;
    member.name = nameStr;
    if (!normalizePlayerName(member.name))
    {
        PSendSysMessage("RaidGuild: '%s' is not a usable character name.", nameStr);
        SetSentErrorMessage(true);
        return false;
    }

    member.race = uint8(race);
    member.classId = uint8(classId);

    uint32 gender = GENDER_MALE;
    ExtractUInt32(&args, gender);
    member.gender = uint8(gender);

    if (char* roleStr = ExtractArg(&args))
    {
        if (!ExtractRole(roleStr, member.role))
        {
            PSendSysMessage("RaidGuild: '%s' is not a role. Use tank, healer, meleedps, rangedps or auto.", roleStr);
            SetSentErrorMessage(true);
            return false;
        }
    }

    uint32 subGroup = 0;
    ExtractUInt32(&args, subGroup);
    member.subGroup = uint8(subGroup);

    std::string error;
    if (!sRaidGuildMgr.AddMember(member, error))
    {
        PSendSysMessage("RaidGuild: cannot add '%s', %s.", member.name.c_str(), error.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("added name=%s race=%u class=%u gender=%u role=%s subgroup=%u",
        member.name.c_str(), uint32(member.race), uint32(member.classId), uint32(member.gender),
        GetRoleName(member.role), uint32(member.subGroup));
    return true;
}

// .raidguild remove <name>
// Takes a member off the roster and leaves its character alone.
bool ChatHandler::HandleRaidGuildRemoveCommand(char* args)
{
    char* nameStr = ExtractArg(&args);
    if (!nameStr)
    {
        SendSysMessage("Syntax: .raidguild remove <name>");
        SetSentErrorMessage(true);
        return false;
    }

    std::string name = nameStr;
    normalizePlayerName(name);

    std::string error;
    if (!sRaidGuildMgr.RemoveMember(name, error))
    {
        PSendSysMessage("RaidGuild: cannot remove '%s', %s.", name.c_str(), error.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("removed name=%s", name.c_str());
    return true;
}

// .raidguild list
// The whole roster and how much of it exists as characters.
bool ChatHandler::HandleRaidGuildListCommand(char* /*args*/)
{
    std::vector<RaidGuildMember> const& roster = sRaidGuildMgr.GetRoster();

    uint32 provisioned = 0;
    for (RaidGuildMember const& member : roster)
    {
        if (member.IsProvisioned())
            provisioned++;

        PSendSysMessage("member name=%s race=%u class=%u gender=%u role=%s subgroup=%u lootgroup=%u guid=%u account=%u",
            member.name.c_str(), uint32(member.race), uint32(member.classId), uint32(member.gender),
            GetRoleName(member.role), uint32(member.subGroup), uint32(member.lootGroup),
            member.guid, member.accountId);
    }

    PSendSysMessage("roster members=%u provisioned=%u", uint32(roster.size()), provisioned);
    return true;
}

// .raidguild provision [name]
// Creates the characters the roster describes, or just the one named. Provisioning a member
// that already has a character is a no-op, so this is the normal way to bring a roster up to
// date rather than something to run once.
bool ChatHandler::HandleRaidGuildProvisionCommand(char* args)
{
    if (char* nameStr = ExtractArg(&args))
    {
        std::string name = nameStr;
        normalizePlayerName(name);

        std::string error;
        if (!sRaidGuildMgr.ProvisionMember(name, error))
        {
            PSendSysMessage("RaidGuild: cannot provision '%s', %s.", name.c_str(), error.c_str());
            SetSentErrorMessage(true);
            return false;
        }

        RaidGuildMember const* pMember = sRaidGuildMgr.FindMember(name);
        PSendSysMessage("provisioned name=%s guid=%u account=%u", name.c_str(),
            pMember ? pMember->guid : 0, pMember ? pMember->accountId : 0);
        return true;
    }

    // Names are collected first because provisioning writes to the roster it is iterating.
    std::vector<std::string> names;
    for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
        if (!member.IsProvisioned())
            names.push_back(member.name);

    uint32 created = 0;
    uint32 failed = 0;
    for (std::string const& name : names)
    {
        std::string error;
        if (sRaidGuildMgr.ProvisionMember(name, error))
        {
            created++;
            continue;
        }

        // Reported per member and not fatal to the run. A roster is forty rows and one bad
        // race and class pair among them should not leave the other thirty-nine uncreated,
        // which is the failure mode that makes a batch command useless.
        failed++;
        PSendSysMessage("RaidGuild: cannot provision '%s', %s.", name.c_str(), error.c_str());
    }

    PSendSysMessage("provision created=%u failed=%u already=%u", created, failed,
        uint32(sRaidGuildMgr.GetRoster().size() - names.size()));
    return true;
}

// The leader is named rather than taken from the session, because the harness drives every
// one of these over SOAP where there is no session player to take it from. An in-game caller
// may still leave it off and mean itself.
Player* ChatHandler::RaidGuildResolveLeader(char** args)
{
    if (char* nameStr = ExtractArg(args))
    {
        std::string name = nameStr;
        normalizePlayerName(name);

        Player* pLeader = ObjectAccessor::FindPlayerByName(name.c_str());
        if (!pLeader || !pLeader->IsInWorld())
        {
            PSendSysMessage("RaidGuild: '%s' is not in the world.", name.c_str());
            return nullptr;
        }

        return pLeader;
    }

    if (Player* pSelf = GetSession() ? GetSession()->GetPlayer() : nullptr)
        return pSelf;

    SendSysMessage("RaidGuild: expected the name of a character to summon to.");
    return nullptr;
}

// .raidguild summon <leader> [name]
// Brings the roster, or one member of it, into the world around the named character. The
// group is made before anyone is summoned so that the arriving members find it rather than
// race each other to create it.
bool ChatHandler::HandleRaidGuildSummonCommand(char* args)
{
    Player* pLeader = RaidGuildResolveLeader(&args);
    if (!pLeader)
    {
        SetSentErrorMessage(true);
        return false;
    }

    std::vector<std::string> names;
    if (char* nameStr = ExtractArg(&args))
    {
        std::string name = nameStr;
        normalizePlayerName(name);
        names.push_back(name);
    }
    else
    {
        for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
            if (member.IsProvisioned() && !sRaidGuildMgr.FindSummonedMember(member))
                names.push_back(member.name);
    }

    // Sized for what the group is about to hold, not for what is being added now, so that
    // summoning the second half of a raid one member at a time does not leave a party that
    // has to be promoted partway through.
    uint32 expected = pLeader->GetGroup() ? pLeader->GetGroup()->GetMembersCount() : 1;
    expected += uint32(names.size());

    std::string error;
    if (!sRaidGuildMgr.PrepareGroup(pLeader, expected, error))
    {
        PSendSysMessage("RaidGuild: cannot summon to '%s', %s.", pLeader->GetName(), error.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    uint32 summoned = 0;
    uint32 failed = 0;
    for (std::string const& name : names)
    {
        if (sRaidGuildMgr.SummonMember(name, pLeader, error))
        {
            summoned++;
            continue;
        }

        failed++;
        PSendSysMessage("RaidGuild: cannot summon '%s', %s.", name.c_str(), error.c_str());
    }

    PSendSysMessage("summon leader=%s summoned=%u failed=%u", pLeader->GetName(), summoned, failed);
    return true;
}

// .raidguild dismiss [name]
// Logs members out, which is also what saves them. Reports how many were asked to go, not
// how many have gone; use .raidguild status to wait for that.
bool ChatHandler::HandleRaidGuildDismissCommand(char* args)
{
    std::vector<std::string> names;
    if (char* nameStr = ExtractArg(&args))
    {
        std::string name = nameStr;
        normalizePlayerName(name);
        names.push_back(name);
    }
    else
    {
        for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
            if (sRaidGuildMgr.FindSummonedMember(member))
                names.push_back(member.name);
    }

    uint32 dismissed = 0;
    uint32 failed = 0;
    for (std::string const& name : names)
    {
        std::string error;
        if (sRaidGuildMgr.DismissMember(name, error))
        {
            dismissed++;
            continue;
        }

        failed++;
        PSendSysMessage("RaidGuild: cannot dismiss '%s', %s.", name.c_str(), error.c_str());
    }

    PSendSysMessage("dismiss dismissed=%u failed=%u", dismissed, failed);
    return true;
}

// .raidguild status
// Who is actually in the world and where the group put them. Logging out takes a while and
// joining a group takes a couple of seconds, so anything waiting on either needs to be able
// to ask rather than assume.
bool ChatHandler::HandleRaidGuildStatusCommand(char* /*args*/)
{
    uint32 online = 0;
    uint32 grouped = 0;

    for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
    {
        Player* pPlayer = sRaidGuildMgr.FindSummonedMember(member);
        if (!pPlayer)
            continue;

        online++;

        Group* pGroup = pPlayer->GetGroup();
        if (pGroup)
            grouped++;

        // Reported one based to match how the roster asks for it, and as zero for a member
        // that is in a party, where subgroups do not exist.
        uint32 subGroup = 0;
        if (pGroup && pGroup->isRaidGroup())
        {
            uint8 const slot = pGroup->GetMemberGroup(pPlayer->GetObjectGuid());
            if (slot < MAX_RAID_SUBGROUPS)
                subGroup = uint32(slot) + 1;
        }

        // The guild is reported from the character rather than from the guild tables,
        // because the question it answers is whether an offline bulk add actually reaches
        // a member when it next logs in, which is a different claim from the row existing.
        PSendSysMessage("summoned name=%s guid=%u inworld=%u level=%u map=%u instance=%u group=%u raid=%u subgroup=%u wanted=%u guild=%u binds=%u",
            member.name.c_str(), member.guid, pPlayer->IsInWorld() ? 1 : 0,
            pPlayer->GetLevel(), pPlayer->GetMapId(), pPlayer->GetInstanceId(),
            pGroup ? 1 : 0, (pGroup && pGroup->isRaidGroup()) ? 1 : 0,
            subGroup, uint32(member.subGroup), pPlayer->GetGuildId(),
            sRaidGuildMgr.CountMemberBinds(member));
    }

    PSendSysMessage("status online=%u grouped=%u", online, grouped);
    return true;
}

// .raidguild guild <name> [founder]
// Finds or founds the guild and puts the whole provisioned roster in it. Idempotent, so it
// is the way to bring a guild up to date after adding members rather than something to run
// once. A guild name with spaces has to be quoted.
bool ChatHandler::HandleRaidGuildGuildCommand(char* args)
{
    char* guildStr = ExtractQuotedOrLiteralArg(&args);
    if (!guildStr)
    {
        SendSysMessage("Syntax: .raidguild guild <name> [founder]");
        SetSentErrorMessage(true);
        return false;
    }

    std::string guildName = guildStr;

    // Only needed when the guild does not exist yet, so a missing or offline founder is
    // not refused here: adding to a guild that is already there needs nobody in the world.
    Player* pFounder = nullptr;
    if (char* founderStr = ExtractArg(&args))
    {
        std::string founderName = founderStr;
        normalizePlayerName(founderName);
        pFounder = ObjectAccessor::FindPlayerByName(founderName.c_str());
    }
    else
    {
        for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
        {
            Player* pSummoned = sRaidGuildMgr.FindSummonedMember(member);
            if (pSummoned && pSummoned->IsInWorld() && !pSummoned->GetGuildId())
            {
                pFounder = pSummoned;
                break;
            }
        }
    }

    uint32 added = 0;
    uint32 failed = 0;
    std::string error;
    if (!sRaidGuildMgr.FormGuild(guildName, pFounder, added, failed, error))
    {
        PSendSysMessage("RaidGuild: cannot form guild '%s', %s.", guildName.c_str(), error.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    Guild* pGuild = sGuildMgr.GetGuildByName(guildName);
    PSendSysMessage("guild name=%s id=%u added=%u failed=%u members=%u", guildName.c_str(),
        pGuild ? pGuild->GetId() : 0, added, failed,
        pGuild ? uint32(pGuild->GetMemberSize()) : 0);
    return true;
}

// .raidguild attune <leader> [name]
// Gives the summoned roster every quest and item an instance doorway asks for that the named
// character already has. Entry conditions are checked against each entering player, so the
// human earns access once and the guild inherits it rather than running the chain forty more
// times. Members have to be in the world, since what is being copied is character state.
bool ChatHandler::HandleRaidGuildAttuneCommand(char* args)
{
    Player* pLeader = RaidGuildResolveLeader(&args);
    if (!pLeader)
    {
        SetSentErrorMessage(true);
        return false;
    }

    std::string only;
    if (char* nameStr = ExtractArg(&args))
    {
        only = nameStr;
        normalizePlayerName(only);
    }

    uint32 granted = 0;
    uint32 touched = 0;
    uint32 offline = 0;
    for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
    {
        if (!only.empty() && member.name != only)
            continue;

        Player* pPlayer = sRaidGuildMgr.FindSummonedMember(member);
        if (!pPlayer || !pPlayer->IsInWorld())
        {
            if (member.IsProvisioned())
                offline++;
            continue;
        }

        touched++;
        granted += sRaidGuildMgr.MirrorAttunements(pLeader, pPlayer);
    }

    PSendSysMessage("attune leader=%s members=%u granted=%u offline=%u",
        pLeader->GetName(), touched, granted, offline);
    return true;
}

// .raidguild resetbinds [name]
// Drops the roster's personal instance binds. Weekly resets and experimentation leave stale
// ones behind, and a stale bind is not a cosmetic problem: a member carrying one for the map
// the raid is entering meets MANGOS_ASSERT at the door rather than an error message.
bool ChatHandler::HandleRaidGuildResetBindsCommand(char* args)
{
    std::string only;
    if (char* nameStr = ExtractArg(&args))
    {
        only = nameStr;
        normalizePlayerName(only);

        if (!sRaidGuildMgr.FindMember(only))
        {
            PSendSysMessage("RaidGuild: '%s' is not on the roster.", only.c_str());
            SetSentErrorMessage(true);
            return false;
        }
    }

    uint32 cleared = 0;
    uint32 touched = 0;
    for (RaidGuildMember const& member : sRaidGuildMgr.GetRoster())
    {
        if (!only.empty() && member.name != only)
            continue;

        Player* pPlayer = sRaidGuildMgr.FindSummonedMember(member);
        uint32 const dropped = sRaidGuildMgr.ClearMemberBinds(
            member, pPlayer ? pPlayer->GetGroup() : nullptr);

        if (dropped)
            touched++;

        cleared += dropped;
    }

    PSendSysMessage("resetbinds members=%u cleared=%u", touched, cleared);
    return true;
}

// .raidguild reload
// Rereads the roster table, for when it has been edited by hand underneath the server.
bool ChatHandler::HandleRaidGuildReloadCommand(char* /*args*/)
{
    sRaidGuildMgr.Load();
    PSendSysMessage("reloaded members=%u", uint32(sRaidGuildMgr.GetRoster().size()));
    return true;
}
