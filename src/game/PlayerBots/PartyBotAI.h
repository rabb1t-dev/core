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

#ifndef MANGOS_PARTYBOTAI_H
#define MANGOS_PARTYBOTAI_H

#include "CombatBotBaseAI.h"
#include "Group.h"
#include "ObjectAccessor.h"

// Where a designated puller has got to. Firing is its own step rather than part of the approach
// because a ranged attack does not leave the weapon the moment it is asked for: it is an auto repeat
// spell that fires on the weapon timer, and moving cancels it. A puller that turned for home as soon
// as it had cast would arrive back with the group having pulled nothing at all.
enum PartyBotPullPhase
{
    PULL_PHASE_NONE,
    PULL_PHASE_APPROACH,
    PULL_PHASE_FIRE,
    PULL_PHASE_RETURN,
};

class PartyBotAI : public CombatBotBaseAI
{
public:

    PartyBotAI(Player* pLeader, Player* pClone, CombatBotRoles role, uint8 race, uint8 class_, uint8 level, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : CombatBotBaseAI(), m_race(race), m_class(class_), m_level(level), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o)
    {
        m_role = role;
        m_leaderGuid = pLeader->GetObjectGuid();
        m_cloneGuid = pClone ? pClone->GetObjectGuid() : ObjectGuid();
        m_updateTimer.Reset(2000);
    }
    PartyBotAI(Player* pLeader, uint32 mapId, uint32 instanceId, float x, float y, float z, float o)
        : CombatBotBaseAI(), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o)
    {
        m_role = ROLE_INVALID;
        m_leaderGuid = pLeader->GetObjectGuid();
        m_updateTimer.Reset(2000);
    }

    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) final;
    void OnPlayerLogin() final;
    void UpdateAI(uint32 const diff) final;
    void OnPacketReceived(WorldPacket const* packet) final;

    void CloneFromPlayer(Player const* pPlayer);
    bool AddToPlayerGroup();

    bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const final;
    // Deliberately hides the non-virtual base version rather than overriding it, so that every
    // cast the party bot rotations make goes through the rank choice below while the casts the
    // shared bot code makes for itself, which are buffs and heals, carry on unchanged.
    SpellCastResult DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry);
    bool IsOverThreatCeiling(Unit const* pTarget) const;
    bool IsInOpeningRamp(Unit const* pTarget) const;
    void HoldOpeningSwings(Unit const* pTarget);
    float GetThreatPullRatio(Unit const* pTarget) const;
    float GetThreatHeadroom(Unit const* pTarget) const;
    float EstimateSpellThreat(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    SpellEntry const* PickRankForThreat(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    Player* GetPartyLeader() const;
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Player* pLeader) const;
    Unit* SelectPartyAttackTarget() const;
    Player* SelectResurrectionTarget(SpellEntry const* pSpellEntry) const;
    bool UseSelfResurrection();
    void AddSelfResurrectionReagent();
    Player* SelectShieldTarget() const;
    Unit* GetMarkedTarget(RaidTargetIcon mark) const;
    bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const;
    bool DrinkAndEat();
    bool ShouldAutoRevive() const;
    bool IsGroupInCombat() const;
    Player* FindGroupHealer() const;
    bool FindInstanceEntrance(uint32 instanceMapId, float& x, float& y, float& z) const;
    bool WaitForLeaderBeforeRising();
    void LogDeathHold(char const* reason);
    void BeginHold(float x, float y, float z, ObjectGuid pullTargetGuid);
    void ReleaseHold();
    bool IsHolding() const { return m_holdPosition; }
    bool ShouldBreakHold() const;
    bool BeginPull(Unit* pTarget, float anchorX, float anchorY, float anchorZ);
    bool IsPulling() const { return m_pullPhase != PULL_PHASE_NONE; }
    void EndPull();
    bool UpdatePullSequence();
    void LogPull(char const* what) const;
    void HoldPet(bool hold);
    uint32 GetRangedAttackSpellId() const;
    float GetPullStandoffDistance() const;
    bool FirePullAttack(Unit* pTarget);
    Unit* SelectHealTargetOutOfReach() const;
    Unit const* GetCurrentFollowTarget() const;
    uint32 ScaleTankRage(uint32 rage) const;
    void LogCombatTick() const;
    bool UpdateCorpseRun();
    void UpdateDeadAI();
    bool IsValidDistancingTarget(Unit* pTarget, Unit* pEnemy);
    Unit* GetDistancingTarget(Unit* pEnemy);
    bool RunAwayFromTarget(Unit* pEnemy);
    bool CrowdControlMarkedTargets();
    bool EnterCombatDruidForm();
    bool ShouldEnterStealth() const;
    bool EnterStealthIfNeeded(SpellEntry const* pStealthSpell);

    bool CheckForDispelTargets();
    void UpdateInCombatAI() final;
    void UpdateOutOfCombatAI() final;
    void UpdateInCombatAI_Paladin() final;
    void UpdateOutOfCombatAI_Paladin() final;
    void UpdateInCombatAI_Shaman() final;
    void UpdateOutOfCombatAI_Shaman() final;
    void UpdateInCombatAI_Hunter() final;
    void UpdateOutOfCombatAI_Hunter() final;
    void UpdateInCombatAI_Mage() final;
    void UpdateOutOfCombatAI_Mage() final;
    void UpdateInCombatAI_Priest() final;
    void UpdateOutOfCombatAI_Priest() final;
    void UpdateInCombatAI_Warlock() final;
    void UpdateOutOfCombatAI_Warlock() final;
    void UpdateInCombatAI_Warrior() final;
    void UpdateInCombatAI_WarriorTank(Unit* pVictim);
    bool ShouldTauntTarget(Unit const* pVictim) const;
    void UpdateOutOfCombatAI_Warrior() final;
    void UpdateInCombatAI_Rogue() final;
    void UpdateOutOfCombatAI_Rogue() final;
    void UpdateInCombatAI_Druid() final;
    void UpdateOutOfCombatAI_Druid() final;

    std::vector<RaidTargetIcon> m_marksToCC;
    std::vector<RaidTargetIcon> m_marksToFocus;
    ShortTimeTracker m_updateTimer;
    ObjectGuid m_leaderGuid;
    ObjectGuid m_cloneGuid;
    uint8 m_race = 0;
    uint8 m_class = 0;
    uint8 m_level = 0;
    uint32 m_mapId = 0;
    uint32 m_instanceId = 0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    float m_o = 0.0f;
    bool m_resetSpellData = false;
    // How long this bot has been waiting at each stage of death, so one that cannot recover
    // on its own gets bailed out rather than stalling the group.
    time_t m_corpseSince = 0;
    time_t m_ghostSince = 0;
    time_t m_ghostStart = 0;
    // When this bot reached its corpse and began holding for the leader to arrive, so the hold
    // can be given up on rather than lasting as long as the leader stays away.
    time_t m_leaderWaitSince = 0;
    // Throttle for the "still down" lines, which are otherwise asked for once a second for as
    // long as the bot stays dead.
    time_t m_lastDeathLog = 0;
    // Coordinated pull. m_holdPosition itself lives on the base class, next to the chase it has to
    // be able to refuse. The anchor is remembered so that the puller has somewhere to come back to,
    // and so a bot shoved off its spot has somewhere to return to.
    float m_holdX = 0.0f;
    float m_holdY = 0.0f;
    float m_holdZ = 0.0f;
    time_t m_holdSince = 0;
    // What the hold is waiting for. Empty for a hold asked for on its own, which then waits only for
    // the order to release.
    ObjectGuid m_pullTargetGuid;
    PartyBotPullPhase m_pullPhase = PULL_PHASE_NONE;
    time_t m_pullSince = 0;
    // Where the corpse run was last seen to have got somewhere, so a stalled run can be told
    // apart from a slow one. Negative distance means the run has not started yet.
    float m_corpseRunBestDistance = -1.0f;
    float m_corpseRunLastX = 0.0f;
    float m_corpseRunLastY = 0.0f;
    float m_corpseRunLastZ = 0.0f;
};

#endif
