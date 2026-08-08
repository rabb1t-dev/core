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

#include "RaidGuildMgr.h"

#include "AccountMgr.h"
#include "Database/DatabaseEnv.h"
#include "Database/DBCStores.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Policies/SingletonImp.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"

INSTANTIATE_SINGLETON_1(RaidGuildMgr);

// Roster members are given an account of their own, but no `realmd` row to go with it.
// Nothing in the bot login path consults one: AccountMgr::GetSecurity falls back to
// SEC_PLAYER for an unknown id, and LoadFromDB skips the account ownership check for bots.
// Confirmed by logging in a character whose account number exists in no table anywhere.
//
// The account still has to be distinct per member, because PlayerBotMgr::AddBot refuses a
// bot whose account already has a session, and a shared account would therefore let the
// first member of the roster block all the others.
//
// The base sits far clear of the range generated bots draw from. That range starts at the
// highest real account plus ten thousand and rises by one per bot spawned, so reaching this
// would take five million spawns in a single uptime.
static uint32 const RAIDGUILD_ACCOUNT_BASE = 5000000;

void RaidGuildMgr::Load()
{
    m_roster.clear();

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT `name`, `race`, `class`, `gender`, `role`, `spec`, `loot_group`, `subgroup`, `guid`, `account`"
        " FROM `raidguild_member` ORDER BY `subgroup`, `name`"));

    if (!result)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded 0 raid guild members");
        return;
    }

    uint32 provisioned = 0;
    do
    {
        Field* fields = result->Fetch();

        RaidGuildMember member;
        member.name = fields[0].GetCppString();
        member.race = fields[1].GetUInt8();
        member.classId = fields[2].GetUInt8();
        member.gender = fields[3].GetUInt8();
        member.role = CombatBotRoles(fields[4].GetUInt8());
        member.spec = fields[5].GetCppString();
        member.lootGroup = fields[6].GetUInt8();
        member.subGroup = fields[7].GetUInt8();
        member.guid = fields[8].GetUInt32();
        member.accountId = fields[9].GetUInt32();

        // A roster row claiming a character that is no longer there would otherwise be
        // spawned as an empty guid and fail somewhere much further along. Reported rather
        // than corrected, since deleting the character may well have been the mistake.
        if (member.IsProvisioned() && !sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER, member.guid)))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[RaidGuild] '%s' is provisioned as character %u, which does not exist. "
                "Provision it again to recreate it.", member.name.c_str(), member.guid);
            member.guid = 0;
            member.accountId = 0;
        }

        if (member.IsProvisioned())
            provisioned++;

        m_roster.push_back(member);
    }
    while (result->NextRow());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u raid guild members, %u provisioned",
        uint32(m_roster.size()), provisioned);
}

RaidGuildMember const* RaidGuildMgr::FindMember(std::string const& name) const
{
    return const_cast<RaidGuildMgr*>(this)->FindMemberInternal(name);
}

RaidGuildMember* RaidGuildMgr::FindMemberInternal(std::string const& name)
{
    for (RaidGuildMember& member : m_roster)
        if (member.name == name)
            return &member;

    return nullptr;
}

uint32 RaidGuildMgr::AllocateAccountId() const
{
    // Counted from the roster rather than from a stored high-water mark, so removing the
    // last member and adding another does not hand out an account still referenced by a
    // character row that a stale roster no longer mentions.
    uint32 highest = RAIDGUILD_ACCOUNT_BASE;
    for (RaidGuildMember const& member : m_roster)
        if (member.accountId > highest)
            highest = member.accountId;

    return highest + 1;
}

bool RaidGuildMgr::AddMember(RaidGuildMember const& member, std::string& error)
{
    if (FindMemberInternal(member.name))
    {
        error = "already on the roster";
        return false;
    }

    if (!sObjectMgr.GetPlayerInfo(member.race, member.classId))
    {
        error = "not a valid race and class combination";
        return false;
    }

    if (member.gender != GENDER_MALE && member.gender != GENDER_FEMALE)
    {
        error = "gender must be 0 for male or 1 for female";
        return false;
    }

    if (ObjectMgr::CheckPlayerName(member.name, true) != CHAR_NAME_SUCCESS)
    {
        error = "not a usable character name";
        return false;
    }

    std::string escapedName = member.name;
    std::string escapedSpec = member.spec;
    CharacterDatabase.escape_string(escapedName);
    CharacterDatabase.escape_string(escapedSpec);

    if (!CharacterDatabase.PExecute(
        "INSERT INTO `raidguild_member` (`name`, `race`, `class`, `gender`, `role`, `spec`, `loot_group`, `subgroup`)"
        " VALUES ('%s', %u, %u, %u, %u, '%s', %u, %u)",
        escapedName.c_str(), uint32(member.race), uint32(member.classId), uint32(member.gender),
        uint32(member.role), escapedSpec.c_str(), uint32(member.lootGroup), uint32(member.subGroup)))
    {
        error = "the roster row could not be written";
        return false;
    }

    m_roster.push_back(member);
    return true;
}

bool RaidGuildMgr::RemoveMember(std::string const& name, std::string& error)
{
    RaidGuildMember const* pMember = FindMemberInternal(name);
    if (!pMember)
    {
        error = "not on the roster";
        return false;
    }

    // The character itself is left alone. Taking a member off the roster is a statement
    // about who is in the guild, and deleting a geared character is not something to do as
    // a side effect of it.
    std::string escapedName = name;
    CharacterDatabase.escape_string(escapedName);
    if (!CharacterDatabase.PExecute("DELETE FROM `raidguild_member` WHERE `name` = '%s'", escapedName.c_str()))
    {
        error = "the roster row could not be deleted";
        return false;
    }

    for (auto itr = m_roster.begin(); itr != m_roster.end(); ++itr)
    {
        if (itr->name == name)
        {
            m_roster.erase(itr);
            break;
        }
    }

    return true;
}

void RaidGuildMgr::SaveProvisioning(RaidGuildMember const& member) const
{
    std::string escapedName = member.name;
    CharacterDatabase.escape_string(escapedName);
    CharacterDatabase.PExecute("UPDATE `raidguild_member` SET `guid` = %u, `account` = %u WHERE `name` = '%s'",
        member.guid, member.accountId, escapedName.c_str());
}

bool RaidGuildMgr::ProvisionMember(std::string const& name, std::string& error)
{
    RaidGuildMember* pMember = FindMemberInternal(name);
    if (!pMember)
    {
        error = "not on the roster";
        return false;
    }

    if (pMember->IsProvisioned())
        return true;

    // A character of that name may already exist without the roster knowing, which is the
    // normal state after the roster table is rebuilt or a member is re-added. Adopt it
    // rather than failing, since the alternative is a member that can never be provisioned
    // under the name it is supposed to have.
    //
    // Asked of the table rather than of the name cache, which is the obvious way to do it
    // and is wrong. The two can disagree, and a deletion that clears the cache entry while
    // leaving the row is enough to do it; asking the cache then reports the name free and
    // SaveNewPlayer, which is a REPLACE keyed on guid, cheerfully writes a second character
    // with the same name. Two characters answering to one name is a bad state to be in, and
    // a quiet one: from then on everything that addresses a player by name reaches whichever
    // the lookup happens to find.
    std::string escapedName = pMember->name;
    CharacterDatabase.escape_string(escapedName);
    std::unique_ptr<QueryResult> existing(CharacterDatabase.PQuery(
        "SELECT `guid`, `account` FROM `characters` WHERE `name` = '%s'", escapedName.c_str()));

    if (existing)
    {
        Field* fields = existing->Fetch();
        pMember->guid = fields[0].GetUInt32();
        pMember->accountId = fields[1].GetUInt32();
        SaveProvisioning(*pMember);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[RaidGuild] '%s' already existed as character %u; adopted it.",
            pMember->name.c_str(), pMember->guid);
        return true;
    }

    if (!sObjectMgr.GetPlayerInfo(pMember->race, pMember->classId))
    {
        error = "not a valid race and class combination";
        return false;
    }

    if (!Player::ValidateAppearance(pMember->race, pMember->gender, 0, 0, 0, 0, 0))
    {
        error = "the default appearance is not valid for that race and gender";
        return false;
    }

    uint32 const accountId = AllocateAccountId();

    // Supplies nothing but the account, security and locale that SaveNewPlayer records. It
    // is never registered with the world, and its destructor is a no-op without a player.
    WorldSession provisioningSession(accountId, nullptr, sAccountMgr.GetSecurity(accountId), 0, LOCALE_enUS);

    uint32 const guidLow = sObjectMgr.GeneratePlayerLowGuid();
    if (!Player::SaveNewPlayer(&provisioningSession, guidLow, pMember->name, pMember->race,
        pMember->classId, pMember->gender, 0, 0, 0, 0, 0))
    {
        error = "the character could not be created";
        return false;
    }

    pMember->guid = guidLow;
    pMember->accountId = accountId;
    SaveProvisioning(*pMember);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[RaidGuild] provisioned '%s' as character %u on account %u.",
        pMember->name.c_str(), guidLow, accountId);
    return true;
}
