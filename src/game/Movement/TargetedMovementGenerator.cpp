/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
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

#include "ByteBuffer.h"
#include "TargetedMovementGenerator.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Player.h"
#include "Map.h"
#include "World.h"
#include "MoveSplineInit.h"
#include "MoveSpline.h"
#include "Anticheat.h"
#include "Transport.h"
#include "TemporarySummon.h"
#include "GameObjectAI.h"
#include "Geometry.h"
#include "Utilities/Random.h"

// Slack on top of the creature's own radius, so a route is not approved a hand's breadth outside it
// and then drifted over the line by the next step.
static constexpr float PULL_CHECK_MARGIN = 3.0f;

// The creature this path would wake, or nothing if the whole route is clear.
//
// Every point is tested and not merely the destination, because a clean spot reached by walking
// through a pack is not a clean spot. That is the limit the checks in the bot AI ran into: they
// judged the endpoint a move was aimed at, which reduces pulls and cannot prevent them, and they
// judged it once when the move was issued, while both of the generators here re-pick a destination
// continuously for as long as the target keeps moving. This is the only place with the finished path
// in hand and a chance to decline it.
//
// Applies to nothing that has not asked for it. Real players never reach these generators for their
// own movement, so the Player instantiation is bots and charmed units, and the flag narrows it again
// to the party bots that want the rule.
static Creature* FindPullOnPath(Unit const& owner, PathFinder const& path)
{
    Player const* pPlayer = owner.ToPlayer();
    if (!pPlayer || !pPlayer->AvoidsAggroPulls())
        return nullptr;

    // Under orders, so the rule steps aside. Told to go and fight something, a bot that stops short
    // because the way there is not clean has refused the instruction while looking like it accepted
    // it, and there is no route to most things in a dungeon that passes nothing else.
    if (pPlayer->HasAttackOrders())
        return nullptr;

    PointsArray const& points = path.getPath();
    if (points.empty())
        return nullptr;

    // Hoisted out of the loop below: this is a cell visit over sixty yards and the path can be
    // dozens of points long.
    std::list<Unit*> enemies;
    owner.GetEnemyListInRadiusAround(&owner, Unit::AGGRO_POSITION_SEARCH_RADIUS, enemies);
    if (enemies.empty())
        return nullptr;

    for (Unit* pEnemy : enemies)
    {
        Creature* pCreature = pEnemy->ToCreature();
        if (!pCreature)
            continue;

        // A route only risks a pull where it takes the bot nearer to this creature than it already
        // stands. Judged against the aggro band alone, a bot that has come to rest inside the band
        // finds every point of every route in violation and cannot move at all -- not even away
        // from the thing it is avoiding. That is not a corner case: the band is the aggro radius
        // plus a margin, so a bot standing a stride outside a mob's aggro radius is inside it, and
        // Wailing Caverns had two melee bots frozen through an entire fight 18 yards from an 18
        // yard radius while the rest of the group killed things around them. Its own distance is
        // the floor, which leaves retreating and circling available and refuses only real closing.
        float const ownDistance = pCreature->GetDistance(owner.GetPositionX(), owner.GetPositionY(), owner.GetPositionZ());

        for (auto const& point : points)
        {
            if (pCreature->GetDistance(point.x, point.y, point.z) >= ownDistance)
                continue;

            if (owner.WouldPositionAggroCreature(pCreature, point.x, point.y, point.z, PULL_CHECK_MARGIN))
                return pCreature;
        }
    }

    return nullptr;
}

// Decline the move and stand still. Returning without launching leaves the generator to ask again on
// its next update, so a bot held here resumes by itself the moment the route clears, whether that is
// because the pack got pulled by someone else, died, or the target moved somewhere reachable.
static bool RefusePathThatWouldPull(Unit& owner, PathFinder const& path, char const* movement)
{
    Creature* pCreature = FindPullOnPath(owner, path);
    if (!pCreature)
        return false;

    if (Player* pPlayer = owner.ToPlayer())
    {
        // Which movement was refused matters more than the refusal. A chase declined leaves a bot
        // idle in a fight it should be in; a follow declined leaves it behind on the way to one,
        // and the two want different answers. The old line named neither.
        if (pPlayer->ShouldLogPullBlock())
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] pullblock bot='%s' lvl=%u refused a %s route past '%s' (lvl %u, aggro "
                     "%.1fy, %.1fy away) on map %u and is holding position",
                     pPlayer->GetName(), pPlayer->GetLevel(), movement, pCreature->GetName(),
                     pCreature->GetLevel(), pCreature->GetAttackDistance(&owner),
                     pCreature->GetDistance(&owner), pPlayer->GetMapId());
    }

    if (!owner.movespline->Finalized())
        owner.StopMoving();

    return true;
}

//-----------------------------------------------//
template<class T, typename D>
bool TargetedMovementGeneratorMedium<T, D>::IsFarEnoughToMoveStationaryFollower(T &owner) const
{
    return !i_target->IsWithinDist(&owner, 1.4f * m_fOffset);
}

template<>
void TargetedMovementGeneratorMedium<Player, ChaseMovementGenerator<Player> >::UpdateFinalDistance(float /*fDistance*/)
{
    // nothing to do for Player
}

template<>
void TargetedMovementGeneratorMedium<Player, FollowMovementGenerator<Player> >::UpdateFinalDistance(float /*fDistance*/)
{
    // nothing to do for Player
}

template<>
void TargetedMovementGeneratorMedium<Creature, ChaseMovementGenerator<Creature> >::UpdateFinalDistance(float fDistance)
{
    m_fOffset = fDistance;
    m_bRecalculateTravel = true;
}

template<>
void TargetedMovementGeneratorMedium<Creature, FollowMovementGenerator<Creature> >::UpdateFinalDistance(float fDistance)
{
    m_fOffset = fDistance;
    m_bRecalculateTravel = true;
}

template<class T, typename D>
void TargetedMovementGeneratorMedium<T, D>::UpdateAsync(T &owner, uint32 /*diff*/)
{
    if (!m_bRecalculateTravel)
        return;
    // All these cases will be handled at next sync update
    if (!i_target.isValid() || !i_target->IsInWorld() || !owner.IsAlive() || owner.HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED)
            || static_cast<D*>(this)->_lostTarget(owner)
            || owner.IsNoMovementSpellCasted())
        return;

    // Lock async updates for safety, see Unit::asyncMovesplineLock doc
    std::unique_lock<std::mutex> guard(owner.asyncMovesplineLock);
    _setTargetLocation(owner);
}

//-----------------------------------------------//

template<class T>
void ChaseMovementGenerator<T>::_setTargetLocation(T &owner)
{
    // Note: Any method that accesses the target's movespline here must be
    // internally locked by the target's spline lock
    if (!i_target.isValid() || !i_target->IsInWorld())
        return;

    if (owner.HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
        return;

    float x, y, z;
    bool losChecked = false;
    bool losResult = false;

    GenericTransport* transport = owner.GetTransport();

    m_bTargetOnTransport = i_target.getTarget()->GetTransport();
    i_target->GetPosition(m_fTargetLastX, m_fTargetLastY, m_fTargetLastZ, i_target.getTarget()->GetTransport());

    // Can't path to target if transports are different.
    if (owner.GetTransport() != i_target.getTarget()->GetTransport())
    {
        m_bReachable = false;
        return;
    }

    if (!m_fOffset)
    {
        if (owner.CanReachWithMeleeAutoAttack(i_target.getTarget()))
        {
            bool futureOkay = true;

            if (i_target->IsMoving())
            {
                Position futurePos;
                if (i_target->ExtrapolateMovement(i_target->m_movementInfo, 200, futurePos.x, futurePos.y, futurePos.z, futurePos.o))
                    if (!i_target->CanReachWithMeleeAutoAttackAtPosition(&owner, futurePos.x, futurePos.y, futurePos.z))
                        futureOkay = false;
            }

            if (futureOkay)
            {
                losResult = owner.IsWithinLOSInMap(i_target.getTarget());
                losChecked = true;
                if (losResult)
                    return;
            }
        }

        // Avoid collisions between mobs.
        // This function takes on a random angle.
        if (!i_target->GetRandomAttackPoint(&owner, x, y, z))
        {
            m_bReachable = false;
            return;
        }
    }
    // prevent redundant micro-movement for pets, other followers.
    else if (!i_target->IsMoving() && owner.movespline->Finalized() && !this->IsFarEnoughToMoveStationaryFollower(owner))
    {
        return;
    }
    else
    {
        // to at m_fOffset distance from target and m_fAngle from target facing
        float srcX, srcY, srcZ;
        i_target->GetSafePosition(srcX, srcY, srcZ, transport);
        if (transport)
            transport->CalculatePassengerPosition(srcX, srcY, srcZ);

        // TRANSPORT_VMAPS
        if (transport)
        {
            i_target->GetNearPoint2D(x, y, m_fOffset, m_fAngle + i_target->GetOrientation());
            z = srcZ;
        }
        else
        {
            float o;
            if (!(sWorld.getConfig(CONFIG_BOOL_ENABLE_MOVEMENT_EXTRAPOLATION_PET) &&
                i_target->ExtrapolateMovement(i_target->m_movementInfo, (WorldTimer::getMSTime() - i_target->m_movementInfo.stime) + 500, x, y, z, o)))
            {
                i_target->GetPosition(x, y, z);
                o = i_target->GetOrientation();
            }

            i_target->GetNearPointAroundPosition(&owner, x, y, z, owner.GetObjectBoundingRadius(), m_fOffset, o + m_fAngle);
        }

        if (!i_target->m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) && !i_target->IsInWater())
            if (!owner.GetMap()->GetWalkHitPosition(transport, srcX, srcY, srcZ, x, y, z))
                i_target->GetSafePosition(x, y, z);
    }

    Movement::MoveSplineInit init(owner, "ChaseMovementGenerator<T>::_setTargetLocation");
    PathFinder path(&owner);
    path.SetTransport(transport);
    path.calculate(x, y, z, false);

    PathType pathType = path.getPathType();
    m_bReachable = pathType & (PATHFIND_NORMAL | PATHFIND_DEST_FORCED);

    if (pathType == PATHFIND_NOPATH)
        return;

    if (owner.IsPet())
    {
        // prevent pets from going through closed doors
        path.CutPathWithDynamicLoS();
        if (path.getPath().size() == 2 && path.Length() < 0.1f)
        {
            m_bReachable = false;
            return;
        }
    }

    if (!m_bReachable && !!(pathType & PATHFIND_INCOMPLETE) && owner.HasUnitState(UNIT_STATE_ALLOW_INCOMPLETE_PATH))
        m_bReachable = true;

    // Chase is refused if the path would pull something unengaged, with one exception: if the bot
    // is already in active combat with this specific target, the fight is happening regardless and
    // the bot needs to be able to close in. Refusing the path here leaves a melee bot standing
    // still while its target is right in front of it -- the rogue staring at a mob a few yards
    // away that was not moving because an adjacent unengaged mob sat inside the path's margin.
    // Following does not get this exemption because the leader can be anywhere.
    bool const botIsChasing = i_target.getTarget() && owner.GetVictim() == i_target.getTarget();
    if (!botIsChasing && RefusePathThatWouldPull(owner, path, "chase"))
        return;

    m_bRecalculateTravel = false;
    if (!transport && owner.HasDistanceCasterMovement() &&
        path.UpdateForCaster(i_target.getTarget(), owner.GetMinChaseDistance()))
    {
        if (!owner.movespline->Finalized())
            owner.StopMoving();
        return;
    }

    // Prevent redundant moves
    if ((path.Length() < 4.0f && (i_target->GetPositionZ() - owner.GetPositionZ()) > 10.0f) || // He is flying too high for me. Moving a few meters wont change anything.
        (pathType & PATHFIND_NOPATH) ||
        (pathType & PATHFIND_INCOMPLETE && !owner.HasUnitState(UNIT_STATE_ALLOW_INCOMPLETE_PATH)) ||
        !m_bReachable)
    {
        if (!losChecked)
            losResult = owner.IsWithinLOSInMap(i_target.getTarget());
        if (losResult)
        {
            if (!owner.movespline->Finalized())
                owner.StopMoving();
            return;
        }
    }

    _addUnitStateMove(owner);
    m_bTargetReached = false;

    init.Move(&path);
    init.SetWalk(EnableWalking());

    // Make player face target he is chasing (player does not automatically face target like creature).
    if (owner.IsPlayer())
        init.SetFacingGUID(i_target->GetGUID());

    init.Launch();
    m_checkDistanceTimer.Reset(500);

    // Fly-hack
    if (Player* player = i_target->ToPlayer())
    {
        float allowed_dist = owner.GetCombatReach(false) + i_target->GetCombatReach(false) + 5.0f;
        G3D::Vector3 dest = owner.movespline->FinalDestination();
        if ((player->GetPositionZ() - allowed_dist - 5.0f) > dest.z)
            player->GetCheatData()->OnUnreachable(&owner);
    }
}

template<class T>
bool ChaseMovementGenerator<T>::Update(T &owner, uint32 const&  time_diff)
{
    if (!i_target.isValid() || !i_target->IsInWorld())
        return false;

    if (!owner.IsAlive())
        return true;

    if (owner.movespline->IsUninterruptible() && !owner.movespline->Finalized())
        return true;

    if (owner.HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
    {
        _clearUnitStateMove(owner);
        return true;
    }

    // prevent movement while casting spells with cast time or channel time
    // don't stop creature movement for spells without interrupt movement flags
    if (owner.IsNoMovementSpellCasted())
    {
        if (!owner.IsStopped())
            owner.StopMoving();
        return true;
    }

    // prevent crash after creature killed pet
    if (_lostTarget(owner))
    {
        _clearUnitStateMove(owner);
        return true;
    }

    bool interrupted = false;
    m_checkDistanceTimer.Update(time_diff);
    if (m_checkDistanceTimer.Passed())
    {
        m_checkDistanceTimer.Reset(100);
        if (m_bIsSpreading)
        {
            if (!owner.movespline->Finalized() && !owner.CanReachWithMeleeAutoAttack(i_target.getTarget()))
            {
                owner.movespline->_Interrupt();
                interrupted = true;
            }
        }
        else
        {
            //More distance let have better performance, less distance let have more sensitive reaction at target move.
            if (!owner.movespline->Finalized() && i_target->IsWithinDist(&owner, 0.0f) && !m_fOffset)
            {
                owner.movespline->_Interrupt();
                interrupted = true;
            }
            else
            {
                float allowed_dist = owner.GetMaxChaseDistance(i_target.getTarget());
                bool targetMoved = false;
                G3D::Vector3 dest(m_fTargetLastX, m_fTargetLastY, m_fTargetLastZ);
                if (GenericTransport* ownerTransport = owner.GetTransport())
                {
                    if (m_bTargetOnTransport)
                        ownerTransport->CalculatePassengerPosition(dest.x, dest.y, dest.z);
                    else
                        targetMoved = true;
                }
                else if (m_bTargetOnTransport)
                    targetMoved = true;

                if (!targetMoved)
                    targetMoved = (dest.x != i_target->GetPositionX() || dest.y != i_target->GetPositionY() || dest.z != i_target->GetPositionZ());

                // Chase movement may be interrupted
                if (!targetMoved)
                    if (owner.movespline->Finalized())
                        targetMoved = !owner.IsWithinDist3d(dest.x, dest.y, dest.z, allowed_dist, SizeFactor::None);

                if (targetMoved)
                {
                    m_bRecalculateTravel = true;
                    owner.GetMotionMaster()->SetNeedAsyncUpdate();
                }
                else
                {
                    // Fly-hack
                    if (Player* player = i_target->ToPlayer())
                        if ((player->GetPositionZ() - allowed_dist - 5.0f) > dest.z)
                            player->GetCheatData()->OnUnreachable(&owner);
                }
            }
        }
    }

    if (owner.movespline->Finalized())
    {
        if (owner.IsCreature() && !owner.HasInArc(i_target.getTarget(), 0.01f))
            owner.SetInFront(i_target.getTarget());

        if (m_bIsSpreading)
            m_bIsSpreading = false;
        else
        {
            MovementInform(owner);

            if (!m_bTargetReached)
            {
                m_uiSpreadAttempts = 0;
                m_bCanSpread = true;
                m_bTargetReached = true;
                _reachTarget(owner);
            }
        }

        if (interrupted)
            owner.StopMoving();

        if (interrupted || owner.IsPlayer() && !owner.HasInArc(i_target.getTarget(), M_PI_F / 2.0f))
            owner.SetFacingTo(owner.GetAngle(i_target.getTarget()));

        m_spreadTimer.Update(time_diff);
        if (m_spreadTimer.Passed())
        {
            m_spreadTimer.Reset(urand(2500, 3500));
            if (Creature* creature = owner.ToCreature())
            {
                if (!creature->HasExtraFlag(CREATURE_FLAG_EXTRA_CHASE_GEN_NO_BACKING) && !creature->IsPet() && !i_target.getTarget()->IsMoving())
                {
                    if (m_bRecalculateTravel && TargetDeepInBounds(owner, i_target.getTarget()))
                        DoBackMovement(owner, i_target.getTarget());
                    else if (m_bCanSpread)
                        DoSpreadIfNeeded(owner, i_target.getTarget());
                }
            }
        }

        // Mobs should chase you infinitely if you stop and wait every few seconds.
        m_leashExtensionTimer.Update(time_diff);
        if (m_leashExtensionTimer.Passed())
        {
            m_leashExtensionTimer.Reset(5000);
            if (Creature* creature = owner.ToCreature())
                creature->UpdateLeashExtensionTime();
        }
    }
    else if (m_bRecalculateTravel)
    {
        m_leashExtensionTimer.Reset(5000);
        owner.GetMotionMaster()->SetNeedAsyncUpdate();
    }

    return true;
}

template<class T>
bool ChaseMovementGenerator<T>::TargetDeepInBounds(T &owner, Unit* target) const
{
    return TargetWithinBoundsPercentDistance(owner, target, 0.5f);
}

template<class T>
bool ChaseMovementGenerator<T>::TargetWithinBoundsPercentDistance(T &owner, Unit* target, float pct) const
{
    float radius = std::min(target->GetObjectBoundingRadius(), owner.GetObjectBoundingRadius());

    radius *= pct;

    return owner.GetDistanceSqr(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()) < radius;
}

template<class T>
void ChaseMovementGenerator<T>::DoBackMovement(T &owner, Unit* target)
{
    float x, y, z;
    target->GetClosePoint(x, y, z, target->GetObjectBoundingRadius() + owner.GetObjectBoundingRadius(), 1.0f, m_fAngle, &owner);

    // Don't move beyond attack range.
    if (!owner.CanReachWithMeleeAutoAttackAtPosition(target, x, y, z, 0.0f))
        return;

    m_bIsSpreading = true;
    Movement::MoveSplineInit init(owner, "ChaseMovementGenerator<T>::DoBackMovement");
    init.MoveTo(x, y, z, MOVE_WALK_MODE);
    init.SetWalk(true);
    init.Launch();
}

#define MAX_SPREAD_ATTEMPTS 3

template<class T>
void ChaseMovementGenerator<T>::DoSpreadIfNeeded(T &owner, Unit* target)
{
    // Move away from any NPC deep in our bounding box. There's no limit to the
    // angle moved; NPCs will eventually start spreading behind the target if
    // there's enough of them.
    Unit* pSpreadingTarget = nullptr;

    for (auto& attacker : target->GetAttackers())
    {
        if (attacker->IsCreature() && (attacker != &owner) &&
            (owner.GetObjectBoundingRadius() - 2.0f < attacker->GetObjectBoundingRadius()) &&
            !attacker->IsMoving() &&
            (owner.GetDistanceSqr(attacker->GetPositionX(), attacker->GetPositionY(), attacker->GetPositionZ()) < std::min(std::max(owner.GetObjectBoundingRadius(), attacker->GetObjectBoundingRadius()), 0.25f)))
        {
            pSpreadingTarget = attacker;
            break;
        }
    }

    if (!pSpreadingTarget)
    {
        m_bCanSpread = false;
        return;
    }

    float const my_angle = target->GetAngle(&owner);
    float const his_angle = target->GetAngle(pSpreadingTarget);
    float const new_angle = (his_angle > my_angle) ? my_angle - frand(0.4f, 1.0f) : my_angle + frand(0.4f, 1.0f);

    float x, y, z;
    target->GetNearPoint(&owner, x, y, z, owner.GetObjectBoundingRadius(), frand(0.8f, (target->GetAttackers().size() > 5 ? 4.0f : 2.0f)), new_angle);

    // Don't move beyond attack range.
    if (!owner.CanReachWithMeleeAutoAttackAtPosition(target, x, y, z, 0.0f))
        return;

    m_bIsSpreading = true;
    m_uiSpreadAttempts++;

    // Don't circle target infinitely if too many attackers.
    if (m_uiSpreadAttempts >= MAX_SPREAD_ATTEMPTS)
        m_bCanSpread = false;

    Movement::MoveSplineInit init(owner, "ChaseMovementGenerator<T>::DoSpreadIfNeeded");
    init.MoveTo(x, y, z, MOVE_WALK_MODE);
    init.SetWalk(true);
    init.Launch();
}

//-----------------------------------------------//
template<class T>
void ChaseMovementGenerator<T>::_reachTarget(T &owner)
{
    if (owner.CanReachWithMeleeAutoAttack(this->i_target.getTarget()))
        owner.Attack(this->i_target.getTarget(), true);
}

template<>
void ChaseMovementGenerator<Player>::Initialize(Player &owner)
{
    owner.AddUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
    m_bRecalculateTravel = true;
    owner.GetMotionMaster()->SetNeedAsyncUpdate();
}

template<>
void ChaseMovementGenerator<Creature>::Initialize(Creature &owner)
{
    owner.SetWalk(false, false);
    owner.AddUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
    m_bRecalculateTravel = true;
    owner.GetMotionMaster()->SetNeedAsyncUpdate();
}

template<class T>
void ChaseMovementGenerator<T>::Finalize(T &owner)
{
    owner.ClearUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
    //MovementInform(owner);
}

template<class T>
void ChaseMovementGenerator<T>::Interrupt(T &owner)
{
    owner.ClearUnitState(UNIT_STATE_CHASE | UNIT_STATE_CHASE_MOVE);
}

template<class T>
void ChaseMovementGenerator<T>::Reset(T &owner)
{
    Initialize(owner);
}

template<class T>
void ChaseMovementGenerator<T>::MovementInform(T& /*unit*/)
{
}

template<>
void ChaseMovementGenerator<Creature>::MovementInform(Creature& unit)
{
    if (!unit.IsAlive())
        return;

    // Pass back the GUIDLow of the target. If it is pet's owner then PetAI will handle
    if (unit.AI())
        unit.AI()->MovementInform(CHASE_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());

    if (unit.IsTemporarySummon())
    {
        TemporarySummon* pSummon = (TemporarySummon*)(&unit);
        if (pSummon->GetSummonerGuid().IsCreature())
        {
            if (Creature* pSummoner = unit.GetMap()->GetCreature(pSummon->GetSummonerGuid()))
                if (pSummoner->AI())
                    pSummoner->AI()->SummonedMovementInform(&unit, CHASE_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());
        }
        else
        {
            if (GameObject* pSummoner = unit.GetMap()->GetGameObject(pSummon->GetSummonerGuid()))
                if (pSummoner->AI())
                    pSummoner->AI()->SummonedMovementInform(&unit, CHASE_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());
        }
    }
}

//-----------------------------------------------//

template<class T>
void FollowMovementGenerator<T>::_setTargetLocation(T &owner)
{
    // Note: Any method that accesses the target's movespline here must be
    // internally locked by the target's spline lock
    if (!i_target.isValid() || !i_target->IsInWorld())
        return;

    if (owner.HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
        return;

    float x, y, z;

    // Can switch transports during follow movement.
    GenericTransport* transport = i_target.getTarget()->GetTransport();
    if (transport != owner.GetTransport())
    {
        if (owner.GetTransport())
            owner.GetTransport()->RemoveFollowerFromTransport(i_target.getTarget(), &owner);

        if (transport)
            transport->AddFollowerToTransport(i_target.getTarget(), &owner);
    }

    m_bTargetOnTransport = i_target.getTarget()->GetTransport();
    i_target->GetPosition(m_fTargetLastX, m_fTargetLastY, m_fTargetLastZ, i_target.getTarget()->GetTransport());

    // Can't path to target if transports are still different.
    if (owner.GetTransport() != i_target.getTarget()->GetTransport())
    {
        m_bReachable = false;
        return;
    }


    // prevent redundant micro-movement for pets, other followers.
    if (!i_target->IsMoving() && owner.movespline->Finalized() && !this->IsFarEnoughToMoveStationaryFollower(owner))
        return;

    // to at m_fOffset distance from target and m_fAngle from target facing
    float srcX, srcY, srcZ;
    i_target->GetSafePosition(srcX, srcY, srcZ, transport);
    if (transport)
        transport->CalculatePassengerPosition(srcX, srcY, srcZ);

    // TRANSPORT_VMAPS
    if (transport)
    {
        i_target->GetNearPoint2D(x, y, m_fOffset, m_fAngle + i_target->GetOrientation());
        z = srcZ;
    }
    else
    {
        float o;
        if (!(sWorld.getConfig(CONFIG_BOOL_ENABLE_MOVEMENT_EXTRAPOLATION_PET) &&
            i_target->ExtrapolateMovement(i_target->m_movementInfo, (WorldTimer::getMSTime() - i_target->m_movementInfo.stime) + 500, x, y, z, o)))
        {
            i_target->GetPosition(x, y, z);
            o = i_target->GetOrientation();
        }

        i_target->GetNearPointAroundPosition(&owner, x, y, z, owner.GetObjectBoundingRadius(), m_fOffset, o + m_fAngle);
    }

    if (!i_target->m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) && !i_target->IsInWater())
        if (!owner.GetMap()->GetWalkHitPosition(transport, srcX, srcY, srcZ, x, y, z))
            i_target->GetSafePosition(x, y, z);

    PathFinder path(&owner);

    // allow pets following their master to cheat while generating paths
    Movement::MoveSplineInit init(owner, "FollowMovementGenerator<T>::_setTargetLocation");
    path.SetTransport(transport);
    path.calculate(x, y, z, true);

    PathType pathType = path.getPathType();
    m_bReachable = pathType & (PATHFIND_NORMAL | PATHFIND_DEST_FORCED);

    if (!m_bReachable && !!(pathType & PATHFIND_INCOMPLETE) && owner.HasUnitState(UNIT_STATE_ALLOW_INCOMPLETE_PATH))
        m_bReachable = true;

    // The follow is where most of the pulling actually came from, and the one an endpoint check in
    // the AI could never have covered: a bot trailing its leader re-picks this spot every time the
    // leader moves, so the route is chosen fresh several times a second all the way down a corridor.
    if (RefusePathThatWouldPull(owner, path, "follow"))
        return;

    m_bRecalculateTravel = false;

    // Prevent redundant moves
    // He is flying too high for me. Moving a few meters wont change anything.
    if (path.Length() < 4.0f && (i_target->GetPositionZ() - owner.GetPositionZ()) > 10.0f)
    {
        if (owner.IsWithinLOSInMap(i_target.getTarget()))
        {
            if (!owner.movespline->Finalized())
                owner.StopMoving();
            return;
        }
    }

    _addUnitStateMove(owner);
    m_bTargetReached = false;

    init.Move(&path);

    float dist = path.Length();
    float speed = i_target->GetSpeedForMovementInfo(i_target->m_movementInfo);
    if (!speed)
        speed = owner.GetSpeedForMovementInfo(owner.m_movementInfo);

    init.SetWalk(false);
    init.SetVelocity(speed);
    if (dist > speed)
    {
        Unit* pOwner = owner.GetCharmerOrOwner();
        if (pOwner && (!pOwner->IsInCombat() && !owner.IsInCombat() || pOwner->IsPlayer() && pOwner->IsMounted()))
        {
            float distFactor = 1.0f + 0.04f * (dist - speed);
            if (distFactor < 1.0f) distFactor = 1.0f;
            if (distFactor > 2.1f) distFactor = 2.1f;
            init.SetVelocity(distFactor * speed);
        }
    }
    else if (dist < 2.0f)
        init.SetWalk(true);
    init.SetFacing(i_target->GetOrientation());
    init.Launch();
    m_checkDistanceTimer.Reset(500);
}

template<class T>
bool FollowMovementGenerator<T>::Update(T &owner, uint32 const&  time_diff)
{
    if (!i_target.isValid() || !i_target->IsInWorld())
    return false;

    if (!owner.IsAlive())
        return true;

    if (owner.HasUnitState(UNIT_STATE_CAN_NOT_MOVE | UNIT_STATE_POSSESSED))
    {
        _clearUnitStateMove(owner);
        return true;
    }

    // prevent movement while casting spells with cast time or channel time
    // don't stop creature movement for spells without interrupt movement flags
    if (owner.IsNoMovementSpellCasted())
    {
        if (!owner.IsStopped())
            owner.StopMoving();
        return true;
    }

    // prevent crash after creature killed pet
    if (_lostTarget(owner))
    {
        _clearUnitStateMove(owner);
        return true;
    }

    bool interrupted = false;
    m_checkDistanceTimer.Update(time_diff);
    if (m_checkDistanceTimer.Passed())
    {
        m_checkDistanceTimer.Reset(100);
        //More distance let have better performance, less distance let have more sensitive reaction at target move.
        if (!owner.movespline->Finalized() &&
                 ((!m_fOffset && i_target->IsWithinDist(&owner, 0.0f)) ||
                 (i_target->IsPlayer() && !i_target->IsMoving() &&
                 Geometry::GetDistance3D(owner.movespline->FinalDestination(), i_target->GetPosition()) > (m_fOffset + owner.GetObjectBoundingRadius() + i_target->GetObjectBoundingRadius() + 0.5f) &&
                 Geometry::GetDistance3D(owner.GetPosition(), i_target->GetPosition()) <= (m_fOffset + owner.GetObjectBoundingRadius() + i_target->GetObjectBoundingRadius() + 0.5f))))
        {
            owner.movespline->_Interrupt();
            interrupted = true;
        }
        else
        {
            bool targetMoved = false;
            G3D::Vector3 dest(m_fTargetLastX, m_fTargetLastY, m_fTargetLastZ);
            if (GenericTransport* ownerTransport = owner.GetTransport())
            {
                if (m_bTargetOnTransport)
                    ownerTransport->CalculatePassengerPosition(dest.x, dest.y, dest.z);
                else
                    targetMoved = true;
            }
            else if (m_bTargetOnTransport)
                targetMoved = true;

            if (!targetMoved)
                targetMoved = !i_target->IsWithinDist3d(dest.x, dest.y, dest.z, 0.1f);

            // Follow movement may be interrupted
            if (!targetMoved && owner.movespline->Finalized())
                targetMoved = this->IsFarEnoughToMoveStationaryFollower(owner);

            if (targetMoved)
            {
                m_bRecalculateTravel = true;
                owner.GetMotionMaster()->SetNeedAsyncUpdate();
            }
        }
    }

    if (owner.movespline->Finalized())
    {
        MovementInform(owner);

        if (m_fAngle == 0.f && !owner.HasInArc(i_target.getTarget(), 0.01f))
            owner.SetInFront(i_target.getTarget());

        if (!m_bTargetReached)
        {
            m_bTargetReached = true;
            _reachTarget(owner);
        }
        if (interrupted)
            owner.StopMoving(true);
    }
    else if (m_bRecalculateTravel)
        owner.GetMotionMaster()->SetNeedAsyncUpdate();

    return true;
}

template<>
bool FollowMovementGenerator<Creature>::EnableWalking() const
{
    return i_target.isValid() && i_target->IsWalking();
}

template<>
bool FollowMovementGenerator<Player>::EnableWalking() const
{
    return false;
}

template<>
void FollowMovementGenerator<Player>::_updateSpeed(Player &/*u*/)
{
    // nothing to do for Player
}

template<>
void FollowMovementGenerator<Creature>::_updateSpeed(Creature &u)
{
    if (!i_target.isValid() || i_target->GetObjectGuid() != u.GetOwnerGuid())
        return;

    u.UpdateSpeed(MOVE_RUN, false);
    u.UpdateSpeed(MOVE_WALK, false);
    u.UpdateSpeed(MOVE_SWIM, false);
}

template<>
void FollowMovementGenerator<Player>::Initialize(Player &owner)
{
    owner.AddUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    _updateSpeed(owner);
    _setTargetLocation(owner);
}

template<>
void FollowMovementGenerator<Creature>::Initialize(Creature &owner)
{
    owner.AddUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    _updateSpeed(owner);
    _setTargetLocation(owner);
}

template<class T>
void FollowMovementGenerator<T>::Finalize(T &owner)
{
    owner.ClearUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    _updateSpeed(owner);
    //MovementInform(owner);
}

template<class T>
void FollowMovementGenerator<T>::Interrupt(T &owner)
{
    owner.ClearUnitState(UNIT_STATE_FOLLOW | UNIT_STATE_FOLLOW_MOVE);
    _updateSpeed(owner);
}

template<class T>
void FollowMovementGenerator<T>::Reset(T &owner)
{
    Initialize(owner);
}

template<class T>
void FollowMovementGenerator<T>::MovementInform(T& /*unit*/)
{
}

template<>
void FollowMovementGenerator<Creature>::MovementInform(Creature& unit)
{
    if (!unit.IsAlive())
        return;

    // Pass back the GUIDLow of the target. If it is pet's owner then PetAI will handle
    if (unit.AI())
        unit.AI()->MovementInform(FOLLOW_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());

    if (unit.IsTemporarySummon())
    {
        TemporarySummon* pSummon = (TemporarySummon*)(&unit);
        if (pSummon->GetSummonerGuid().IsCreature())
        {
            if (Creature* pSummoner = unit.GetMap()->GetCreature(pSummon->GetSummonerGuid()))
                if (pSummoner->AI())
                    pSummoner->AI()->SummonedMovementInform(&unit, FOLLOW_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());
        }
        else
        {
            if (GameObject* pSummoner = unit.GetMap()->GetGameObject(pSummon->GetSummonerGuid()))
                if (pSummoner->AI())
                    pSummoner->AI()->SummonedMovementInform(&unit, FOLLOW_MOTION_TYPE, i_target.getTarget()->GetGUIDLow());
        }
    }
}

//-----------------------------------------------//
template void TargetedMovementGeneratorMedium<Player, ChaseMovementGenerator<Player> >::UpdateAsync(Player &, uint32);
template void TargetedMovementGeneratorMedium<Player, FollowMovementGenerator<Player> >::UpdateAsync(Player &, uint32);
template void TargetedMovementGeneratorMedium<Creature, ChaseMovementGenerator<Creature> >::UpdateAsync(Creature &, uint32);
template void TargetedMovementGeneratorMedium<Creature, FollowMovementGenerator<Creature> >::UpdateAsync(Creature &, uint32);
template bool TargetedMovementGeneratorMedium<Player, ChaseMovementGenerator<Player> >::IsFarEnoughToMoveStationaryFollower(Player &) const;
template bool TargetedMovementGeneratorMedium<Player, FollowMovementGenerator<Player> >::IsFarEnoughToMoveStationaryFollower(Player &) const;
template bool TargetedMovementGeneratorMedium<Creature, ChaseMovementGenerator<Creature> >::IsFarEnoughToMoveStationaryFollower(Creature &) const;
template bool TargetedMovementGeneratorMedium<Creature, FollowMovementGenerator<Creature> >::IsFarEnoughToMoveStationaryFollower(Creature &) const;

template void ChaseMovementGenerator<Player>::_setTargetLocation(Player &);
template void ChaseMovementGenerator<Creature>::_setTargetLocation(Creature &);
template bool ChaseMovementGenerator<Player>::Update(Player &, uint32 const&);
template bool ChaseMovementGenerator<Creature>::Update(Creature &, uint32 const&);
template void ChaseMovementGenerator<Player>::_reachTarget(Player &);
template void ChaseMovementGenerator<Creature>::_reachTarget(Creature &);
template void ChaseMovementGenerator<Player>::Finalize(Player &);
template void ChaseMovementGenerator<Creature>::Finalize(Creature &);
template void ChaseMovementGenerator<Player>::Interrupt(Player &);
template void ChaseMovementGenerator<Creature>::Interrupt(Creature &);
template void ChaseMovementGenerator<Player>::Reset(Player &);
template void ChaseMovementGenerator<Creature>::Reset(Creature &);
template void ChaseMovementGenerator<Player>::MovementInform(Player&);

template void FollowMovementGenerator<Player>::_setTargetLocation(Player &);
template void FollowMovementGenerator<Creature>::_setTargetLocation(Creature &);
template bool FollowMovementGenerator<Player>::Update(Player &, uint32 const&);
template bool FollowMovementGenerator<Creature>::Update(Creature &, uint32 const&);
template void FollowMovementGenerator<Player>::Finalize(Player &);
template void FollowMovementGenerator<Creature>::Finalize(Creature &);
template void FollowMovementGenerator<Player>::Interrupt(Player &);
template void FollowMovementGenerator<Creature>::Interrupt(Creature &);
template void FollowMovementGenerator<Player>::Reset(Player &);
template void FollowMovementGenerator<Creature>::Reset(Creature &);
template void FollowMovementGenerator<Player>::MovementInform(Player&);
