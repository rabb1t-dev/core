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
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PartyBotAI.h"
#include "Player.h"
#include "PlayerBotMgr.h"
#include "Policies/SingletonImp.h"
#include "Util.h"
#include "Utilities/Random.h"
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

// How often summoned members are checked against the subgroup the roster asks for. Only
// worth doing while members are arriving, which is a few seconds after a summon, so this
// wants to be often enough not to be noticed and rare enough not to matter.
static uint32 const RAIDGUILD_RECONCILE_INTERVAL = 1000;

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

Player* RaidGuildMgr::FindSummonedMember(RaidGuildMember const& member) const
{
    if (!member.IsProvisioned())
        return nullptr;

    // Not FindPlayer, which only answers for a player already in the world. A member that
    // is still loading is very much summoned, and summoning it a second time would be
    // refused by AddBot as an account that is already online.
    return sObjectAccessor.FindPlayerNotInWorld(ObjectGuid(HIGHGUID_PLAYER, member.guid));
}

bool RaidGuildMgr::PrepareGroup(Player* pLeader, uint32 expectedSize, std::string& error)
{
    Group* pGroup = pLeader->GetGroup();
    if (!pGroup)
    {
        pGroup = new Group;
        if (!pGroup->Create(pLeader->GetObjectGuid(), pLeader->GetName()))
        {
            delete pGroup;
            error = "the group could not be created";
            return false;
        }

        sObjectMgr.AddGroup(pGroup);
    }

    if (!pGroup->IsLeader(pLeader->GetObjectGuid()))
    {
        error = "is not the leader of the group it is in";
        return false;
    }

    // Promoted before anyone joins rather than when the sixth member is turned away. Doing
    // it here also settles the question once, for the whole summon, instead of leaving each
    // arriving bot to notice the group is full and convert it, which is a race when two
    // arrive on the same tick.
    //
    // Only when the party will not hold them, since a raid group of three is not what a
    // player would form for a five man and it changes how the group reads to everything
    // downstream.
    if (expectedSize > MAX_GROUP_SIZE && !pGroup->isRaidGroup())
        pGroup->ConvertToRaid();

    // Stated rather than inherited. Group::Create hardcodes these two, and the loot design
    // this roster is being built for keys off both, so a change to that default would
    // silently change how the guild distributes loot.
    pGroup->SetLootMethod(GROUP_LOOT);
    pGroup->SetLooterGuid(ObjectGuid());
    pGroup->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
    pGroup->SendUpdate();
    return true;
}

bool RaidGuildMgr::SummonMember(std::string const& name, Player* pLeader, std::string& error)
{
    RaidGuildMember* pMember = FindMemberInternal(name);
    if (!pMember)
    {
        error = "not on the roster";
        return false;
    }

    if (!pMember->IsProvisioned())
    {
        error = "not provisioned yet";
        return false;
    }

    if (FindSummonedMember(*pMember))
    {
        error = "already in the world";
        return false;
    }

    // Before the session loads, so the bot comes up carrying nothing that can disagree
    // with the group. A personal bind beats a group bind in
    // GetBoundInstanceSaveForSelfOrGroup, and DungeonMap::BindPlayerOrGroupOnEnter answers
    // a disagreement with MANGOS_ASSERT rather than an error, so this is a crash the roster
    // would otherwise walk into at the door of a raid the leader is saved to.
    ClearMemberBinds(*pMember, pLeader->GetGroup());

    float x, y, z;
    pLeader->GetNearPoint(pLeader, x, y, z, 0, 5.0f, frand(0.0f, 6.0f));

    // The load constructor, which leaves race and class zero. That is not an omission: it
    // is what keeps the bot off the init branch that unequips every slot, resets talents
    // and re-rolls gear, and so it is the only constructor a member carrying earned gear
    // may ever be spawned through.
    PartyBotAI* pAI = new PartyBotAI(pLeader, pLeader->GetMapId(),
        pLeader->GetMap()->GetInstanceId(), x, y, z, pLeader->GetOrientation());

    // The roster's word beats the spell book. Left invalid, the bot works its role out from
    // which talents it happens to have, which is a reasonable guess and not a decision.
    if (pMember->role != ROLE_INVALID)
        pAI->m_role = pMember->role;

    if (!sPlayerBotMgr.AddBot(pMember->guid, false, pAI))
    {
        delete pAI;
        error = "the bot session could not be started";
        return false;
    }

    return true;
}

bool RaidGuildMgr::DismissMember(std::string const& name, std::string& error)
{
    RaidGuildMember const* pMember = FindMemberInternal(name);
    if (!pMember)
    {
        error = "not on the roster";
        return false;
    }

    if (!FindSummonedMember(*pMember))
    {
        error = "not in the world";
        return false;
    }

    // Logging out is what saves the character, so this is the only way a member may be
    // sent away. Killing the session instead loses everything since the last periodic save,
    // which is up to fifteen minutes of a raid night.
    if (!sPlayerBotMgr.DeleteBot(pMember->guid))
    {
        error = "the bot session could not be stopped";
        return false;
    }

    return true;
}

bool RaidGuildMgr::FormGuild(std::string const& guildName, Player* pMaster, uint32& added,
    uint32& failed, std::string& error)
{
    added = 0;
    failed = 0;

    Guild* pGuild = sGuildMgr.GetGuildByName(guildName);
    if (!pGuild)
    {
        // Founding needs a character in the world, because Guild::Create reads the locale
        // off its session to name the default ranks. Filling the tables by hand instead
        // would skip that and leave a guild with no ranks, so the requirement is kept and
        // stated rather than worked around.
        if (!pMaster)
        {
            error = "no guild of that name exists, and no character was given to found one";
            return false;
        }

        if (pMaster->GetGuildId())
        {
            error = "the founder is already in a guild";
            return false;
        }

        pGuild = new Guild;
        if (!pGuild->Create(pMaster, guildName))
        {
            delete pGuild;
            error = "the guild could not be created";
            return false;
        }

        sGuildMgr.AddGuild(pGuild);
    }

    for (RaidGuildMember const& member : m_roster)
    {
        if (!member.IsProvisioned())
            continue;

        ObjectGuid const guid(HIGHGUID_PLAYER, member.guid);
        if (pGuild->GetRank(guid) != -1)
            continue;

        // Guild::AddMember reads an offline character's name, level and class out of the
        // player cache and refuses it outright if there is no entry. A provisioned member
        // normally has one, but an adopted character need not: adoption reads the
        // `characters` table precisely because the cache can be missing a row that exists.
        // So the cache is filled from the table first rather than the add failing for a
        // character that is demonstrably there.
        if (!sObjectMgr.GetPlayerDataByGUID(member.guid))
            sObjectMgr.LoadPlayerCacheData(member.guid);

        // Member rather than the lowest rank. Initiate is what a guild gives someone it is
        // still deciding about, and every one of these was written down on purpose.
        if (pGuild->AddMember(guid, GR_MEMBER) == GuildAddStatus::OK)
        {
            added++;
            continue;
        }

        failed++;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[RaidGuild] '%s' could not be added to guild '%s'.",
            member.name.c_str(), guildName.c_str());
    }

    return true;
}

uint32 RaidGuildMgr::CountMemberBinds(RaidGuildMember const& member) const
{
    if (!member.IsProvisioned())
        return 0;

    if (Player* pPlayer = FindSummonedMember(member))
        return uint32(pPlayer->GetBoundInstances().size());

    std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
        "SELECT COUNT(*) FROM `character_instance` WHERE `guid` = '%u'", member.guid));
    return result ? result->Fetch()[0].GetUInt32() : 0;
}

uint32 RaidGuildMgr::ClearMemberBinds(RaidGuildMember const& member, Group* pGroup)
{
    if (!member.IsProvisioned())
        return 0;

    uint32 cleared = 0;
    Player* pPlayer = FindSummonedMember(member);

    if (!pPlayer)
    {
        // Deleting the rows of an offline character is how the rest of the server does
        // this too, in Group::ChangeLeader and in ConvertInstancesToGroup. Doing it before
        // the session loads is also the only race-free moment: after login the bot is
        // teleported to the leader within a couple of seconds, and if that lands it at an
        // instance door with a stale bind the answer is an assert rather than an error.
        std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
            "SELECT COUNT(*) FROM `character_instance` WHERE `guid` = '%u'", member.guid));
        if (result)
            cleared = result->Fetch()[0].GetUInt32();

        if (cleared)
            CharacterDatabase.PExecute("DELETE FROM `character_instance` WHERE `guid` = '%u'", member.guid);

        return cleared;
    }

    Player::BoundInstancesMap& binds = pPlayer->GetBoundInstances();
    for (Player::BoundInstancesMap::iterator itr = binds.begin(); itr != binds.end();)
    {
        uint32 const mapId = itr->first;

        // Never the map it is standing on. Unbinding that is how a character ends up
        // inside an instance it has no claim to, which is a worse state than the stale
        // bind this is here to remove.
        if (pPlayer->IsInWorld() && pPlayer->GetMapId() == mapId)
        {
            ++itr;
            continue;
        }

        if (pGroup)
        {
            if (InstanceGroupBind* pGroupBind = pGroup->GetBoundInstance(mapId))
            {
                if (pGroupBind->state == itr->second.state)
                {
                    ++itr;
                    continue;
                }
            }
        }

        // Increments the iterator itself, since it erases through it.
        pPlayer->UnbindInstance(itr, false);
        cleared++;
    }

    return cleared;
}

void RaidGuildMgr::Update(uint32 diff)
{
    if (m_reconcileTimer > diff)
    {
        m_reconcileTimer -= diff;
        return;
    }

    m_reconcileTimer = RAIDGUILD_RECONCILE_INTERVAL;

    for (RaidGuildMember const& member : m_roster)
    {
        if (!member.subGroup)
            continue;

        Player* pPlayer = FindSummonedMember(member);
        if (!pPlayer || !pPlayer->IsInWorld())
            continue;

        Group* pGroup = pPlayer->GetGroup();
        if (!pGroup || !pGroup->isRaidGroup())
            continue;

        uint8 const wanted = member.subGroup - 1;
        if (pGroup->GetMemberGroup(pPlayer->GetObjectGuid()) == wanted)
            continue;

        // Silently declines a subgroup that is already full, which is the right answer to a
        // roster that asks for nine people in one of them. The member keeps the slot it was
        // given rather than the run stopping over a seating plan.
        pGroup->ChangeMembersGroup(pPlayer, wanted);
    }
}
