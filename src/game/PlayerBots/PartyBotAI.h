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
#include "DungeonTactics.h"
#include "Group.h"
#include "ObjectAccessor.h"

// Where a designated puller has got to. Firing is its own step rather than part of the approach
// because a ranged attack does not leave the weapon the moment it is asked for: it is an auto repeat
// spell that fires on the weapon timer, and moving cancels it. A puller that turned for home as soon
// as it had cast would arrive back with the group having pulled nothing at all.
// Walk to where the order was given, shoot from there, wait for the mob to commit, walk it to the
// tank. Closing is a separate phase from approaching rather than a branch inside it, because the two
// walk in opposite directions: sharing a phase, arriving at the anchor and then setting off towards
// the mob reads as having left the anchor, and the puller paces between the two.
enum PartyBotPullPhase
{
    PULL_PHASE_NONE,
    PULL_PHASE_APPROACH,
    PULL_PHASE_CLOSE,
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
    // Asked rather than answered for. UpdateLootRolls votes on this bot's behalf, one roll per
    // tick, after DecideLootRoll has scored the item and after any person in the group has had
    // their say.
    bool AnswersLootRollsItself() const final { return true; }

    void CloneFromPlayer(Player const* pPlayer);
    bool AddToPlayerGroup();

    bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const final;
    // Deliberately hides the non-virtual base version rather than overriding it, so that every
    // cast the party bot rotations make goes through the rank choice below while the casts the
    // shared bot code makes for itself, which are buffs and heals, carry on unchanged.
    SpellCastResult DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry);
    bool IsOverThreatCeiling(Unit const* pTarget) const;
    // Which tank owns which target, so that a raid's worth of tanks does not all claim the
    // exemptions written for the single tank of a five man.
    bool IsAssignedTankFor(Unit const* pTarget) const;
    bool ShouldDumpThreatWithFeignDeath(Unit* pTarget) const;
    bool IsApproachAnywayTarget(Unit const* pTarget) const;
    bool ShouldChargeToPull() const;
    bool IsWorthALongCooldownCC(SpellEntry const* pSpellEntry, Unit const* pTarget) const;
    bool HasManaToSpendOnAbsorbs() const;
    bool HasManaWorthCastingWith() const;
    mutable time_t m_lastWandHoldLog = 0;
    // When the current wand refusal began, so it can be bounded rather than held for ever.
    uint32 m_wandHoldSince = 0;
    bool TryFreezingTrapSequence();
    void EndTrapAttempt(uint32 now);
    bool ShouldReserveGlobalCooldownForInterrupt(Unit const* pVictim) const;
    Unit* FindSummonWorthTrapping(float radius) const;

    // When the current feign-and-trap attempt began, and when the hunter may try another. Both
    // zero when no attempt is running. The deadline these carry is what makes the sequence
    // unable to livelock the way the removed version did.
    uint32 m_feignUntil = 0;
    uint32 m_trapAttemptStart = 0;
    uint32 m_trapStandDownUntil = 0;

    bool IsInOpeningRamp(Unit const* pTarget) const;
    void HoldOpeningSwings(Unit const* pTarget);
    float GetThreatPullRatio(Unit const* pTarget) const;
    float GetThreatHeadroom(Unit const* pTarget) const;
    float EstimateSpellThreat(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    SpellEntry const* PickRankForThreat(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    Player* GetPartyLeader() const;
    bool WaitForOfflineLeader();
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Player* pLeader) const;
    Unit* SelectPartyAttackTarget() const;
    bool CanInterruptWith(SpellEntry const* pSpellEntry, Unit const* pTarget) const;
    bool IsIgnoredByParty(Unit const* pEnemy) const;
    bool IsSummonWorthLeavingBossFor(Unit const* pAdd, bool logIt = true) const;
    Unit* FindSuspendedSummonNearby(float radius) const;
    Unit* FindFocusTotemForPet(float radius) const;

    // Instance tactics, and the generic combat behaviour that measuring one instance exposed as
    // missing everywhere. Only GetTacticalStandoff and the escort pair read m_tactics; the rest are
    // ungated, because nothing about interrupting a heal or finding a firing line is particular to
    // one dungeon.
    void RefreshDungeonTactics();
    float GetTacticalStandoff(Unit const* pTarget) const final;
    bool WouldLeaveHeldGround(float x, float y, float z) const final;
    // The other half of a hold line: get back on it after something threw the bot off.
    bool ReturnToHeldGround();
    // Whether this bot could fight this target at all without leaving the ground it holds.
    bool CanEngageFromHeldGround(Unit const* pTarget) const;
    // Step into sight of a party member the healer can reach but cannot see.
    bool RecoverHealLineOfSight();
    std::vector<uint32> const* GetApproachAnywayEntries() const final;
    bool GetFightAnchor(Unit const* pVictim, float& x, float& y, float& z,
                        float& radius) const final;
    bool IsEngagedWithGroup(Unit const* pEnemy) const;
    void SetGroupAttackOrder(ObjectGuid guid);
    void ClearGroupAttackOrder();
    bool HasThreatOnGroup(Unit const* pEnemy) const;
    Unit* SelectControlledLeftoverTarget() const;
    bool IsTargetInCurrentFight(Unit const* pTarget) const;
    Unit* SelectGroupFocusTarget() const;
    bool CrowdControlOffFocus();
    void GetInterruptSpells(std::vector<SpellEntry const*>& out) const;
    uint32 GetInterruptPriority(Unit const* pCaster) const;
    uint32 ScoreFocusCandidate(Unit const* pEnemy) const;
    uint32 GetWorstKnownCastPriority(Unit const* pEnemy) const;
    uint32 GetPowerReservedForInterrupt(Powers powerType, SpellEntry const* pSpellEntry) const;
    bool IsAnythingNearbyWorthInterrupting() const;
    Unit* SelectInterruptTarget(SpellEntry const* pInterruptSpell, uint32 minPriority,
                                bool mayPreempt) const;
    bool InterruptHostileCasters();
    Unit* SelectPeelTarget() const;
    bool PeelForTheHealer();

    // Step clear of anything nobody has pulled that this bot is standing too close to. The only
    // rule here that moves a bot for a reason outside the fight it is in, and the answer to
    // trouble that walks up to a bot rather than the other way round.
    bool AvoidUnpulledNeighbours();

    // Whether a taunt is available to use on this target this instant. Asked before a tank
    // considers peeling with its body, because a taunt does the same job from where it stands.
    bool HasTauntReadyFor(Unit const* pTarget) const;
    void FindGuardedEscorts(std::vector<Creature*>& out) const;
    Creature* FindGuardedEscort() const;
    Unit* SelectEscortAttackTarget() const;
    Unit* GetGuardedEscort() const override;
    bool RecoverLineOfSight();
    // One rule for every caster and healer: do not stand in a melee arc a raid boss can swing
    // through. Shared rather than per class, because half the rotations had their own version of
    // this and the other half had none.
    bool BackOutOfMeleeRange();

    // Walk a caster or healer back out to the distance the instance table asks for.
    //
    // Separate from BackOutOfMeleeRange because the two are about different things and only one
    // of them existed. That rule answers "something can swing at me"; this answers "I am inside
    // the radius of an area effect this creature is known to have", which is a fact about the
    // creature rather than about reach, and no amount of melee logic sees it.
    bool HoldTacticalStandoff();

    // A cast aimed at this bot that the instance tactics say to break line of sight against, or
    // null when there is none, when it is aimed at somebody else, or when the bot is already out
    // of sight and so has nothing left to do about it.
    Unit* FindCastToBreakSightFrom() const;
    // Step out of sight of it. True whenever the bot is walking to cover or already there, so that
    // the rest of the tick is left alone until it is done.
    bool TakeCoverFromCast();

    // The patch of hostile ground this bot is standing in, or null. Named by the instance table
    // rather than judged from the spell, because "damaging area aura on the floor" also describes
    // the party's own Blizzard and every totem pulse.
    DynamicObject* FindGroundHazardUnderfoot() const;
    // Walk out of it. True whenever the bot is on its way out or has just set off, so the rest of
    // the tick is left alone until it is clear.
    bool StepOutOfGroundHazard();

    bool StepAwayFromHeldAttacker();
    bool SafeMoveTo(float x, float y, float z);
    bool DragFightAwayFromNeighbours();
    bool GatherLooseEnemies();
    bool GetGatherAnchor(float& x, float& y, float& z) const;
    bool ShouldThisWarriorPeel(Unit* pAdd, bool onHealer) const;
    void ReconsiderMeleeChaseAngle();
    void GetFormationSlot(float& distance, float& angle) const;
    void GetSafeFormationSlot(Player const* pLeader, float& distance, float& angle) const;
    bool CanIssueCombatMovement() const;
    void NoteCombatMovement();
    void CollectLooseEnemies(std::vector<Unit*>& out) const;
    Player* SelectResurrectionTarget(SpellEntry const* pSpellEntry) const;
    bool UseSelfResurrection();
    void AddSelfResurrectionReagent();
    Player* SelectShieldTarget() const;
    Unit* GetMarkedTarget(RaidTargetIcon mark) const;
    bool CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const;
    bool DrinkAndEat();
    bool ShouldAutoRevive() const;
    bool IsPositionSafeToRise(float x, float y, float z) const;
    bool FindSafeRisePosition(Corpse* pCorpse, float& x, float& y, float& z) const;
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
    void CommandPetAttack(Pet* pPet, Unit* pTarget);
    void UpdatePetCombat();
    uint32 GetRangedAttackSpellId() const;
    float GetPullStandoffDistance() const;
    SpellEntry const* GetInstantPullSpell() const;
    bool FirePullAttack(Unit* pTarget);
    bool AddFillerDamage(Unit* pTarget);
    bool KeepBusy();
    bool IsWorthDotting(Unit const* pVictim) const;
    void SampleVictimHealth();
    float EstimateSecondsToLive(Unit const* pVictim) const;
    float EstimateSecondsPerComboPoint() const;
    void UpdateLootRolls();
    bool ShouldDeferRollToPlayers(Roll const* pRoll) const;
    bool DidPlayerNeedRoll(Roll const* pRoll) const;
    RollVote DecideLootRoll(uint32 itemId) const;
    void RememberCorpseToLoot(ObjectGuid guid);
    void UpdateCorpseLooting();
    bool CanAnyPlayerLoot(Creature* pCreature) const;
    bool LootCorpse(Creature* pCreature);
    bool IsCastingFillerDamage() const;
    bool IsCastingFillerAutoRepeat() const;
    Player* GetGroupTank() const;
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

    // A corpse the group killed, and the time after which whatever is left in it may be taken. The
    // delay is the group's window to pick what it wants; a corpse has to end up completely empty
    // before it can be skinned, so the bot takes the remainder rather than choosing among it.
    struct PartyBotCorpse
    {
        ObjectGuid guid;
        time_t lootAfter;
    };

    std::vector<PartyBotCorpse> m_corpsesToLoot;
    std::vector<RaidTargetIcon> m_marksToCC;
    // Skull by default, because that is what every group in the game already means by it, and an
    // empty list meant marking a target did nothing at all until somebody had run .partybot
    // focusmark first - so the one gesture a player would reach for to redirect the group was
    // silently inert. Further marks are still added by that command.
    std::vector<RaidTargetIcon> m_marksToFocus = { RAID_TARGET_ICON_SKULL };
    ShortTimeTracker m_updateTimer;
    // Throttle for the per-tick state line. Mutable because logging is the one thing a const
    // reporting function is allowed to change about the bot.
    mutable uint32 m_lastTickLog = 0;
    // The target whose health is being watched, what it last was, when it was last looked at,
    // and how fast it is falling. Behind EstimateSecondsToLive, which is what the rotations ask.
    ObjectGuid m_ttlVictimGuid;
    uint32 m_ttlLastHealth = 0;
    uint32 m_ttlLastSample = 0;
    float m_ttlDamagePerSecond = 0.0f;
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
    // When the owner went offline, or zero while they are here. A crashed client is indistinguishable
    // from a quit one at this level, so both are waited out: see WaitForOfflineLeader.
    time_t m_leaderOfflineSince = 0;
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
    // Where the close-in walk is currently headed, so it is re-issued when the mob moves and
    // not on every tick. Unset until the walk has been aimed once.
    float m_pullCloseX = 0.0f;
    float m_pullCloseY = 0.0f;
    bool m_pullCloseAimed = false;
    PartyBotPullPhase m_pullPhase = PULL_PHASE_NONE;
    time_t m_pullSince = 0;
    // When the shot currently being waited on was asked for, so that a queued autorepeat which is
    // never going to fire can be told apart from one that simply has not come round yet.
    time_t m_pullShotSince = 0;
    // What this instance asks of the bots, looked up once per map rather than per tick, and null
    // for the great majority of maps that ask nothing. The map it was looked up for is kept
    // alongside it because a bot changes maps without being reinitialised.
    DungeonTactics const* m_tactics = nullptr;
    uint32 m_tacticsMapId = 0;
    // When the walk to cover was started, or zero when there is no such walk. Needed because a
    // point move in flight is not on its own evidence of one: half this class asks for point moves,
    // and hiding behind a wall is the only one that is allowed to be the whole of a tick. Aged out
    // rather than trusted, so a walk that cannot finish -- shoved, rooted, or sent at a spot the
    // mesh changed its mind about -- releases the bot back to fighting on its own.
    uint32 m_breakSightSince = 0;
    // When the walk out of a cloud was started, for the same reason m_breakSightSince exists: a
    // point move in flight is not on its own evidence that this is the move in flight.
    uint32 m_groundHazardSince = 0;
    // What the bot last could not see, and for how many ticks running. A bot standing behind rock
    // is the single most common wasted tick in a cave instance, and the count is what tells a
    // corner that will clear itself apart from one that will not.
    // The last creature taunted off the healer, and when. Without it a mob still running from the
    // healer to the tank still counts as being on the healer, and gets taunted again every time it
    // is looked at.
    ObjectGuid m_lastPeelGuid;
    time_t m_lastPeelTime = 0;
    ObjectGuid m_blindTargetGuid;
    uint32 m_blindTicks = 0;
    time_t m_lastBlindStep = 0;
    // When this bot last walked out of a held mob's reach, so it steps once rather than every
    // tick for as long as the root lasts.
    time_t m_lastHeldStep = 0;
    // When this bot last stepped out of an unpulled creature's aggro radius, so it steps once and
    // then lets the follow or the chase have its say rather than shuffling every tick.
    time_t m_lastNeighbourStep = 0;
    time_t m_lastBackout = 0;
    // Rate limit for the standoff walk above, kept apart from m_lastBackout so that a bot doing
    // one is not silently prevented from doing the other.
    time_t m_lastStandoffWalk = 0;
    // One clock for every system that repositions a bot mid-fight, so they take turns rather than
    // fight each other.
    // When a warrior last changed target to collect an add, so that collecting cannot become a
    // warrior that changes its mind every tick and finishes nothing.
    time_t m_lastGatherSwitch = 0;
    // The add a warrior is currently peeling and the target it left to do it, so that a peel is a
    // detour rather than a change of plan.
    ObjectGuid m_gatherPeelTarget;
    ObjectGuid m_gatherReturnTarget;
    time_t m_gatherSwitchTime = 0;
    time_t m_lastCombatMove = 0;
    time_t m_lastFacingCheck = 0;
    // Where the last drag away from a neighbouring camp was aimed, kept only so the next drag can
    // report how far the tank has since strayed from it.
    float m_lastDragX = 0.0f;
    float m_lastDragY = 0.0f;
    float m_lastDragZ = 0.0f;
    time_t m_lastDragTime = 0;
    // Throttle for the "no poison to apply" line, which would otherwise repeat every tick a rogue
    // spends out of combat.
    time_t m_lastPoisonLog = 0;
    mutable time_t m_lastStandLog = 0;
    // Whether this death has already had the bot's standing orders torn up. Reset on rising, so
    // every death gets one clearing and no death gets one per tick.
    bool m_ordersClearedByDeath = false;
    // How many checks in a row have wanted the melee bot on the other side of its target.
    uint32 m_facingChangeStreak = 0;
    // Where the corpse run was last seen to have got somewhere, so a stalled run can be told
    // apart from a slow one. Negative distance means the run has not started yet.
    float m_corpseRunBestDistance = -1.0f;
    float m_corpseRunLastX = 0.0f;
    float m_corpseRunLastY = 0.0f;
    float m_corpseRunLastZ = 0.0f;
};

#endif
