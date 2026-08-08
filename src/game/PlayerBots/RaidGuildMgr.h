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

    private:
        RaidGuildMember* FindMemberInternal(std::string const& name);
        uint32 AllocateAccountId() const;
        void SaveProvisioning(RaidGuildMember const& member) const;

        std::vector<RaidGuildMember> m_roster;
};

#define sRaidGuildMgr MaNGOS::Singleton<RaidGuildMgr>::Instance()

#endif
