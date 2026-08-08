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
#include "Player.h"
#include "World.h"
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

// .raidguild reload
// Rereads the roster table, for when it has been edited by hand underneath the server.
bool ChatHandler::HandleRaidGuildReloadCommand(char* /*args*/)
{
    sRaidGuildMgr.Load();
    PSendSysMessage("reloaded members=%u", uint32(sRaidGuildMgr.GetRoster().size()));
    return true;
}
