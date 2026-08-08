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

#ifndef MANGOS_RAIDGUILDMGR_H
#define MANGOS_RAIDGUILDMGR_H

#include "Common.h"
#include "SharedDefines.h"
#include "Policies/Singleton.h"

#include <string>
#include <vector>

class Group;
class Player;

// A stable of real, saved characters that gear up over time, as opposed to the throwaway
// characters `.partybot add` generates. The distinction is not a matter of degree: a
// generated bot goes through Player::Create, which sets m_saveDisabled unconditionally and
// nothing ever clears it, so it cannot persist anything at all. Roster members are written
// to the `characters` table up front and spawned only through the database load path.
struct RaidGuildMember
{
    std::string name;
    uint8 race = 0;
    uint8 classId = 0;
    uint8 gender = GENDER_MALE;
    CombatBotRoles role = ROLE_INVALID;
    std::string spec;
    uint8 lootGroup = 0;

    // One based, so that zero can mean "put it wherever there is room". The engine counts
    // subgroups from zero, and conflating "the first subgroup" with "no preference" would
    // make the common case of an unplanned roster silently pile everyone into subgroup one.
    uint8 subGroup = 0;

    // Both zero until the member is provisioned, which is the one thing that distinguishes
    // an authored roster row from a character that exists.
    uint32 guid = 0;
    uint32 accountId = 0;

    bool IsProvisioned() const { return guid != 0; }
};

class RaidGuildMgr
{
    public:
        void Load();

        std::vector<RaidGuildMember> const& GetRoster() const { return m_roster; }
        RaidGuildMember const* FindMember(std::string const& name) const;

        // Authoring. Each writes the row through immediately, since a roster half in memory
        // and half on disk is the kind of state a crash mid-session turns into a mystery.
        bool AddMember(RaidGuildMember const& member, std::string& error);
        bool RemoveMember(std::string const& name, std::string& error);

        // Creates the `characters` row for a member that has none, and records the guid and
        // account it was given. Provisioning an already provisioned member is a no-op rather
        // than an error, so the command can be run over the whole roster repeatedly.
        bool ProvisionMember(std::string const& name, std::string& error);

        // Brings members into the world through the database load path, which is the only
        // one that can save them again. The leader's group is prepared once, up front, so
        // that no member ever has to create it and two arriving on the same tick cannot
        // both decide to.
        bool PrepareGroup(Player* pLeader, uint32 expectedSize, std::string& error);
        bool SummonMember(std::string const& name, Player* pLeader, std::string& error);
        bool DismissMember(std::string const& name, std::string& error);

        // Whether a member is in the world right now, which is a different question from
        // whether it is provisioned.
        Player* FindSummonedMember(RaidGuildMember const& member) const;

        // Finds or founds the named guild and puts every provisioned member in it. Members
        // that are not in the world are added straight to the guild tables, which is the
        // point: a roster of forty should not have to be summoned to be guilded.
        bool FormGuild(std::string const& guildName, Player* pMaster, uint32& added,
            uint32& failed, std::string& error);

        // Drops a member's personal instance binds. A roster member is a body following the
        // leader and not a raider with a lockout of its own, so the group's bind should be
        // the only thing deciding which copy of a map it walks into. An offline member
        // loses all of them, which is safe precisely because the group bind still routes
        // it; one already in the world keeps the bind for the map it is standing on and any
        // that the group agrees with, since unbinding those would be churn at best. Returns
        // how many were dropped.
        uint32 ClearMemberBinds(RaidGuildMember const& member, Group* pGroup);

        // How many personal binds a summoned member is holding, which should be nothing it
        // did not earn by walking through a door with the group.
        uint32 CountMemberBinds(RaidGuildMember const& member) const;

        void Update(uint32 diff);

    private:
        RaidGuildMember* FindMemberInternal(std::string const& name);
        uint32 AllocateAccountId() const;
        void SaveProvisioning(RaidGuildMember const& member) const;

        std::vector<RaidGuildMember> m_roster;

        // Subgroups cannot be set when a member is summoned, because it is not in the group
        // yet: the bot joins on its own first tick, some seconds later. So placement is
        // reconciled from here rather than commanded at the point of spawning.
        uint32 m_reconcileTimer = 0;
};

#define sRaidGuildMgr MaNGOS::Singleton<RaidGuildMgr>::Instance()

#endif
