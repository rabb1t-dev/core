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

#include "PartyBotAI.h"
#include "ItemEvaluator.h"
#include "Player.h"
#include "Corpse.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "TargetedMovementGenerator.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "Database/SQLStorages.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "Packets/Loot.h"
#include "Utilities/Random.h"

#include <random>

enum PartyBotSpells
{
    PB_SPELL_FOOD = 1131,
    PB_SPELL_DRINK = 1137,
    PB_SPELL_AUTO_SHOT = 75,
    PB_SPELL_SHOOT_WAND = 5019,
    PB_SPELL_HONORLESS_TARGET = 2479,
    // The ranged attacks everyone else gets. Auto Shot above is the hunter's own and is granted with
    // the class; these come with the weapon skill, so which one applies is a question about what the
    // bot is holding rather than what class it is.
    PB_SPELL_SHOOT_BOW = 2480,
    PB_SPELL_SHOOT_GUN = 7918,
    PB_SPELL_SHOOT_CROSSBOW = 7919,
    PB_SPELL_THROW = 2764,
};

// How much nearer the destination a corpse run has to get before it counts as progressing.
static constexpr float PB_CORPSE_RUN_PROGRESS_STEP = 5.0f;
// How many no-progress deadlines' worth of walking a run is allowed in total before the bot is
// made to give up regardless of how busy it looks. The longest run in the game is well inside
// this, and the point is only to put an end to a ghost pacing a loop it cannot get out of.
static constexpr int PB_CORPSE_RUN_MAX_TIMEOUTS = 10;
// How many of those same deadlines a bot standing on its own corpse will hold for the leader to
// reach the same map before giving up and rising anyway. Shorter than the run allowance above so
// that a hold always ends by standing up at the corpse, never by the spirit healer collecting a
// bot that had already walked all the way back.
static constexpr int PB_LEADER_RETURN_TIMEOUTS = 5;

// How near a dungeon entrance the bot has to be before it is worth checking which area trigger
// it is standing in. Comfortably wider than the largest entrance box, which runs to about
// sixteen yards from its centre at Maraudon.
static constexpr float PB_PORTAL_SCAN_RANGE = 60.0f;
// Close enough to a way in that the remaining gap is the mesh falling short of the door rather
// than any distance worth pathing, so it is crossed in a straight line.
static constexpr float PB_PORTAL_STEP_IN_RANGE = 25.0f;

// How far from a map's ghost entrance an area trigger may sit and still be taken to be the
// doorway those coordinates are naming. The two describe the same spot, so this only has to
// absorb rounding.
static constexpr float PB_GHOST_ENTRANCE_MATCH = 10.0f;

// The share of the current target's threat at which a mob changes its mind about who to hit.
// ThreatContainer::selectNextVictim flips at 110 percent for someone the creature can reach
// with a melee swing and 130 percent for anyone else, so the pair below are facts about the
// server rather than a policy.
static constexpr float PB_THREAT_PULL_RATIO_MELEE = 1.10f;
static constexpr float PB_THREAT_PULL_RATIO_RANGED = 1.30f;
// How far below the flip to stop, which is overshoot rather than caution: the ceiling is tested
// before a cast and then crossed by the threat that cast makes, so whatever is already committed
// when the answer comes back has to fit underneath. Both numbers are measured. Melee overshoot
// about fifteen points of the tank's threat, because their abilities are instant and small.
// Casters overshoot about twice that, because a nuke is worth several of those and its damage
// over time keeps arriving for another fifteen seconds after the decision to stop.
//
// They are deliberately no wider than that. Room left over here is damage not done, and the aim
// is a raid that holds its target rather than one whose damage dealers are all idling at half
// the tank's threat.
static constexpr float PB_THREAT_HEADROOM_MELEE = 0.20f;
static constexpr float PB_THREAT_HEADROOM_RANGED = 0.30f;
// How long a damage dealer leaves the tank alone at the start of a fight. The ratio above cannot
// govern the opening, because at the moment of the pull the tank's threat is near zero and any
// share of near zero is a number a single spell steps straight over. Real raids solve this the
// same way, and this is the one part of the scheme that costs every bot rather than only the
// ones near their ceiling, so five seconds was tried. It is not enough here and the reason is
// worth keeping: a bot tank does not open like a player one, and at five seconds into a
// twenty-five bot pull it held 215 threat and was behind a rogue's auto-attacks, where at eight
// it held 966. Releasing onto a tank that low is worse for damage as well as for safety, since
// every ceiling is a share of it and the whole raid stalls at once waiting for it to catch up.
static constexpr time_t PB_THREAT_PULL_HOLD_SECONDS = 8;
// How much bigger than the bot the target has to be before any of that ramp applies. A raid
// target carries tens of times a player's health and an ordinary one carries less than its
// own, so this separates the fights worth ramping into from the ones the ramp would consume.
// Dungeon bosses sit near the line and may fall either side of it, which costs them the ramp
// rather than breaking them.
static constexpr float PB_THREAT_RAMP_HEALTH_RATIO = 5.0f;
// What share of the distance still left to the flip one cast may spend, once a damage dealer is
// held back and is choosing a lower rank to keep working with. It is not the whole distance
// because a cast is never the only thing in flight: damage over time laid down earlier keeps
// ticking, and at a one second decision interval two more casts may land before the next look.
// Half leaves room for those without leaving the caster idle, which is the entire point of
// downranking and is how a real caster opens a fight rather than watching the first ten seconds
// of it.
static constexpr float PB_THREAT_RANK_SHARE = 0.5f;
// Rage, in the tenths the field is stored in. Below the first a tank has too little to run its
// list at all and reaches for Bloodrage. The other two are floors under the abilities that cost
// rage without using the global cooldown, and they exist so that spending on those can never be
// what leaves Shield Slam short: Shield Slam is twenty, so anything taken outside the cooldown
// has to leave that behind it.
static constexpr uint32 PB_TANK_RAGE_LOW = 200;
static constexpr uint32 PB_TANK_RAGE_BLOCK = 300;
static constexpr uint32 PB_TANK_RAGE_DUMP = 600;

// How often a bot gets to think, which is the ceiling on everything it does.
//
// This was a second, against a global cooldown of one and a half, and the two do not divide: a bot
// acting at zero is refused at one and acts again at two, so every ability lands on a two second
// cycle and a rotation that should be uninterrupted gives up a quarter of itself to arithmetic. The
// cost is invisible in the cast log, which shows only casts that happened and nothing about the
// half second each one waited. Four ticks to the second divides the cooldown exactly and leaves the
// rotation to be limited by its own costs and cooldowns instead.
//
// Cheap enough at this rate: a tick is target selection and a walk down a list of spell checks, for
// a handful of bots, against a server that is idle between them.
#define PB_UPDATE_INTERVAL 250
// How often a bot writes a state line, held apart from the tick rate above so that speeding the
// rotation up does not multiply the log by the same factor.
static constexpr uint32 PB_TICK_LOG_INTERVAL_MS = 1000;

// Clearance demanded on top of a mob's own aggro radius before a bot will stand up inside it. A bot
// rises on half health with no buffs and cannot afford to be judged on the edge of the band, and a
// boss whose radius is measured against a level sixty raider is not generous to a level twenty bot.
static constexpr float PB_RISE_SAFETY_MARGIN = 8.0f;
// How close a ghost has to get to a chosen rise spot before standing up there. Only used when the
// spot is not the body: rising anywhere inside the reclaim radius is what walks a ghost the last
// thirty yards onto a corpse lying under whatever killed it.
static constexpr float PB_RISE_ARRIVE_DIST = 5.0f;
#define PB_MIN_FOLLOW_DIST 3.0f
#define PB_MAX_FOLLOW_DIST 6.0f
// Behind the leader, not anywhere around them. FollowMovementGenerator measures this angle from
// the leader's own facing, so zero is directly in front, and the full circle this used to draw
// from put half the group abreast of or ahead of whoever was steering. In a corridor that drags
// three or four aggro radii along the walls and pulls exactly what the leader was walking around.
// The spread is what keeps them from stacking on one spot, so it stays wide enough to fan out
// across the rear and no wider.
#define PB_FOLLOW_ANGLE_SPREAD 0.7f
#define PB_MIN_FOLLOW_ANGLE (M_PI_F - PB_FOLLOW_ANGLE_SPREAD)
#define PB_MAX_FOLLOW_ANGLE (M_PI_F + PB_FOLLOW_ANGLE_SPREAD)

// How hurt somebody has to be before a healer will leave formation to get within range of them.
// Chip damage is not worth walking for and a healer that chases every scratch is a healer out
// of position when something real happens, so this sits well below the threshold it heals at.
#define PB_HEAL_REPOSITION_PERCENT 70.0f
// Where it stands once it gets there. Comfortably inside every heal in the game, and chosen
// short because in a corridor the binding constraint is line of sight rather than range: the
// distance that fixes being unable to see somebody is closer than the distance that fixes
// being unable to reach them.
#define PB_HEAL_REPOSITION_DIST 10.0f

// What a healer needs before it spends mana on damage rather than saving it. Both conditions have
// to hold at once: a full bar while the tank sits at eighty percent means the next few seconds
// belong to healing, and a group at full health with a third of a bar left means the mana does.
static constexpr float PB_FILLER_MANA_PERCENT = 80.0f;
static constexpr float PB_FILLER_PARTY_HEALTH = 90.0f;
// When a filler cast already under way is worth throwing away. Smite is two and a half seconds and
// nothing else can be cast during it, so without this the group's healing waits on damage nobody
// asked for. Lower than the threshold above so the two do not argue over the same tick.
static constexpr float PB_FILLER_ABANDON_HEALTH = 80.0f;

// What a pack has to be carrying before damage-over-time spells are worth their cast time.
// Expressed in multiples of the caster's own health because that scales with level at no cost: a
// normal mob of the caster's level carries roughly a player's health, so this is a pack of about
// two of them. Below it the mobs die inside a couple of direct casts and a dot never finishes.
static constexpr float PB_DOT_WORTH_HEALTH_MULTIPLE = 2.0f;
// A pack this size outlasts its own dots whatever each mob is carrying, and each dot ticks on a
// separate target, so one cast is paid back several times over.
static constexpr uint32 PB_DOT_WORTH_ENEMY_COUNT = 3;
// A target already this far down dies to the next direct spell either way.
static constexpr float PB_DOT_WORTH_TARGET_HEALTH = 50.0f;
// How far out to look for the rest of the pack.
static constexpr float PB_DOT_PACK_RADIUS = 30.0f;

// How long to leave a corpse before deciding who it belongs to. Loot permission is not settled at
// the moment of death: the round robin assignment a bot holds is given up a tick later, and group
// rolls take seconds, so reading it immediately would call a corpse nobody's while it was still
// being decided.
static constexpr time_t PB_LOOT_GRACE_SECONDS = 6;
// How many corpses a bot will keep track of at once. A wipe-sized pull is well inside this.
static constexpr size_t PB_LOOT_QUEUE_LIMIT = 24;

// How close the pulled mob has to get before the party stops waiting and fights it. Generous on
// purpose: the point is to be sure the mob has committed to coming, and a held melee bot that breaks
// a little early still only walks the last few yards rather than the length of the room.
static constexpr float PB_PULL_ARRIVE_DIST = 12.0f;
// How long a hold waits for a mob that never arrives. A pull can fail in ways nothing here can see:
// the mob evades, roots itself on a ledge, gets killed by somebody else, or resets to its spawn. The
// party standing still forever afterwards would be a worse failure than the one being handled, so
// the hold expires and ordinary AI resumes.
static constexpr int PB_PULL_HOLD_TIMEOUT = 45;
// How long the puller itself keeps trying before giving the attempt up. Shorter than the hold above,
// so that a puller which cannot reach or cannot fire stops before the party does.
static constexpr int PB_PULL_SEQUENCE_TIMEOUT = 30;
// How long to let a shot that has been asked for stay pending before treating it as never coming.
// Two ranged swings at the slowest weapon in the game, so a shot genuinely on its way is never cut
// off, and a shot that is stuck no longer costs the whole sequence timeout in silence.
static constexpr int PB_PULL_SHOT_WAIT = 4;
// How near the anchor counts as being back with the group. Loose enough that pathing around the
// people already standing there does not leave the puller circling for a spot.
static constexpr float PB_PULL_ANCHOR_TOLERANCE = 4.0f;

// How far a bot backs off when it flees melee. Shared by the move and by the check that runs
// ahead of it, so the position tested is always the position taken.
static constexpr float PB_DISTANCING_RANGE = 15.0f;
// Seconds between lines while a bot is down. It is asked once a second for as long as the bot
// stays dead, which is precisely the stretch that is worth reading and far too often to log.
static constexpr time_t PB_DEATH_LOG_INTERVAL = 5;

bool PartyBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    if (!m_race && !m_class)
    {
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    return SpawnNewPlayer(sess, m_class, m_race, m_mapId, m_instanceId, m_x, m_y, m_z, m_o, sObjectAccessor.FindPlayer(m_cloneGuid));
}

void PartyBotAI::CloneFromPlayer(Player const* pPlayer)
{
    if (!pPlayer)
        return;

    if (pPlayer->GetLevel() != me->GetLevel())
    {
        me->GiveLevel(pPlayer->GetLevel());
        me->InitTalentForLevel();
        me->SetUInt32Value(PLAYER_XP, 0);
    }

    // Learn all of the target's spells.
    for (const auto& spell : pPlayer->GetSpellMap())
    {
        if (spell.second.disabled)
            continue;

        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;

        uint32 const firstRankId = sSpellMgr.GetFirstSpellInChain(spell.first);
        if (!me->HasSpell(spell.first))
            me->LearnSpell(spell.first, false, (firstRankId == spell.first && GetTalentSpellPos(firstRankId)));
    }

    me->GetHonorMgr().SetHighestRank(pPlayer->GetHonorMgr().GetHighestRank());
    me->GetHonorMgr().SetRank(pPlayer->GetHonorMgr().GetRank());

    // Unequip current gear
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        me->AutoUnequipItemFromSlot(i);

    // Copy gear from target.
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            me->SatisfyItemRequirements(pItem->GetProto());
            me->StoreNewItemInBestSlots(pItem->GetEntry(), 1, pItem->GetEnchantmentId(EnchantmentSlot(0)));
        }
    }
}

Player* PartyBotAI::GetPartyLeader() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    if (Player* originalLeader = ObjectAccessor::FindPlayerNotInWorld(m_leaderGuid))
    {
        if (me->InBattleGround() == originalLeader->InBattleGround())
        {
            // In case the original spawner is not in the same group as the bots anymore.
            if (pGroup != originalLeader->GetGroup())
                return nullptr;

            // In case the current leader is the bot itself and it's not inside a Battleground.
            ObjectGuid currentLeaderGuid = pGroup->GetLeaderGuid();
            if (currentLeaderGuid == me->GetObjectGuid() && !me->InBattleGround())
                return nullptr;
        }

        return originalLeader;
    }
    return nullptr;
}

bool PartyBotAI::IsValidDistancingTarget(Unit* pTarget, Unit* pEnemy)
{
    if (pTarget->IsInWorld() && pTarget->IsAlive() &&
        pTarget->GetMap() == me->GetMap())
    {
        float const distance = me->GetDistance(pTarget);
        if (distance >= 15.0f && distance <= 30.0f &&
            pTarget->GetDistance(pEnemy) >= 15.0f &&
            !WouldPositionPullExtraEnemies(pTarget->GetPositionX(), pTarget->GetPositionY(),
                                           pTarget->GetPositionZ()))
            return true;
    }

    return false;
}

Unit* PartyBotAI::GetDistancingTarget(Unit* pEnemy)
{
    if (Player* pLeader = GetPartyLeader())
        if (IsValidDistancingTarget(pLeader, pEnemy))
            return pLeader;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Unit* pNonTank = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == me)
                continue;

            if (IsValidDistancingTarget(pMember, pEnemy))
            {
                if (IsTankingForm(pMember->GetShapeshiftForm()) || IsWearingShield(pMember))
                    return pMember;
                else
                    pNonTank = pMember;
            }
        }
    }

    return pNonTank;
}

bool PartyBotAI::RunAwayFromTarget(Unit* pEnemy)
{
    if (Unit* pTarget = GetDistancingTarget(pEnemy))
    {
        me->MonsterMove(pTarget->GetPositionX(), pTarget->GetPositionY(), pTarget->GetPositionZ());
        return true;
    }

    // Backing straight away from whatever is hitting it is how a caster in a corridor walks into
    // the next pack. The direction is decided entirely by where the enemy happens to stand, and
    // nothing looked at what was behind. Test the spot MoveDistance would choose before
    // committing to it, computed the same way so the two cannot disagree, and stay put when it
    // would wake something: a few more hits in a fight the group is already having is a better
    // trade than starting a second one on top of it.
    float x, y, z;
    pEnemy->GetNearPoint(me, x, y, z, 0, PB_DISTANCING_RANGE, pEnemy->GetAngle(me));
    if (WouldPositionPullExtraEnemies(x, y, z))
        return false;

    return me->GetMotionMaster()->MoveDistance(pEnemy, PB_DISTANCING_RANGE);
}

// Stand here until the fight comes to us.
//
// Distinct from the pause behind .partybot pause, which stops the AI running at all: a paused bot
// does not heal, does not defend itself and does not notice it is being eaten, which is acceptable
// for parking a roster and not for waiting out a pull. Everything carries on here except the two
// things that would spoil the wait, closing on a target and following the leader, so a held healer
// still heals and a held caster still casts at whatever is already in range.
void PartyBotAI::BeginHold(float x, float y, float z, ObjectGuid pullTargetGuid)
{
    m_holdPosition = true;
    m_holdX = x;
    m_holdY = y;
    m_holdZ = z;
    m_holdSince = time(nullptr);
    m_pullTargetGuid = pullTargetGuid;

    // Stopped once, here, rather than re-issued every tick. Refusing to start new movement is what
    // keeps a held bot in place from now on, and repeatedly clearing the motion master would fight
    // whatever the combat AI is legitimately doing, knockbacks and fear included.
    if (!me->IsStopped())
        me->StopMoving();

    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();

    HoldPet(true);
}

void PartyBotAI::ReleaseHold()
{
    m_holdPosition = false;
    m_holdSince = 0;
    m_pullTargetGuid.Clear();

    HoldPet(false);

    // Left idle by the hold, and ordinary AI only issues a follow when it finds the bot standing
    // still, so it picks the group back up on its own from here.
}

// Holding the bot was never enough by itself, because a pet is a second body taking its own orders.
// The rotation goes on picking a target while the bot waits -- that is deliberate, so a held caster
// can still cast at what is already in range -- and the warlock and hunter branches hand that target
// straight to the pet, which then crosses the room the bot was told not to cross. The pack wakes and
// the group is in the fight it was waiting to avoid, having stood perfectly still throughout.
//
// Passive rather than merely recalled, since a pet left aggressive picks its own fights out of
// whatever wanders past. Following rather than staying, so it waits beside its owner instead of
// wherever it happened to be standing when the order came.
void PartyBotAI::HoldPet(bool hold)
{
    Pet* pPet = me->GetPet();
    if (!pPet || !pPet->GetCharmInfo())
        return;

    if (hold)
    {
        pPet->GetCharmInfo()->SetReactState(REACT_PASSIVE);
        pPet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
        pPet->GetCharmInfo()->SetIsCommandAttack(false);
        pPet->AttackStop();
        return;
    }

    // Back to defending itself and its owner, which is where a summoned pet starts. Aggressive is
    // not restored on purpose: nothing here set it, and it is the setting that makes a pet pull.
    pPet->GetCharmInfo()->SetReactState(REACT_DEFENSIVE);
    pPet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
}

bool PartyBotAI::ShouldBreakHold() const
{
    // Something is on us, so there is nothing left to protect by standing still.
    if (!me->GetAttackers().empty())
        return true;

    time_t const now = time(nullptr);
    if (m_holdSince && (now - m_holdSince) >= PB_PULL_HOLD_TIMEOUT)
        return true;

    // A hold asked for on its own waits to be told, and only the two conditions above cut it short.
    if (m_pullTargetGuid.IsEmpty())
        return false;

    Unit* pTarget = me->GetMap()->GetUnit(m_pullTargetGuid);

    // Whatever was being pulled is gone: killed on the way in, despawned, or reset to its spawn.
    // Waiting on it is waiting on nothing.
    if (!pTarget || !pTarget->IsAlive())
        return true;

    // Measured against the whole party rather than against this bot alone. A pull is over once the
    // mob reaches the people waiting for it, and the member it walks up to is rarely the one at the
    // back: a hunter standing thirty yards behind the tank is never approached, so on a per-bot test
    // its own hold never lifted and it stayed held for the entire fight. It still shot, because a
    // hold only suppresses movement, which is why this looked like a pet problem rather than a hold
    // one -- the pet is commanded only on ticks where the bot is free to move, so it stood there.
    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (pMember && pMember->IsAlive() && pMember->IsWithinDist(pTarget, PB_PULL_ARRIVE_DIST))
                return true;
        }

        return false;
    }

    return me->IsWithinDist(pTarget, PB_PULL_ARRIVE_DIST);
}

// Which ranged attack this bot can actually make, if any.
//
// The weapon decides in every case, a hunter's included. Auto Shot used to be answered off the
// spellbook alone, on the reasoning that a hunter always knows it -- which is true, and is exactly
// why this reported a ranged pull for a hunter standing there with an empty ranged slot. The command
// then announced "at range", CastSpell accepted the autorepeat and returned OK, and no shot was ever
// fired, because an autorepeat waits on the weapon timer and there was no weapon to time. Nothing
// failed anywhere an error could be seen: the puller walked into position, stopped, and stared at the
// mob until the sequence timed out thirty seconds later.
//
// Answering zero here is not a refusal to pull. It sends FirePullAttack down its melee path, so the
// bot walks up and hits the thing instead, which is worse than a shot and far better than nothing.
uint32 PartyBotAI::GetRangedAttackSpellId() const
{
    // Asked with nonbroken and useable set, so a weapon the bot cannot presently fire counts as no
    // weapon rather than as a shot that silently never happens.
    Item* pWeapon = me->GetWeaponForAttack(RANGED_ATTACK, true, true);
    if (!pWeapon)
        return 0;

    ItemPrototype const* pProto = pWeapon->GetProto();
    if (!pProto || pProto->Class != ITEM_CLASS_WEAPON)
        return 0;

    // Bows, guns and crossbows fire what is in the ammo slot, and with that slot empty the shot dies
    // at the same silent place a missing weapon did. Thrown weapons are their own ammo, so they are
    // not asked. AddHunterAmmo stocks this at spawn, but it gives up early when nothing is equipped,
    // so a bot that acquired its weapon later has the skill, the slot and no arrows.
    switch (pProto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_GUN:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
        {
            if (!me->GetUInt32Value(PLAYER_AMMO_ID))
                return 0;
            break;
        }
    }

    // Now that there is something to fire, a hunter's own shot is the one to use.
    if (me->HasSpell(PB_SPELL_AUTO_SHOT))
        return PB_SPELL_AUTO_SHOT;

    switch (pProto->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
            return PB_SPELL_SHOOT_BOW;
        case ITEM_SUBCLASS_WEAPON_GUN:
            return PB_SPELL_SHOOT_GUN;
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return PB_SPELL_SHOOT_CROSSBOW;
        case ITEM_SUBCLASS_WEAPON_THROWN:
            return PB_SPELL_THROW;
    }

    return 0;
}

// How close the puller needs to get. Short of the weapon's true maximum, since the mob has to still
// be in range when the shot actually leaves rather than when the approach was decided, and a target
// that steps a yard away mid-pull would otherwise put the puller back to walking.
float PartyBotAI::GetPullStandoffDistance() const
{
    uint32 const spellId = GetRangedAttackSpellId();
    if (!spellId)
        return 0.0f;

    SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(spellId);
    if (!pSpell)
        return 0.0f;

    SpellRangeEntry const* pRange = sSpellRangeStore.LookupEntry(pSpell->rangeIndex);
    if (!pRange)
        return 0.0f;

    // Just inside the weapon's reach rather than comfortably inside it. The margin exists because the
    // shot leaves on the weapon timer instead of when it is asked for, so a target drifting outwards
    // in between would put the puller back to walking, but it was five yards and that is five yards
    // of walking towards a pack on every pull that starts out of range. Two is enough for the drift
    // and keeps the puller as close to standing still and shooting as its weapon allows.
    float const maxRange = pRange->maxRange;
    return maxRange > 2.0f ? maxRange - 2.0f : maxRange;
}

// An instant shot to open a pull with, for the classes that have one.
//
// Auto Shot is an autorepeat, which makes asking for it a request rather than an act: the cast only
// starts the weapon timer, and the shot leaves later on conditions that are tested again at the
// moment it fires. When one of those tests fails there is nothing to see -- the shot is neither
// fired nor cancelled, so the puller stands holding a shot that will never leave, the mob is never
// aggroed, and the whole sequence times out in silence. Arcane Shot resolves on cast and has the
// mob in combat on the same tick, which is the property a pull actually needs.
SpellEntry const* PartyBotAI::GetInstantPullSpell() const
{
    if (m_spells.hunter.pArcaneShot && me->IsSpellReady(m_spells.hunter.pArcaneShot))
        return m_spells.hunter.pArcaneShot;

    return nullptr;
}

// Take the shot, if it can be taken from where the bot is standing.
bool PartyBotAI::FirePullAttack(Unit* pTarget)
{
    if (!me->IsWithinLOSInMap(pTarget))
        return false;

    uint32 const rangedSpellId = GetRangedAttackSpellId();
    if (!rangedSpellId)
    {
        // Nothing to shoot with, so the pull is made with a fist. Worth doing rather than refusing:
        // it still brings the mob back to a group that is standing still, which is the point, and it
        // is what a warrior without a gun would have to do anyway.
        if (!me->CanReachWithMeleeAutoAttack(pTarget))
            return false;

        me->SetFacingToObject(pTarget);
        return me->Attack(pTarget, true);
    }

    SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(rangedSpellId);
    if (!pSpell || !pSpell->IsTargetInRange(me, pTarget))
        return false;

    // Forced, and not conditional on IsStopped. These are two different questions that are usually
    // answered the same way and were being treated as one: IsStopped reads a unit state, while
    // whether a ranged attack may fire is decided by the movement flags, and only StopMoving clears
    // those. A spline that ends because the bot arrived clears the spline flag and the forward flag
    // and leaves the rest of the moving mask behind, and with no client to send a stop packet there
    // is nothing else that ever clears it. So a puller that had finished walking counted as stopped,
    // skipped the call, and kept a movement flag that made every shot it queued unfireable.
    //
    // That is the whole bug, and it explains why it looked so arbitrary: pulls where the shot went
    // off mid-walk worked, because cutting a spline short goes through StopMoving and clears the
    // flags, while every pull that fired from a standstill at the anchor failed. It is also why a
    // hold command fixed it by hand.
    me->StopMoving(true);
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();

    me->SetFacingToObject(pTarget);
    me->Attack(pTarget, false);

    // Cast directly rather than through DoCastSpell, which would refuse this outright.
    // GetThreatHeadroom gives no allowance at all against a mob that is not yet fighting
    // anybody, on the grounds that casting into one is the pull. That is the right answer for a
    // damage dealer opening too early and the wrong one here, where pulling is the instruction
    // given. Auto Shot already reached the weapon by this route for the same reason.
    //
    // The instant shot is preferred where there is one, so that the pull is an act with an
    // observable result rather than a queued intention. Auto Shot is still started underneath it,
    // because it is what keeps the damage coming once the mob is on its way.
    if (SpellEntry const* pInstant = GetInstantPullSpell())
    {
        if (pInstant->IsTargetInRange(me, pTarget) &&
            me->CastSpell(pTarget, pInstant->Id, false) == SPELL_CAST_OK)
        {
            me->CastSpell(pTarget, rangedSpellId, false);
            return true;
        }
    }

    return me->CastSpell(pTarget, rangedSpellId, false) == SPELL_CAST_OK;
}

bool PartyBotAI::BeginPull(Unit* pTarget, float anchorX, float anchorY, float anchorZ)
{
    if (!pTarget || !IsValidHostileTarget(pTarget))
        return false;

    // Already working on this one, so leave the sequence where it is. The command is a natural thing
    // to press again when nothing looks to be happening, and every press used to send the phase back
    // to the approach and fire afresh, so a puller that was standing still waiting for its shot to
    // land -- which is what waiting for a shot to land looks like -- was restarted for doing it.
    if (IsPulling() && m_pullTargetGuid == pTarget->GetObjectGuid())
        return true;

    m_pullTargetGuid = pTarget->GetObjectGuid();
    m_pullPhase = PULL_PHASE_APPROACH;
    m_pullSince = time(nullptr);
    m_pullShotSince = 0;
    m_holdX = anchorX;
    m_holdY = anchorY;
    m_holdZ = anchorZ;

    // Everything it was doing stops here. Half of the reports of this command not working were a
    // puller that was mid-cast, holding position from a previous pull, or already swinging at
    // something else, and none of that gives way on its own: the sequence would set off only once
    // whatever it was busy with had finished with it.
    m_holdPosition = false;
    m_isBuffing = false;

    me->InterruptNonMeleeSpells(true);
    me->AttackStop();

    if (!me->IsStopped())
        me->StopMoving();

    // Idle put back deliberately. Clearing with all set empties the whole stack, including the idle
    // generator at the bottom that everything else assumes is there, and UpdateMotion asserts on an
    // empty one: leaving it bare crashed the world server on the next tick of the bot that had just
    // been told to pull.
    me->GetMotionMaster()->Clear(false, true);
    me->GetMotionMaster()->MoveIdle();

    // On orders now, which suspends the aggro rule. Otherwise the generators refuse every step of the
    // approach: the mob being pulled is by definition one the group is not fighting yet, which is
    // precisely what that rule keeps away from.
    me->SetAttackOrders(m_pullTargetGuid);

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // The puller's own pet is held too, so that the shot is what pulls. Left to itself it charges
    // the moment its owner takes a target, which is a body pull into the middle of the pack by the
    // one bot that is supposed to be taking a single mob off the edge of it.
    HoldPet(true);

    LogPull(GetRangedAttackSpellId() ? "ordered to pull at range" : "ordered to pull in melee");
    return true;
}

void PartyBotAI::EndPull()
{
    m_pullPhase = PULL_PHASE_NONE;
    m_pullSince = 0;
    m_pullShotSince = 0;
    me->SetAttackOrders(ObjectGuid());
    me->SetCasterChaseDistance(0.0f);

    // Released unconditionally, including on the way into a hold, which re-holds it a moment later.
    // The alternative is a pet left passive for the rest of the instance every time a pull is
    // abandoned, and an abandoned pull is exactly when nobody is watching the pet.
    HoldPet(false);
}

// Called on the steps of a pull rather than every tick, so it stays readable while a pull is being
// watched. Worth having at all because the sequence used to report only the way it ended: a puller
// stuck part way through looked identical to a command that had never arrived, and telling those
// apart took a reading of the source rather than of the log.
void PartyBotAI::LogPull(char const* what) const
{
    if (!sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_COMBAT_LOG))
        return;

    Unit const* pTarget = me->GetMap()->GetUnit(m_pullTargetGuid);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[BotCombat] pull bot='%s' lvl=%u phase=%u %s (target='%s' dist=%.1f los=%u anchor=%.1f)",
             me->GetName(), me->GetLevel(), uint32(m_pullPhase), what,
             pTarget ? pTarget->GetName() : "gone",
             pTarget ? me->GetDistance(pTarget) : 0.0f,
             pTarget ? uint32(me->IsWithinLOSInMap(pTarget)) : 0,
             me->GetDistance2d(m_holdX, m_holdY));
}

// The bot in the group whose job it is to hold whatever we pull. Nullptr when there is not one,
// which covers both a group with no tank and the common case of the tank being a real player: in
// either of those the anchor the order was given from is the best guess available.
Player* PartyBotAI::GetGroupTank() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive() || pMember->GetMapId() != me->GetMapId())
            continue;

        PlayerBotEntry const* pEntry = pMember->GetSession() ? pMember->GetSession()->GetBot() : nullptr;
        if (PartyBotAI const* pAI = pEntry ? dynamic_cast<PartyBotAI const*>(pEntry->ai.get()) : nullptr)
            if (pAI->m_role == ROLE_TANK)
                return pMember;
    }

    return nullptr;
}

// Walk in, shoot, walk back. Returns true when the sequence has taken the tick for itself.
bool PartyBotAI::UpdatePullSequence()
{
    Unit* pTarget = me->GetMap()->GetUnit(m_pullTargetGuid);
    bool const expired = m_pullSince && (time(nullptr) - m_pullSince) >= PB_PULL_SEQUENCE_TIMEOUT;

    if (!pTarget || !pTarget->IsAlive() || !IsValidHostileTarget(pTarget) || expired)
    {
        if (sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_COMBAT_LOG))
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] pull bot='%s' gave up in phase %u (%s)",
                     me->GetName(), uint32(m_pullPhase), expired ? "timed out" : "target gone");

        EndPull();
        return false;
    }

    switch (m_pullPhase)
    {
        case PULL_PHASE_APPROACH:
        {
            // Come here first, then shoot from here. Firing on the way in is what this used to do,
            // and it made the command unpredictable: a puller that happened to have a view of the
            // mob from wherever it was standing shot from there, which could be anywhere, and the
            // walk home then started from a spot nobody had chosen.
            //
            // Where the order was given is also the one place known to have a view of the target,
            // since whoever gave it had the mob selected to do so.
            if (me->GetDistance2d(m_holdX, m_holdY) > PB_PULL_ANCHOR_TOLERANCE)
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                {
                    me->GetMotionMaster()->MovePoint(0, m_holdX, m_holdY, m_holdZ, MOVE_PATHFINDING);
                    LogPull("walking to where the order came from");
                }

                return true;
            }

            if (FirePullAttack(pTarget))
            {
                m_pullPhase = PULL_PHASE_FIRE;
                LogPull("took the shot");
                return true;
            }

            m_pullPhase = PULL_PHASE_CLOSE;
            LogPull("no shot from here, closing in");
            return true;
        }
        case PULL_PHASE_CLOSE:
        {
            if (FirePullAttack(pTarget))
            {
                m_pullPhase = PULL_PHASE_FIRE;
                LogPull("took the shot");
                return true;
            }

            // Not shootable from where the order was given, so either the view that prompted it is
            // not available at head height or the mob is past the weapon's reach from there. Closing
            // is all that is left, and the standoff is dropped to melee for it deliberately: keeping
            // the standoff is what had a puller stop dead thirty yards from a wall, never in sight
            // to shoot and never far enough out to walk, doing nothing at all as far as anyone
            // watching could tell. FirePullAttack above takes the shot the moment one exists, so the
            // walk ends early whenever it can and only reaches melee when no shot was ever possible.
            me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(pTarget, 1.0f, 0.0f);

            return true;
        }
        case PULL_PHASE_FIRE:
        {
            // Held still until the shot lands. A ranged attack fires on the weapon timer some way
            // after it is asked for, and moving cancels it, so turning for home on the tick the cast
            // began would produce a pull that never happened: the party waits, and the mob never
            // comes. Combat on the target is the acknowledgement that it did happen.
            //
            // The chase from the approach has to be taken away and not merely interrupted. StopMoving
            // ends the current spline and leaves the generator in place, so the chase re-issued
            // itself on its very next update and walked the puller back in behind our backs: one
            // Wailing Caverns capture has a hunter shoot from 35.9 yards and then arrive 5.6 yards
            // from the mob, twenty yards adrift of the anchor it had just been standing on. That is
            // the body pull this whole command exists to avoid, performed by the bot that was told
            // to shoot instead.
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
            {
                if (!me->IsStopped())
                    me->StopMoving();

                me->GetMotionMaster()->Clear(false, true);
                me->GetMotionMaster()->MoveIdle();
            }

            if (pTarget->IsInCombat())
            {
                m_pullPhase = PULL_PHASE_RETURN;
                LogPull("it bit, heading home");
                return true;
            }

            // Something is still in flight, so leave it be. Asking again here would restart the
            // very weapon timer the shot is waiting on, and a shot re-asked for every tick never
            // leaves at all.
            //
            // Bounded, because "in flight" and "never going to leave" look identical from here. A
            // queued autorepeat that fails its checks at firing time is neither fired nor cancelled,
            // so this branch held the sequence for its full timeout without logging a line: the
            // fourteen seconds of complete silence in the capture that found this bug. Waiting a
            // couple of weapon swings is generous for a shot that is genuinely coming, and anything
            // longer is a stall worth describing and abandoning.
            if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) || me->IsNonMeleeSpellCasted())
            {
                if (!m_pullShotSince)
                    m_pullShotSince = time(nullptr);

                if ((time(nullptr) - m_pullShotSince) < PB_PULL_SHOT_WAIT)
                    return true;

                // Everything the firing path tests, so the next one of these does not need a
                // debugging session to read. Movement is first because it is the one that silently
                // holds an autorepeat forever, and it is deliberately reported from both sources:
                // they disagree, and the disagreement was the bug.
                if (sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_COMBAT_LOG))
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] pull bot='%s' shot never left after %us: moving=%u "
                             "stopped=%u moveflags=0x%x rangedready=%u rangedtimer=%u dist=%.1f",
                             me->GetName(), uint32(PB_PULL_SHOT_WAIT), uint32(me->IsMoving() ? 1 : 0),
                             uint32(me->IsStopped() ? 1 : 0), me->GetUnitMovementFlags(),
                             uint32(me->IsAttackReady(RANGED_ATTACK) ? 1 : 0),
                             me->GetAttackTimer(RANGED_ATTACK), me->GetDistance(pTarget));
                }

                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
                m_pullShotSince = 0;
                m_pullPhase = PULL_PHASE_CLOSE;
                LogPull("shot never left, closing in");
                return true;
            }

            m_pullShotSince = 0;

            // Nothing in flight and nothing landed, so it was interrupted or never started. Ask
            // again -- and if it cannot be asked, the mob has moved out of reach or behind something
            // while we stood here. That became possible the moment standing still was enforced
            // rather than merely requested: a patrolling mob walks off and leaves the puller rooted,
            // shooting at nothing until the sequence times out.
            //
            // Closing, not approaching. Walking back to where the order came from would be walking
            // away from the mob that has just wandered off, and it is a spot already known not to
            // have a shot from it.
            if (!FirePullAttack(pTarget))
            {
                m_pullPhase = PULL_PHASE_CLOSE;
                LogPull("lost the shot, closing again");
            }

            return true;
        }
        case PULL_PHASE_RETURN:
        {
            // Home is where the tank is standing rather than where the order was given from. The
            // mob is following the puller, so the whole point of the walk back is to hand it to
            // whoever is meant to hold it, and stopping short at the commander's old spot drops it
            // wherever they happened to be standing at the time. Re-aimed as we walk, so a tank
            // that has shifted in the meantime is still where we end up.
            if (Player const* pTank = GetGroupTank())
            {
                if (pTank->GetDistance2d(m_holdX, m_holdY) > PB_PULL_ANCHOR_TOLERANCE)
                {
                    pTank->GetPosition(m_holdX, m_holdY, m_holdZ);

                    // Drop the walk to the old spot so the re-issue below picks up the new one. Idle
                    // goes back on immediately: clearing with all set empties the stack completely,
                    // and an empty stack fails an assertion in UpdateMotion on the next tick.
                    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                    {
                        me->GetMotionMaster()->Clear(false, true);
                        me->GetMotionMaster()->MoveIdle();
                    }
                }
            }

            if (me->GetDistance2d(m_holdX, m_holdY) <= PB_PULL_ANCHOR_TOLERANCE)
            {
                // Back with the group, and now waiting alongside it. Handing straight back to
                // ordinary AI would send the puller out again at the mob it just shot, which is the
                // behaviour this command exists to prevent, so it holds on the same terms as
                // everyone else and breaks when the mob arrives.
                ObjectGuid const pullTarget = m_pullTargetGuid;
                LogPull("home, holding with the group");
                EndPull();
                BeginHold(m_holdX, m_holdY, m_holdZ, pullTarget);
                return true;
            }

            // Auto Shot would keep the bot standing here firing, and the mob is meant to be
            // following it home rather than trading shots at range.
            if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                me->GetMotionMaster()->MovePoint(0, m_holdX, m_holdY, m_holdZ, MOVE_PATHFINDING);

            return true;
        }
        default:
            break;
    }

    return false;
}

bool PartyBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;

    if (me->GetVictim())
        return false;

    bool const needToEat = me->GetHealthPercent() < 100.0f;
    bool const needToDrink = (me->GetPowerType() == POWER_MANA) && (me->GetPowerPercent(POWER_MANA) < 100.0f);

    if (!needToEat && !needToDrink)
        return false;

    bool const isEating = me->HasAura(PB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(PB_SPELL_DRINK);

    if (!isEating && needToEat)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_FOOD))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(PB_SPELL_DRINK))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

// Whether a bot could stand up here without something immediately killing it again.
//
// The aggro rule the living bots move by, asked about one spot instead of a route. Margin on top of
// the mob's own radius because a bot rises with half its health and no buffs, so the edge of the
// band is not somewhere to cut fine.
bool PartyBotAI::IsPositionSafeToRise(float x, float y, float z) const
{
    return me->FindUnengagedCreatureAggroedByPosition(x, y, z, PB_RISE_SAFETY_MARGIN) == nullptr;
}

// Where to stand up, given that the body may be lying somewhere it cannot be stood up on.
//
// A corpse inside a boss's aggro radius is the case this exists for: rising on it aggroes the boss
// at half health, which kills the bot, which leaves a corpse in the same place, which it rises on
// again. The reclaim radius is much wider than any aggro radius, so there is nearly always
// somewhere within reach of the body that is out of reach of whatever killed it.
//
// False means there is nowhere, which is worth telling apart from success rather than papering over:
// the caller waits instead, since rising into the boss is not an improvement on staying down.
bool PartyBotAI::FindSafeRisePosition(Corpse* pCorpse, float& x, float& y, float& z) const
{
    pCorpse->GetPosition(x, y, z);

    // The body itself, which is the answer almost every time and costs one query to confirm.
    if (IsPositionSafeToRise(x, y, z))
        return true;

    float const corpseX = x;
    float const corpseY = y;
    float const corpseZ = z;

    // Outward, so the bot gives up as little ground as the danger allows and still has a short walk
    // to the body afterwards. Kept inside the reclaim radius throughout, because a spot the corpse
    // cannot be reclaimed from is not a spot to walk to.
    static constexpr float PB_RISE_SEARCH_RADII[] = { 15.0f, 22.0f, 28.0f, 34.0f };
    static constexpr int PB_RISE_SEARCH_ANGLES = 8;

    for (float const radius : PB_RISE_SEARCH_RADII)
    {
        for (int i = 0; i < PB_RISE_SEARCH_ANGLES; ++i)
        {
            float const angle = (2.0f * M_PI_F * float(i)) / float(PB_RISE_SEARCH_ANGLES);
            float testX = corpseX + radius * cos(angle);
            float testY = corpseY + radius * sin(angle);
            float testZ = corpseZ;

            // Asked of the map rather than assumed flat, since a spot hanging in the air or buried
            // in rock is no use even when nothing can reach it.
            me->UpdateAllowedPositionZ(testX, testY, testZ);

            if (!IsPositionSafeToRise(testX, testY, testZ))
                continue;

            // Line of sight to the body, which keeps the search on this side of the wall it is
            // circling. Without it the far side of a boss room scores as safe on distance alone.
            if (!pCorpse->IsWithinLOS(testX, testY, testZ + 2.0f))
                continue;

            x = testX;
            y = testY;
            z = testZ;
            return true;
        }
    }

    return false;
}

bool PartyBotAI::ShouldAutoRevive() const
{
    // Nothing is worth reviving into. This is the death loop in one line: a wipe at a boss leaves
    // every corpse inside its radius, the group revives on the spot the moment the boss resets,
    // and the boss pulls them again at half health. Falling through to the release below sends the
    // bot to the graveyard instead, which is somewhere safe by construction.
    if (!IsPositionSafeToRise(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ()))
        return false;

    // Deliberately no shortcut for the DEAD state here. A released ghost is mid-recovery,
    // and reviving it on the spot would undo the release and destroy its own corpse.
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    bool alivePlayerNearby = false;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (pMember == me)
                continue;

            if (pMember->IsInCombat())
                return false;

            if (pMember->IsAlive())
            {
                if (IsHealerClass(pMember->GetClass()))
                    return false;

                if (me->IsWithinDistInMap(pMember, 15.0f))
                    alivePlayerNearby = true;
            }
        }
    }

    return alivePlayerNearby;
}

bool PartyBotAI::IsGroupInCombat() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (pMember && pMember != me && pMember->IsAlive() && pMember->IsInCombat())
            return true;
    }

    return false;
}

// Someone left standing who could resurrect this corpse. Being in range says nothing about
// whether the resurrection will actually arrive, so callers have to be able to give up.
Player* PartyBotAI::FindGroupHealer() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (pMember && pMember != me && pMember->IsAlive() &&
            IsHealerClass(pMember->GetClass()) && pMember->IsWithinDistInMap(me, 40.0f))
            return pMember;
    }

    return nullptr;
}

// Where on this map to run to get back into the dungeon the corpse is lying in. False when
// there is no way in from here.
bool PartyBotAI::FindInstanceEntrance(uint32 instanceMapId, float& x, float& y, float& z) const
{
    // A portal that teleports straight in, which is how all but one dungeon is entered. The
    // trigger's own position is used rather than the map's ghost entrance coordinates, which
    // are a flat x and y saying nothing about the height of a cave mouth sunk into the ground.
    //
    // The nearest of them rather than the first, because a dungeon can have more than one way in
    // and they are not close together: Maraudon's two are three hundred yards apart at opposite
    // ends of a canyon. The container these come from is unordered, so taking the first match
    // picks between them by hash order, and half the time that is the far one.
    AreaTriggerEntry const* pNearest = nullptr;
    float bestDistance = 0.0f;
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* pTrigger = &itr.second;
        if (pTrigger->map_id != me->GetMapId())
            continue;

        AreaTriggerTeleport const* pTeleport = sObjectMgr.GetAreaTriggerTeleport(pTrigger->id);
        if (!pTeleport || pTeleport->destination.mapId != instanceMapId)
            continue;

        float const distance = me->GetDistance(pTrigger->x, pTrigger->y, pTrigger->z);
        if (pNearest && distance >= bestDistance)
            continue;

        pNearest = pTrigger;
        bestDistance = distance;
    }

    if (pNearest)
    {
        x = pNearest->x;
        y = pNearest->y;
        z = pNearest->z;
        return true;
    }

    // Blackwing Lair is entered by no such portal. Its way back in is a scripted trigger beside
    // the Orb of Command that answers only to the dead, and it appears in no teleport table, so
    // searching for one concludes the raid is unreachable and abandons the run before it starts.
    // The map knows better: a ghost entrance is precisely the spot to walk to from outside.
    MapEntry const* pMapEntry = sMapStorage.LookupEntry<MapEntry>(instanceMapId);
    if (!pMapEntry || pMapEntry->ghostEntranceMap < 0 ||
        uint32(pMapEntry->ghostEntranceMap) != me->GetMapId())
        return false;

    float const entranceX = pMapEntry->ghostEntranceX;
    float const entranceY = pMapEntry->ghostEntranceY;

    // Those two coordinates carry no height, and taking the ground beneath them is only right
    // where the way in is on top of the world. The Orb of Command is a hundred and forty yards
    // inside Blackrock Mountain, with a walkable summit above it, so a ghost sent to the terrain
    // height arrives at the correct spot on the map and nowhere near the trigger. What the
    // entrance is really naming is that trigger, and a trigger knows how high it is.
    AreaTriggerEntry const* pClosest = nullptr;
    float bestDistanceSq = PB_GHOST_ENTRANCE_MATCH * PB_GHOST_ENTRANCE_MATCH;
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* pTrigger = &itr.second;
        if (pTrigger->map_id != me->GetMapId())
            continue;

        float const dx = pTrigger->x - entranceX;
        float const dy = pTrigger->y - entranceY;
        float const distanceSq = dx * dx + dy * dy;
        if (distanceSq > bestDistanceSq)
            continue;

        bestDistanceSq = distanceSq;
        pClosest = pTrigger;
    }

    if (pClosest)
    {
        x = pClosest->x;
        y = pClosest->y;
        z = pClosest->z;
        return true;
    }

    x = entranceX;
    y = entranceY;
    z = me->GetMap()->GetHeight(x, y, MAX_HEIGHT);
    return true;
}

// Whether to keep lying there having arrived. A group that wiped should come back together, so a
// bot that has walked all the way to its corpse holds until the leader has reached the same map
// rather than rising the moment it can and trailing back out to a leader still running in.
//
// A ghost leader counts: what is being waited on is the group being in one place, and a leader
// picking their way back through the dungeon is exactly the moment to stand up beside them.
bool PartyBotAI::WaitForLeaderBeforeRising()
{
    Player* pLeader = GetPartyLeader();
    if (pLeader && pLeader->IsInWorld() && pLeader->GetMapId() == me->GetMapId())
    {
        m_leaderWaitSince = 0;
        return false;
    }

    time_t const now = time(nullptr);
    if (!m_leaderWaitSince)
        m_leaderWaitSince = now;

    uint32 const timeout = sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_DEATH_RECOVERY_TIMEOUT);
    if (timeout && (now - m_leaderWaitSince) >= time_t(timeout) * PB_LEADER_RETURN_TIMEOUTS)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBot] '%s' waited %us on its corpse on map %u for the leader to come back, "
                 "and is rising without them.",
                 me->GetName(), uint32(now - m_leaderWaitSince), me->GetMapId());
        m_leaderWaitSince = 0;
        return false;
    }

    // Reaching the corpse ended the run, so the run's own stall deadline must not collect a bot
    // that is now standing still on purpose. The hold above is what bounds this phase instead.
    m_ghostStart = now;
    LogDeathHold("standing on its corpse, waiting for the leader to get back to this map");
    return true;
}

// The run back from the graveyard. Returns whether the bot is getting anywhere, so the caller
// can hold the spirit healer off while it is and fall back to one when it is not.
//
// Nothing here needs a route described to it. The destination comes from the corpse or, for a
// death inside a dungeon, from the entrance portal, and the navigation mesh knows the terrain
// in between.
bool PartyBotAI::UpdateCorpseRun()
{
    Corpse* pCorpse = me->GetCorpse();
    if (!pCorpse)
        return false;

    float x, y, z;
    bool const corpseIsOnThisMap = pCorpse->GetMapId() == me->GetMapId();

    // Where to rise from, which is the body unless the body is somewhere that cannot be stood up
    // on. When it is, the walk aims at the safe spot instead and the bot rises when it gets there
    // rather than anywhere inside the reclaim radius: the shortcut below is what walks a ghost the
    // last thirty yards onto a corpse lying under a boss.
    bool riseAnywhereInRange = true;

    if (corpseIsOnThisMap)
    {
        if (!FindSafeRisePosition(pCorpse, x, y, z))
        {
            // Nowhere within reach of the body is out of reach of what is standing over it. Rising
            // is a death, so the run reports itself as still going and the deadline collects it into
            // a spirit healer resurrection at the graveyard, which is the one place that is safe.
            LogDeathHold("its body is inside something's reach and there is nowhere safe to rise");
            return true;
        }

        // Whether the spot chosen is the body. When it is, nothing has changed and the shortcut
        // stands; when it is not, the point of it is where the bot stands, so it has to arrive.
        riseAnywhereInRange = pCorpse->GetDistance2d(x, y) < 1.0f;
    }
    else if (!FindInstanceEntrance(pCorpse->GetMapId(), x, y, z))
    {
        // A corpse left inside an instance cannot be walked to, and without a way back in
        // there is nothing this run can achieve.
        return false;
    }

    float const distance = me->GetDistance(x, y, z);

    if (corpseIsOnThisMap &&
        distance <= (riseAnywhereInRange ? CORPSE_RECLAIM_RADIUS : PB_RISE_ARRIVE_DIST))
    {
        // Arriving is not the end of it. The reclaim delay escalates to two minutes across
        // repeated deaths, and waiting it out is progress rather than a stall.
        if (time(nullptr) < pCorpse->GetGhostTime() + time_t(me->GetCorpseReclaimDelay(pCorpse->GetType() == CORPSE_RESURRECTABLE_PVP)))
            return true;

        if (WaitForLeaderBeforeRising())
            return true;

        me->ResurrectPlayer(0.5f);
        me->SpawnCorpseBones();

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBot] '%s' finished its corpse run and rose at its body on map %u after %us.",
                 me->GetName(), me->GetMapId(), uint32(time(nullptr) - m_ghostStart));

        // Deliberately not reported as progress. If that did not take, standing on the body
        // repeating it forever is a stall like any other, and the deadline should collect it.
        return false;
    }

    // Whichever trigger the bot is standing in, rather than one picked in advance, because the
    // way into Blackwing Lair is a scripted trigger that no search for a portal would have
    // found. This is also what a client does: it reports what it walked into and lets the
    // server decide what that means.
    //
    // The proximity test only decides when to look, not whether the bot has arrived, since
    // arriving is a question of containment: half the dungeon portals are box shaped and carry
    // a radius of zero. It is a wide margin against the largest of those boxes, and exists so
    // that a run measured in thousands of yards does not scan every trigger in the world on
    // every tick of it.
    if (!corpseIsOnThisMap && distance < PB_PORTAL_SCAN_RANGE)
        ActivateNearbyAreaTrigger();

    // One pathfinding query cannot span a corpse run. Paths are capped at 256 polygons, a
    // limit sized for a creature chasing someone rather than a cross-zone journey, and Detour
    // answers with its best partial route instead of failing. Reissuing each time the
    // previous leg runs out therefore walks the real path in chunks, which is what keeps the
    // bot going around the mountain rather than into it.
    //
    // Steep ground is not excluded the way it is when a bot is alive. Several dungeon mouths
    // sit at the bottom of a drop, and refusing the descent leaves the ghost pacing the rim
    // above its own corpse. Falling costs a ghost nothing.
    //
    // The last few yards are walked in a straight line instead. Pathfinding stops where the mesh
    // does and an instance portal regularly sits a little past that, so the ghost ends up beside
    // the door: close enough to see it, too far for the five yard tolerance the trigger handler
    // applies, and hopping between the last two reachable points until chance drops it inside.
    // Maraudon spent a full minute doing exactly that. Walking the remainder directly is what a
    // player does, and a short line through scenery costs a ghost nothing.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
    {
        // Measured flat, because the gap that needs crossing here is usually the vertical one.
        // A cave mouth is sunk into the ground and the mesh route ends on the lip above it, so
        // the ghost stands within a few paces of its door and forty-eight yards over the top of
        // it, which is precisely where Maraudon kept stranding them. Dropping in is the way in.
        bool const atTheDoor = !corpseIsOnThisMap && me->GetDistance2d(x, y) < PB_PORTAL_STEP_IN_RANGE;
        me->GetMotionMaster()->MovePoint(0, x, y, z, atTheDoor ? MOVE_NONE : MOVE_PATHFINDING);
    }

    // Progress is measured rather than assumed, because a ghost with nowhere to path still
    // looks busy: its movement generator finishes immediately and gets reissued forever.
    //
    // What counts is ground covered, not distance remaining. Distance remaining calls a stall
    // on every route that has to go the long way around, and dungeon mouths are full of them:
    // a ghost spiralling through Blackrock Mountain or working down into the Maraudon canyon
    // walks perfectly well for minutes at a time while the straight line to where it is headed
    // gets no shorter. A ghost that is actually stuck does something quite different, which is
    // stand still.
    if (m_corpseRunBestDistance < 0.0f)
    {
        m_corpseRunBestDistance = distance;
        me->GetPosition(m_corpseRunLastX, m_corpseRunLastY, m_corpseRunLastZ);
        return true;
    }

    // Only advanced on progress, so short steps accumulate across ticks instead of each one
    // being judged on its own and found wanting.
    if (me->GetDistance(m_corpseRunLastX, m_corpseRunLastY, m_corpseRunLastZ) < PB_CORPSE_RUN_PROGRESS_STEP)
        return false;

    me->GetPosition(m_corpseRunLastX, m_corpseRunLastY, m_corpseRunLastZ);
    m_corpseRunBestDistance = std::min(m_corpseRunBestDistance, distance);
    return true;
}

// Why a bot that is down is still down. The recovery path records the moment it releases and
// the moment it stands back up, and says nothing whatever about the stretch in between, which
// is the only part anyone ever complains about. Every hold below is deliberate and every one of
// them looks identical from the outside: a corpse lying there not releasing.
void PartyBotAI::LogDeathHold(char const* reason)
{
    if (!sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_COMBAT_LOG))
        return;

    time_t const now = time(nullptr);
    if (m_lastDeathLog && (now - m_lastDeathLog) < PB_DEATH_LOG_INTERVAL)
        return;

    m_lastDeathLog = now;

    Player* pLeader = GetPartyLeader();
    Player* pHealer = FindGroupHealer();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBot] '%s' still down on map %u: %s. state=%s ressreq=%u groupcombat=%u "
             "healer='%s' leadermap=%d leaderalive=%u deadfor=%us",
             me->GetName(), me->GetMapId(), reason,
             me->GetDeathState() == CORPSE ? "corpse" : "ghost",
             uint32(me->IsRessurectRequested() ? 1 : 0),
             uint32(IsGroupInCombat() ? 1 : 0),
             pHealer ? pHealer->GetName() : "none",
             pLeader ? int32(pLeader->GetMapId()) : -1,
             uint32(pLeader && pLeader->IsAlive() ? 1 : 0),
             uint32(m_corpseSince ? now - m_corpseSince : 0));
}

// Recovery after death. A resurrection is always preferred, but nothing here may depend on
// one arriving: after a wipe there is nobody left to cast it, which is exactly the case that
// used to leave the whole group on the floor permanently.
void PartyBotAI::UpdateDeadAI()
{
    // Battlegrounds run their own graveyard cycle and already worked.
    if (me->InBattleGround())
    {
        if (me->GetDeathState() == CORPSE)
        {
            me->BuildPlayerRepop();
            me->RepopAtGraveyard();
        }
        return;
    }

    time_t const now = time(nullptr);
    uint32 const timeout = sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_DEATH_RECOVERY_TIMEOUT);

    if (me->GetDeathState() == CORPSE)
    {
        // An offer is already in flight and bots accept immediately, so never release out
        // from under one.
        if (me->IsRessurectRequested())
        {
            LogDeathHold("a resurrection has been offered and is about to be accepted");
            return;
        }

        // While the group is still fighting there is nothing to break out of, and releasing
        // would throw away both the battle res and the free one after the kill. Combat ends
        // one way or another, so wait it out and start the clock from there.
        if (IsGroupInCombat())
        {
            m_corpseSince = 0;

            // A soulstone and an Ankh are for this exact moment and no other. Both are held
            // against a death during the pull, and spending one after the fight is over buys
            // nothing that walking back would not, at a cooldown of half an hour or more.
            UseSelfResurrection();
            LogDeathHold("somebody in the group is still in combat, so the clock has not started");
            return;
        }

        if (!m_corpseSince)
            m_corpseSince = now;

        // Once the fight is over, patience is budgeted, because a healer who survived the
        // wipe but is out of mana would otherwise keep this bot down for good.
        if (Player* pHealer = FindGroupHealer())
        {
            // Resurrection is a ten second cast, so a healer working down a pile of corpses
            // is making progress well before reaching this one. Only start the clock once
            // they stop casting altogether.
            if (pHealer->IsNonMeleeSpellCasted(false))
                m_corpseSince = now;

            if (!timeout || (now - m_corpseSince) < time_t(timeout))
            {
                LogDeathHold("a healer is alive, so holding for a resurrection rather than releasing");
                return;
            }
        }

        if (sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_AUTO_REVIVE) && ShouldAutoRevive())
        {
            me->ResurrectPlayer(0.5f);
            me->SpawnCorpseBones();
            me->CastSpell(me, PB_SPELL_HONORLESS_TARGET, true);

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[PartyBot] '%s' revived on the spot on map %u, no corpse run needed.",
                     me->GetName(), me->GetMapId());
            return;
        }

        // Last call for a stone held through a wipe where nobody survived to spend it on. It is
        // reached only after the healer window above has expired, so a living healer is still
        // preferred: their resurrection costs mana that regenerates, and this one does not
        // come back for half an hour.
        if (UseSelfResurrection())
            return;

        // Nothing is coming, so release rather than lying here. The engine stopped
        // auto-releasing inside instances in 1.11, so this has to be explicit.
        me->BuildPlayerRepop();
        me->ScheduleRepopAtGraveyard();
        m_ghostSince = now;
        m_ghostStart = now;
        m_leaderWaitSince = 0;
        m_corpseRunBestDistance = -1.0f;

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBot] '%s' released after %us dead on map %u and is running back.",
                 me->GetName(), uint32(now - m_corpseSince), me->GetMapId());
        return;
    }

    // Released, and on the way back to the body.
    if (!m_ghostSince)
    {
        m_ghostSince = now;
        m_ghostStart = now;
    }

    // A run that is getting somewhere keeps the deadline at bay, but not indefinitely, since
    // ground can be covered in a circle as easily as in a line.
    bool const walkedLongEnough = timeout &&
        (now - m_ghostStart) >= time_t(timeout) * PB_CORPSE_RUN_MAX_TIMEOUTS;

    if (UpdateCorpseRun() && !walkedLongEnough)
        m_ghostSince = now;
    else if (timeout && (now - m_ghostSince) >= time_t(timeout))
    {
        // Three quite different things end up here and they are indistinguishable from
        // outside: no corpse to run to at all, a corpse somewhere with no way back in, and a
        // route the bot could not walk. Say which, or every one of these costs an afternoon.
        Corpse* pCorpse = me->GetCorpse();
        float x = 0.0f, y = 0.0f, z = 0.0f;
        char const* target = "no corpse";
        if (pCorpse && pCorpse->GetMapId() == me->GetMapId())
        {
            pCorpse->GetPosition(x, y, z);
            target = "its corpse";
        }
        else if (pCorpse)
            target = FindInstanceEntrance(pCorpse->GetMapId(), x, y, z) ? "the way in"
                                                                        : "no way in";

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[PartyBot] '%s' gave up its corpse run after %us standing at %.0f %.0f %.0f "
                 "on map %u, heading for %s at %.0f %.0f %.0f, %.0f yards off.",
                 me->GetName(), uint32(now - m_ghostStart), me->GetPositionX(),
                 me->GetPositionY(), me->GetPositionZ(), me->GetMapId(), target, x, y, z,
                 me->GetDistance(x, y, z));

        // Take the spirit healer's terms. Plenty of ways to die leave a corpse that cannot
        // be reached at all, and one bot stuck on the way back would otherwise hold up
        // everyone else indefinitely.
        me->GetSession()->SendSpiritResurrect();
    }
}

bool PartyBotAI::IsInOpeningRamp(Unit const* pTarget) const
{
    Creature const* pCreature = pTarget->ToCreature();
    if (!pCreature)
        return false;

    // Only against something big enough to be worth the wait. Eight seconds is discipline in a
    // boss fight and most of the fight against a trash mob, and the health bar separates the two
    // without needing to know which encounter this is.
    if (pTarget->GetMaxHealth() < me->GetMaxHealth() * PB_THREAT_RAMP_HEALTH_RATIO)
        return false;

    return !pCreature->IsInCombat() ||
            pCreature->GetCombatTime(false) < PB_THREAT_PULL_HOLD_SECONDS;
}

void PartyBotAI::HoldOpeningSwings(Unit const* pTarget)
{
    if (m_role == ROLE_TANK || IsInDuel() || !IsInOpeningRamp(pTarget))
        return;

    // Only spellcasts pass through CanTryToCastSpell, so the opening hold was silence for a
    // caster and nothing whatever for a rogue. At full raid size that was the whole of what was
    // left wrong with the opening: melee swinging through the hold finished it level with the
    // tank, having cast nothing and so having passed no gate.
    //
    // Pushing the swing timer out is the least invasive way to stop it. The bot goes on
    // attacking, chasing and running its rotation, and everything that reads what it is fighting
    // still reads the same answer; the swings simply land after the tank has its lead, which is
    // what a melee damage dealer in a real raid is doing while it waits.
    Creature const* pCreature = pTarget->ToCreature();
    time_t const elapsed = pCreature->IsInCombat() ? pCreature->GetCombatTime(false) : 0;
    if (elapsed >= PB_THREAT_PULL_HOLD_SECONDS)
        return;

    uint32 const remaining = uint32(PB_THREAT_PULL_HOLD_SECONDS - elapsed) * IN_MILLISECONDS;

    for (uint8 att = BASE_ATTACK; att < MAX_ATTACK; ++att)
    {
        WeaponAttackType const type = WeaponAttackType(att);
        if (me->GetAttackTimer(type) < remaining)
            me->SetAttackTimer(type, remaining);
    }
}

bool PartyBotAI::IsOverThreatCeiling(Unit const* pTarget) const
{
    // The tank is the one meant to be at the top of the list, and a group with nobody tanking
    // has no ceiling to speak of: whoever is being hit is holding it by default.
    if (m_role == ROLE_TANK || IsInDuel() || !pTarget->CanHaveThreatList())
        return false;

    // Leave the opening to whoever is tanking it. The ratio below cannot govern the first
    // seconds of a fight, because it is a share of the tank's threat and the tank has almost
    // none yet, so a single nuke steps over any ceiling drawn from it; and casting into a mob
    // that is not yet fighting is not merely early but is itself the pull. Both are answered
    // the way a raid answers them, by not starting for a few seconds. This is deliberately
    // ahead of every question about who currently holds the mob, including whether it is on
    // this bot: a stray opening pull is taken back by the tank soonest if the bot it landed
    // on stops adding to it.
    //
    // Only against something big enough to be worth the wait. Eight seconds is discipline in
    // a boss fight and most of the fight against a trash mob, and the health bar separates
    // the two without needing to know which encounter this is.
    if (IsInOpeningRamp(pTarget))
        return true;

    // Read-only, but neither the threat lookup nor the container beneath it is marked const.
    ThreatManager& threat = const_cast<Unit*>(pTarget)->GetThreatManager();

    HostileReference const* pTop = threat.getCurrentVictim();
    if (!pTop || pTop->getTarget() == me)
        return false;

    // Only defer to someone the group is actually relying on. Deferring to a pet, or to a
    // second mob that has wandered into the fight, would have damage dealers throttling
    // themselves against a threat pool that nobody is trying to hold.
    Player const* pHolder = pTop->getTarget() ? pTop->getTarget()->ToPlayer() : nullptr;
    if (!pHolder || !me->IsInSameGroupWith(pHolder))
        return false;

    float const topThreat = pTop->getThreat();
    if (topThreat <= 0.0f)
        return false;

    float const ceiling = GetThreatPullRatio(pTarget) - (m_role == ROLE_MELEE_DPS
        ? PB_THREAT_HEADROOM_MELEE
        : PB_THREAT_HEADROOM_RANGED);

    return threat.getThreat(me) >= (topThreat * ceiling);
}

float PartyBotAI::GetThreatPullRatio(Unit const* pTarget) const
{
    // Which of the two flips applies is a question about where this bot is standing, not about
    // what it does for the group: ThreatContainer::selectNextVictim asks whether the creature
    // can reach the candidate with a melee swing and uses 110 percent if it can. A caster parked
    // inside a boss's reach is on the melee rule with a caster's threat, which is the worst
    // combination available, and reading the role instead of the geometry granted it a fifth
    // more threat than it actually had. Two mages took a boss off the tank at 124 percent that
    // way, below the 130 they were being measured against and above the 110 that governed them.
    return pTarget->CanReachWithMeleeAutoAttack(me)
        ? PB_THREAT_PULL_RATIO_MELEE
        : PB_THREAT_PULL_RATIO_RANGED;
}

float PartyBotAI::GetThreatHeadroom(Unit const* pTarget) const
{
    // Casting into something that is not fighting anyone yet is not an early cast, it is the
    // pull, and no rank is small enough to make that acceptable.
    Creature const* pCreature = pTarget->ToCreature();
    if (!pCreature || !pCreature->IsInCombat())
        return 0.0f;

    ThreatManager& threat = const_cast<Unit*>(pTarget)->GetThreatManager();
    HostileReference const* pTop = threat.getCurrentVictim();
    if (!pTop || !pTop->getTarget())
        return 0.0f;

    // Already holding it. Whatever is added here is threat the tank has to climb over to take
    // the target back, so the answer is none of it.
    if (pTop->getTarget() == me)
        return 0.0f;

    Player const* pHolder = pTop->getTarget()->ToPlayer();
    if (!pHolder || !me->IsInSameGroupWith(pHolder))
        return 0.0f;

    float const room = pTop->getThreat() * GetThreatPullRatio(pTarget) - threat.getThreat(me);
    return room > 0.0f ? room * PB_THREAT_RANK_SHARE : 0.0f;
}

float PartyBotAI::EstimateSpellThreat(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    // Threat for a damage spell is the damage: SpellEffects hands the number it just dealt
    // straight to AddThreat. So the question of how much threat a rank is worth is the question
    // of how hard it hits, which the caster can answer about itself before casting anything.
    float const base = me->CalculateSpellEffectValue(pTarget, pSpellEntry, EFFECT_INDEX_0);
    return me->SpellDamageBonusDone(pTarget, pSpellEntry, EFFECT_INDEX_0, base,
                                    SPELL_DIRECT_DAMAGE);
}

SpellEntry const* PartyBotAI::PickRankForThreat(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!pTarget || !pSpellEntry || pSpellEntry->IsPositiveSpell())
        return pSpellEntry;

    if (!IsOverThreatCeiling(pTarget))
        return pSpellEntry;

    // Held back, so the question changes from whether to cast to what to cast. Only a direct
    // nuke can answer it: a lower rank of one is the same spell for less of everything, where a
    // lower rank of a damage over time effect occupies the same slot on the target for the same
    // duration and would lock the good version out, and melee abilities barely differ by rank.
    if (pSpellEntry->Effect[EFFECT_INDEX_0] != SPELL_EFFECT_SCHOOL_DAMAGE)
        return nullptr;

    float const budget = GetThreatHeadroom(pTarget);
    if (budget <= 0.0f)
        return nullptr;

    for (SpellEntry const* pRank = pSpellEntry; pRank;)
    {
        if (EstimateSpellThreat(pTarget, pRank) <= budget)
            return pRank;

        uint32 const prev = sSpellMgr.GetPrevSpellInChain(pRank->Id);
        pRank = prev ? sSpellMgr.GetSpellEntry(prev) : nullptr;
    }

    // Even the first rank is too big for the room available, which is the ordinary state of
    // affairs in the first second of a pull.
    return nullptr;
}

SpellCastResult PartyBotAI::DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry)
{
    SpellEntry const* pRank = PickRankForThreat(pTarget, pSpellEntry);
    if (!pRank)
        return SPELL_FAILED_DONT_REPORT;

    return CombatBotBaseAI::DoCastSpell(pTarget, pRank);
}

bool PartyBotAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!CombatBotBaseAI::CanTryToCastSpell(pTarget, pSpellEntry))
        return false;

    // Hold below the threshold at which this target would turn round. Nothing in the bot code
    // has ever done this, so a damage dealer simply cast until it took the boss off the tank,
    // which in a raid loses the attempt outright and does so for a reason that has nothing to
    // do with whichever encounter is being tested. Only damage is throttled: refusing to heal
    // because healing makes threat would trade one lost raid for another.
    //
    // Being over the ceiling is not by itself a refusal, because a smaller version of the same
    // spell may still fit underneath it. PickRankForThreat answers both questions at once and
    // gives back nothing only when no rank fits; DoCastSpell asks it again to find out which.
    if (!pSpellEntry->IsPositiveSpell() && pTarget && !PickRankForThreat(pTarget, pSpellEntry))
        return false;

    if (pSpellEntry->IsAreaOfEffectSpell() && !pSpellEntry->IsPositiveSpell() && !IsInDuel())
    {
        if (!m_marksToCC.empty())
            return false;

        // do not cast aoe if it will pull aggro
        if (m_role != ROLE_TANK)
        {
            float radius;
            if (pSpellEntry->EffectRadiusIndex[0])
                radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[0]));
            else if (pSpellEntry->EffectRadiusIndex[1])
                radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[1]));
            else if (pSpellEntry->EffectRadiusIndex[2])
                radius = Spells::GetSpellRadius(sSpellRadiusStore.LookupEntry(pSpellEntry->EffectRadiusIndex[2]));
            else
                radius = 10.0f;

            std::list<Unit*> targets;
            me->GetEnemyListInRadiusAround(pTarget, radius, targets);

            for (auto const& pEnemy : targets)
            {
                if (((pEnemy->GetLevel() + 5) > me->GetLevel()) &&
                    ((pEnemy->GetHealth() * 4) > me->GetHealth()) &&
                    pEnemy->GetVictim() && pEnemy->GetVictim() != me &&
                    pEnemy->IsValidAttackTarget(me) &&
                    pEnemy->CanHaveThreatList())
                {
                    float const myThreat = pEnemy->GetThreatManager().getThreat(me);
                    float const victimThreat = pEnemy->GetThreatManager().getThreat(pEnemy->GetVictim());

                    if (victimThreat < (myThreat + me->GetMaxHealth()))
                        return false;
                }
            }
        }
    }

    return true;
}

bool PartyBotAI::CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const
{
    if (IsInDuel())
        return true;

    if (pSpellEntry->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) &&
        AreOthersOnSameTarget(pTarget->GetObjectGuid()))
        return false;

    if (pSpellEntry->HasSingleTargetAura())
    {
        auto const& singleAuras = me->GetSingleCastSpellTargets();
        if (singleAuras.find(pSpellEntry) != singleAuras.end())
            return false;
    }

    return true;
}

bool PartyBotAI::AttackStart(Unit* pVictim)
{
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    if (me->Attack(pVictim, true))
    {
        BeginChasing(pVictim);
        return true;
    }

    return false;
}

Unit* PartyBotAI::GetMarkedTarget(RaidTargetIcon mark) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    ObjectGuid targetGuid = pGroup->GetTargetWithIcon(mark);
    if (targetGuid.IsUnit())
        return me->GetMap()->GetUnit(targetGuid);

    return nullptr;
}

Unit* PartyBotAI::SelectAttackTarget(Player* pLeader) const
{
    if (IsInDuel())
    {
        if (me->m_duel->opponent && IsValidHostileTarget(me->m_duel->opponent))
            return me->m_duel->opponent;
    }
    else
    {
        // Stick to marked target in combat.
        if (me->IsInCombat() || pLeader->GetVictim())
        {
            if (Group* pGroup = me->GetGroup())
            {
                for (auto markId : m_marksToFocus)
                {
                    ObjectGuid targetGuid = pGroup->GetTargetWithIcon(markId);
                    if (targetGuid.IsUnit())
                        if (Unit* pVictim = me->GetMap()->GetUnit(targetGuid))
                            if (IsValidHostileTarget(pVictim))
                                return pVictim;
                }
            }
        }

        // Who is the leader attacking.
        if (Unit* pVictim = pLeader->GetVictim())
        {
            if (IsValidHostileTarget(pVictim))
                return pVictim;
        }
    }

    // Who is attacking me.
    for (const auto pAttacker : me->GetAttackers())
    {
        if (IsValidHostileTarget(pAttacker))
            return pAttacker;
    }

    if (!IsInDuel())
    {
        // Check if other group members are under attack.
        if (Unit* pPartyAttacker = SelectPartyAttackTarget())
            return pPartyAttacker;
    }

    // Assist pet if its in combat.
    if (Pet* pPet = me->GetPet())
    {
        if (Unit* pPetAttacker = pPet->GetAttackerForHelper())
            if (IsValidHostileTarget(pPetAttacker))
                return pPetAttacker;
    }

    return nullptr;
}

Unit* PartyBotAI::SelectPartyAttackTarget() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            for (const auto pAttacker : pMember->GetAttackers())
            {
                if (IsValidHostileTarget(pAttacker) &&
                    me->IsWithinDist(pAttacker, 50.0f))
                    return pAttacker;
            }
        }
    }

    return nullptr;
}

Player* PartyBotAI::SelectResurrectionTarget(SpellEntry const* pSpellEntry) const
{
    if (IsInDuel() || !pSpellEntry)
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Player* pBest = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // Can't resurrect self.
            if (pMember == me)
                continue;

            // Released ghosts count too. Refusing them meant a healer gave up on anyone who
            // released, which is everyone once wipe recovery is doing its job.
            DeathState const deathState = pMember->GetDeathState();
            if (deathState != CORPSE && deathState != DEAD)
                continue;

            if (!me->IsWithinLOSInMap(pMember))
                continue;

            if (!pSpellEntry->IsTargetInRange(me, pMember))
                continue;

            // Order matters when the resurrection is rationed. A battle res is one per fight
            // and the fight is lost without healing long before it is lost without a rogue, so
            // a healer is taken over whoever the group happens to be iterated in front of.
            // Out of combat this only decides who stands up first.
            if (!pBest || (IsHealerClass(pMember->GetClass()) && !IsHealerClass(pBest->GetClass())))
                pBest = pMember;
        }
    }

    return pBest;
}

void PartyBotAI::AddSelfResurrectionReagent()
{
    // Reincarnation is the one reagent AddAllSpellReagents cannot reach, because it walks the
    // named spell slots and Reincarnation has never been one. It also cannot be discovered by
    // asking Player::SelectResurrectionSpellId, which reports the shaman has no self
    // resurrection available until the Ankh is already in the bag. So the pairing is named
    // here, the same pairing the engine keeps in Player.cpp: the talent the shaman learns, and
    // the spell that does the work and charges an Ankh for it.
    uint32 const REINCARNATION_PASSIVE = 20608;
    uint32 const REINCARNATION_EFFECT = 21169;

    if (!me->HasSpell(REINCARNATION_PASSIVE))
        return;

    SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(REINCARNATION_EFFECT);
    if (!pSpellEntry)
        return;

    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (pSpellEntry->Reagent[i] <= 0)
            continue;

        uint32 const itemId = uint32(pSpellEntry->Reagent[i]);
        if (!me->HasItemCount(itemId, pSpellEntry->ReagentCount[i]))
            AddItemToInventory(itemId, pSpellEntry->ReagentCount[i]);
    }
}

bool PartyBotAI::UseSelfResurrection()
{
    // The engine works out which self resurrection applies at the moment of death and leaves
    // the answer here, so a soulstone, an Ankh and Twisting Nether are all one branch and none
    // of them has to be recognised by name. This mirrors HandleSelfResOpcode, which is what a
    // client sends when the player takes the offer on the release dialog.
    uint32 const spellId = me->GetUInt32Value(PLAYER_SELF_RES_SPELL);
    if (!spellId)
        return false;

    SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spellId);
    if (!pSpellEntry)
        return false;

    // Cleared only once the cast is away, which is where this has to differ from the opcode.
    // A player clicks the button once and clearing it unconditionally costs them nothing; a
    // bot arrives here every tick, so clearing first would spend the charge on the first
    // attempt whatever came of it, and a single transient refusal would look ever after like a
    // shaman that simply does not reincarnate.
    if (me->CastSpell(me, pSpellEntry, false) != SPELL_CAST_OK)
        return false;

    me->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[PartyBot] '%s' self resurrected on map %u with spell %u.",
             me->GetName(), me->GetMapId(), spellId);
    return true;
}

// The worst-off member this bot could heal if only it were standing somewhere else. Range and
// line of sight are the two things a healer can fix by walking, so they are the only reasons
// anything is returned here: somebody merely above the heal threshold is not a problem that
// being nearer would solve.
Unit* PartyBotAI::SelectHealTargetOutOfReach() const
{
    if (IsInDuel())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    float const reach = GetMaxHealSpellRange();
    float worst = PB_HEAL_REPOSITION_PERCENT;
    Unit* pTarget = nullptr;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive() || !pMember->IsInWorld() ||
            pMember->GetMapId() != me->GetMapId())
            continue;

        if (pMember->GetHealthPercent() >= worst)
            continue;

        if (!me->IsValidHelpfulTarget(pMember))
            continue;

        // Already reachable, so the rotation has it and there is nothing to walk towards.
        if (me->IsWithinDist(pMember, reach) && me->IsWithinLOSInMap(pMember))
            continue;

        worst = pMember->GetHealthPercent();
        pTarget = pMember;
    }

    return pTarget;
}

// Who the follow generator is currently pointed at, or nothing if the bot is not following.
// Needed because "is this bot following" and "is this bot following the leader" stopped being
// the same question once a healer could be off following somebody it needs to reach.
Unit const* PartyBotAI::GetCurrentFollowTarget() const
{
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        return nullptr;

    if (FollowMovementGenerator<Player> const* pMoveGen =
            dynamic_cast<FollowMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent()))
        return pMoveGen->GetTarget();

    return nullptr;
}

Player* PartyBotAI::SelectShieldTarget() const
{
    if (!m_spells.priest.pPowerWordShield)
        return nullptr;

    if (IsInDuel())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            // Line of sight, which nothing here asked for and the spell then demanded: fifteen
            // shields in half an hour were thrown at a group member the priest could not see and
            // failed with SPELL_FAILED_LINE_OF_SIGHT, each one a tick given up. Range for the same
            // reason, taken from the spell rather than guessed at.
            if ((pMember->GetHealthPercent() < 90.0f) &&
                !pMember->GetAttackers().empty() &&
                !pMember->IsImmuneToMechanic(MECHANIC_SHIELD) &&
                me->IsWithinLOSInMap(pMember) &&
                m_spells.priest.pPowerWordShield->IsTargetInRange(me, pMember))
                return pMember;
        }
    }

    return nullptr;
}

bool PartyBotAI::CrowdControlMarkedTargets()
{
    SpellEntry const* pSpellEntry = GetCrowdControlSpell();
    if (!pSpellEntry)
        return false;

    for (auto mark : m_marksToCC)
    {
        if (Unit* pTarget = GetMarkedTarget(mark))
        {
            if (!pTarget->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                IsValidHostileTarget(pTarget) && !AreOthersOnSameTarget(pTarget->GetObjectGuid()))
            {
                if (CanTryToCastSpell(pTarget, pSpellEntry))
                {
                    if (DoCastSpell(pTarget, pSpellEntry) == SPELL_CAST_OK)
                    {
                        me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool PartyBotAI::AddToPlayerGroup()
{
    Player* pPlayer = ObjectAccessor::FindPlayer(m_leaderGuid);
    if (!pPlayer)
        return false;

    Group* group = pPlayer->GetGroup();
    if (!group)
    {
        group = new Group;
        // new group: if can't add then delete
        if (!group->Create(pPlayer->GetObjectGuid(), pPlayer->GetName()))
        {
            delete group;
            return false;
        }
        sObjectMgr.AddGroup(group);
    }

    if (me->GetGroup() == group)
        return true;

    if (me->GetGroup())
        me->RemoveFromGroup();

    // A party holds five, so everyone past that needs the group promoted to a raid first.
    if (group->IsFull() && !group->isRaidGroup())
        group->ConvertToRaid();

    if (!group->AddMember(me->GetObjectGuid(), me->GetName()))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[PartyBot] '%s' could not join the group of '%s'.",
                 me->GetName(), pPlayer->GetName());
        return false;
    }

    return true;
}

void PartyBotAI::OnPacketReceived(WorldPacket const* packet)
{
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        {
            if (m_initialized)
                m_resetSpellData = true;
            return;
        }
        case SMSG_DUEL_REQUESTED:
        {
            auto data = std::make_unique<WorldPackets::Duel::DuelAccepted>();
            data->playerGuid = me->GetObjectGuid();
            me->GetSession()->QueuePacket(std::move(data));
            return;
        }
        case SMSG_PARTYKILLLOG:
        {
            if (!me)
                return;

            // Noted for the cleanup pass in UpdateCorpseLooting. This is the one moment the corpse
            // is known for certain without searching the grid for it, so it is written down here and
            // dealt with later, once the fight is over and the group has had its pick.
            RememberCorpseToLoot(ObjectGuid(*(((uint64*)(*packet).contents()) + 1)));

            if (Group const* pGroup = me->GetGroup())
            {
                if (pGroup->GetLootMethod() == ROUND_ROBIN ||
                    pGroup->GetLootMethod() == GROUP_LOOT ||
                    pGroup->GetLootMethod() == NEED_BEFORE_GREED)
                {
                    ObjectGuid victimGuid = *(((uint64*)(*packet).contents()) + 1);
                    if (Creature* pCreature = me->GetMap()->GetCreature(victimGuid))
                    {
                        pCreature->m_Events.AddLambdaEventAtOffset([pCreature, guid = me->GetGUID()]()
                        {
                            if (pCreature->loot.roundRobinPlayer == guid)
                            {
                                // unassign loot from bot so real players can loot
                                pCreature->loot.roundRobinPlayer = 0;
                                pCreature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
                            }
                        }, 1);
                    }
                }
            }
            
            return;
        }
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

// How much of a roll's window to leave for the bots once the people have gone quiet. A roll nobody
// answers is decided on the votes it did get, so a bot that waited too long has passed by omission.
static constexpr uint32 PB_ROLL_DEFER_MARGIN_MS = 10 * IN_MILLISECONDS;

// Whether to hold this bot's vote back and ask again next tick.
//
// Bots decide instantly and a person has to notice the window and click it, so voting on sight put
// every bot's answer in before the player had one. Anyone the bot might defer to has to have spoken
// first for deference to mean anything, which means voting last.
//
// Bounded by the roll's own clock rather than waiting indefinitely, because a player who never
// answers is a common case -- the window is easy to miss and easy to ignore -- and a bot that waits
// for an answer that is not coming has passed on the item without deciding to.
bool PartyBotAI::ShouldDeferRollToPlayers(Roll const* pRoll) const
{
    bool pending = false;

    for (auto const& vote : pRoll->playerVote)
    {
        // Only people are worth waiting for. Other bots vote on the same tick this one does, so
        // waiting on them is waiting on nobody and would deadlock a group with no players in it.
        Player const* pVoter = ObjectAccessor::FindPlayer(vote.first);
        if (!pVoter || pVoter->IsBot())
            continue;

        if (vote.second == ROLL_NOT_EMITED_YET)
            pending = true;
    }

    if (!pending)
        return false;

    // No corpse means no clock to read, and guessing long here risks the roll resolving while the
    // bot is still being polite about it.
    Creature const* pCreature = me->GetMap()->GetCreature(pRoll->lootedTargetGUID);
    if (!pCreature)
        return false;

    return pCreature->GetGroupLootTimer() > PB_ROLL_DEFER_MARGIN_MS;
}

// Whether a person in this group has claimed the item for themselves.
//
// Need is the claim: greed is not, and neither is silence. A group of bots that stood down for
// anything a player so much as considered would never gear up at all, since most of what drops gets
// a greed roll from somebody.
bool PartyBotAI::DidPlayerNeedRoll(Roll const* pRoll) const
{
    for (auto const& vote : pRoll->playerVote)
    {
        if (vote.second != ROLL_NEED)
            continue;

        Player const* pVoter = ObjectAccessor::FindPlayer(vote.first);
        if (pVoter && !pVoter->IsBot())
            return true;
    }

    return false;
}

// Whether this bot wants the thing on the corpse badly enough to roll for it.
//
// Need for a genuine upgrade and pass on everything else, deliberately: greed would have the group
// hoovering up every vendor grey and every drop a real player was hoping for, and winning things it
// will not wear is how a bot's bags fill until it can no longer loot at all. Passing costs the bot
// nothing, because an item nobody needs still goes to whoever did want it.
RollVote PartyBotAI::DecideLootRoll(uint32 itemId) const
{
    ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
    if (!pProto)
        return ROLL_PASS;

    // Every branch reports itself, passes included. A pass is the answer that looks like a
    // malfunction from the outside -- a blue drops, a bot that obviously wants it says nothing, and
    // there is no way to tell a considered decline from a broken one. Declining a rare shield
    // because the green in the slot carries stamina is correct and completely invisible, so the
    // score that decided it is written down next to the verdict.
    char const* reason;
    float delta = 0.0f;
    RollVote vote = ROLL_PASS;

    // Not a question of taste. A bot that cannot wear the thing has no upgrade to measure, and
    // CanUseItem is what rules out the wrong armour class, the wrong weapon, and the level it has
    // not reached yet.
    if (me->CanUseItem(pProto) != EQUIP_ERR_OK)
        reason = "cannot use it at all";
    else if (StatWeights const* pWeights = GetStatWeights())
    {
        // Scored against what is worn in that slot, so the answer accounts for the thing being
        // replaced rather than the drop in isolation. Passed as a prototype with no Item behind it,
        // since the instance does not exist until somebody wins it: random-property enchantments
        // are invisible here, which understates a few drops and never overstates one.
        delta = sItemEvaluator.UpgradeDelta(me, pProto, nullptr, *pWeights);

        if (delta > 0.0f)
        {
            reason = "better than what it is wearing";
            vote = ROLL_NEED;
        }
        else
            reason = "no better than what it is wearing";
    }
    else
    {
        // Worth saying out loud rather than folding into the ordinary pass. A missing weight row
        // makes a bot decline everything forever, which is a configuration gap wearing the costume
        // of a decision.
        reason = "has no stat weights to judge it by";
    }

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[BotCombat] roll bot='%s' %s '%s' (%u): %s, worth "
                 "%+.1f", me->GetName(), vote == ROLL_NEED ? "needs" : "passes on", pProto->Name1,
                 itemId, reason, delta);
    }

    return vote;
}

// Answer any roll this bot has been asked for and has not yet voted on.
//
// Bots are entered into rolls exactly as players are, and with nobody to answer for them the item
// waited out its timer and went to whoever did vote. That is why the warrior stood over a shield it
// wanted: not indifference, but no way to say so.
void PartyBotAI::UpdateLootRolls()
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;

    // Details copied out before voting rather than voting mid-iteration. A vote can be the last one
    // a roll was waiting for, which finishes it, erases it from the list and deletes it, leaving
    // both the iterator and the Roll dangling. One vote per update makes that impossible, and a bot
    // ticks far more often than a roll's sixty seconds.
    ObjectGuid lootedTarget;
    uint32 itemSlot = 0;
    uint32 itemId = 0;
    bool defer = false;
    bool playerClaimed = false;

    for (Roll const* pRoll : pGroup->GetRolls())
    {
        if (!pRoll->isValid())
            continue;

        auto const vote = pRoll->playerVote.find(me->GetObjectGuid());
        if (vote == pRoll->playerVote.end() || vote->second != ROLL_NOT_EMITED_YET)
            continue;

        lootedTarget = pRoll->lootedTargetGUID;
        itemSlot = pRoll->itemSlot;
        itemId = pRoll->itemid;

        // Asked before deferring, and it is why the wait can end early: once somebody has claimed
        // the item there is nothing left to wait for and nothing left to decide.
        playerClaimed = DidPlayerNeedRoll(pRoll);
        defer = !playerClaimed && ShouldDeferRollToPlayers(pRoll);
        break;
    }

    if (lootedTarget.IsEmpty())
        return;

    // Still waiting on a person to make up their mind. Left unvoted, which is not the same as a
    // pass: the roll stays open and this bot is asked again on the next tick.
    if (defer)
        return;

    // A player wants it, so the bots are out of it regardless of what it would be worth to them.
    // Passing rather than greeding, so that the roll is not merely lost but uncontested.
    RollVote const vote = playerClaimed ? ROLL_PASS : DecideLootRoll(itemId);

    pGroup->CountRollVote(me, lootedTarget, itemSlot, vote);
}

void PartyBotAI::RememberCorpseToLoot(ObjectGuid guid)
{
    if (!guid.IsCreature())
        return;

    for (PartyBotCorpse const& corpse : m_corpsesToLoot)
        if (corpse.guid == guid)
            return;

    // Oldest goes when the list is full. A corpse that has waited through this many kills is either
    // out of reach for good or already gone.
    if (m_corpsesToLoot.size() >= PB_LOOT_QUEUE_LIMIT)
        m_corpsesToLoot.erase(m_corpsesToLoot.begin());

    m_corpsesToLoot.push_back({ guid, time(nullptr) + PB_LOOT_GRACE_SECONDS });
}

// Whether any real player in the group could still loot this corpse themselves. If one can, the bot
// leaves it alone: emptying it takes the loot out from under someone who was entitled to it, and the
// only reason a bot is looting at all is to clear corpses nobody else is able to.
bool PartyBotAI::CanAnyPlayerLoot(Creature* pCreature) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember->IsBot())
            continue;

        if (pMember->IsAllowedToLoot(pCreature))
            return true;
    }

    return false;
}

// Empty a corpse completely, which is the only thing that makes it skinnable. Anything left behind,
// down to the last grey, keeps a skinner on "creature must be looted first": the check wants the
// loot gone rather than merely opened.
//
// The session's own handlers do the work rather than a reimplementation of them. A bot owns a real
// WorldSession and every one of these is public on it, so this takes the same path a player's client
// would and cannot drift away from it as the loot rules change.
bool PartyBotAI::LootCorpse(Creature* pCreature)
{
    if (!me->IsAllowedToLoot(pCreature))
        return false;

    // Somebody real can still take this, so it is not the bot's to take.
    if (CanAnyPlayerLoot(pCreature))
        return false;

    // Held to the same distance the client's own loot request is held to, and not closed manually:
    // walking to corpses would have the bot wander off after fights and into the mobs the aggro rule
    // spends its time keeping it away from. It does not need to. Bots follow the leader at a few
    // yards, so a skinner standing over a corpse brings one along with them.
    if (!pCreature->IsWithinDistInMap(me, me->GetMaxLootDistance(pCreature), true, SizeFactor::None))
        return false;

    WorldSession* pSession = me->GetSession();
    if (!pSession)
        return false;

    // Opens the loot, and sets the loot guid that every handler below reads back.
    me->SendLoot(pCreature->GetObjectGuid(), LOOT_CORPSE);
    if (me->GetLootGuid() != pCreature->GetObjectGuid())
        return false;

    uint32 const maxSlot = pCreature->loot.GetMaxSlotInLootFor(me->GetGUIDLow());
    for (uint32 slot = 0; slot < maxSlot; ++slot)
    {
        WorldPackets::Loot::AutoStoreLootItem take;
        take.lootSlot = uint8(slot);
        pSession->HandleAutostoreLootItemOpcode(take);
    }

    if (pCreature->loot.gold)
    {
        NullClientPacket money(CMSG_LOOT_MONEY);
        pSession->HandleLootMoneyOpcode(money);
    }

    // The release is what finishes it. It is the only path that tests isLooted, drops the lootable
    // flag and calls AllLootRemovedFromCorpse, and that last call starts the tap timer the skinning
    // check waits on. Emptying the loot without releasing would leave the corpse looking full.
    pSession->DoLootRelease(pCreature->GetObjectGuid());

    return pCreature->loot.isLooted();
}

void PartyBotAI::UpdateCorpseLooting()
{
    // Never during a fight. A tick spent looting is a tick not spent healing or holding threat, and
    // the corpse is not going anywhere.
    if (m_corpsesToLoot.empty() || me->IsInCombat() || !me->IsAlive() || me->IsBeingTeleported())
        return;

    time_t const now = time(nullptr);

    for (auto itr = m_corpsesToLoot.begin(); itr != m_corpsesToLoot.end();)
    {
        // Still inside the grace period. Loot permission settles over the first moment or so after a
        // kill -- the round robin assignment a bot holds is given up a tick later, and group rolls
        // take longer than that -- so asking too early would read a corpse as nobody's when it was
        // about to become someone's.
        if (now < itr->lootAfter)
        {
            ++itr;
            continue;
        }

        Creature* pCreature = me->GetMap()->GetCreature(itr->guid);
        if (!pCreature || pCreature->IsAlive() || pCreature->loot.isLooted())
        {
            itr = m_corpsesToLoot.erase(itr);
            continue;
        }

        // Out of reach, or still somebody else's to take. Kept on the list rather than dropped:
        // range changes as the bot follows the leader, and permission changes as rolls resolve and
        // players walk away. It leaves the list with the corpse itself when that despawns.
        if (!LootCorpse(pCreature))
        {
            ++itr;
            continue;
        }

        if (sWorld.getConfig(CONFIG_BOOL_PARTY_BOT_COMBAT_LOG))
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] loot bot='%s' emptied '%s' on map %u, now skinnable",
                     me->GetName(), pCreature->GetName(), me->GetMapId());

        itr = m_corpsesToLoot.erase(itr);

        // One a tick. Emptying a whole pack in a single pass is a burst of inventory work and
        // several item-received packets for no gain; the next tick is fifty milliseconds away.
        return;
    }
}

void PartyBotAI::OnPlayerLogin()
{
    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
}

void PartyBotAI::UpdateAI(uint32 const diff)
{
    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(PB_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported())
        return;

    if (!m_initialized)
    {
        // Running the AI ungrouped is not survivable, so bail out rather than
        // initializing a bot that never made it into the group.
        if (!AddToPlayerGroup())
        {
            botEntry->requestRemoval = true;
            return;
        }

        if (m_race && m_class) // temporary character
        {
            if (m_level && m_level != me->GetLevel())
            {
                me->GiveLevel(m_level);
                me->InitTalentForLevel();
                me->SetUInt32Value(PLAYER_XP, 0);
            }

            if (!m_cloneGuid.IsEmpty())
            {
                CloneFromPlayer(sObjectAccessor.FindPlayer(m_cloneGuid));
                AutoAssignRole();
            }
            else
            {
                LearnPremadeSpecForClass();

                if (m_role == ROLE_INVALID)
                    AutoAssignRole();

                AutoEquipGear(sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_AUTO_EQUIP));

                // fix client bug causing some item slots to not be visible
                if (Player* pLeader = GetPartyLeader())
                {
                    me->SetVisibility(VISIBILITY_OFF);
                    pLeader->UpdateVisibilityOf(pLeader, me);
                    me->SetVisibility(VISIBILITY_ON);
                }
            }
            me->UpdateSkillsToMaxSkillsForLevel();
        }
        else // loaded from db
        {
            if (m_role == ROLE_INVALID)
                AutoAssignRole();

            if (me->IsGameMaster())
                me->SetGameMaster(false);

            me->TeleportTo(m_mapId, m_x, m_y, m_z, m_o);
        }

        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();

        // Stocked here alongside the reagents, and for the same reason: what it saves is a trip
        // to a vendor, which is gold and tedium rather than any part of the game being measured.
        // What it no longer does is refill a quiver that empties mid-fight.
        AddHunterAmmo();
        AddSelfResurrectionReagent();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();

        // A character conjured a moment ago has no history worth keeping, so it starts whole.
        // One loaded from the database keeps the health and mana it logged out with, because
        // otherwise dismissing a wiped roster and summoning it straight back is a free full
        // heal, and the corpse run that recovery rests on is one command away from optional.
        if (m_race && m_class)
        {
            me->SetHealthPercent(100.0f);
            me->SetPowerPercent(me->GetPowerType(), 100.0f);
        }
        else if (me->IsAlive() && !me->GetHealth())
        {
            // Alive at no health is not a state anything recovers from on its own.
            me->SetHealthPercent(100.0f);
        }

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        // Opt this bot's movement into the aggro check in the chase and follow generators, which is
        // what actually keeps it from walking the group into a pack. Set here rather than for every
        // bot, because a battleground bot runs the same AI base through maps full of neutral
        // creatures standing beside the only route anywhere.
        me->SetAvoidAggroPulls(true);

        m_initialized = true;
        return;
    }

    if (m_resetSpellData)
    {
        ResetSpellData();
        PopulateSpellData();
        m_resetSpellData = false;
    }

    Player* pLeader = GetPartyLeader();
    if (!pLeader)
    {
        botEntry->requestRemoval = true;
        return;
    }

    if (!pLeader->IsInWorld())
        return;

    if (pLeader->InBattleGround() &&
        !me->InBattleGround())
    {
        if (m_receivedBgInvite)
        {
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
            return;
        }

        // Remain idle until we can join battleground.
        return;
    }

    if (pLeader->IsTaxiFlying())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
        }
        return;
    }

    if (me->HasUnitState(UNIT_STATE_FEIGN_DEATH) && me->HasAuraType(SPELL_AURA_FEIGN_DEATH) &&
       !me->IsInCombat() && (!me->GetPet() || !me->GetPet()->IsInCombat()) &&
       !me->SelectRandomUnfriendlyTarget(nullptr, 20.0f, false, true))
        me->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    if (me->IsDead())
    {
        UpdateDeadAI();
        return;
    }

    // Ahead of everything below, and not confined to the out-of-combat path: a roll opens the moment
    // its corpse is looted, which in a chain pull is in the middle of the next fight, and a roll left
    // unanswered until the fight ends has already timed out.
    UpdateLootRolls();

    // Back on our feet, by whichever route. Clearing here covers all of them, so a later
    // death cannot inherit stale timestamps and skip straight to the spirit healer.
    m_corpseSince = 0;
    m_ghostSince = 0;
    m_ghostStart = 0;
    m_leaderWaitSince = 0;
    m_corpseRunBestDistance = -1.0f;

    // An order is spent once its target is gone or is fighting somebody. At that point the aggro
    // rule lets the bot approach anyway, since the mob is engaged, so holding the suspension open
    // any longer would only extend it to the rest of the room for nothing. Expiring it on the target
    // rather than on a timer is what keeps a bot from quietly keeping the exemption for a whole
    // instance after one order.
    //
    // Not while a pull is still running, though. The mob entering combat is the pull working, and it
    // is also the moment the puller has to walk home past that same mob, so expiring the order there
    // expires it exactly when it is needed. The log of one Wailing Caverns pull has both lines on the
    // same second: "it bit, heading home", then the rule refusing the route home and the puller
    // holding position in the open. EndPull clears the order for that case instead.
    if (me->HasAttackOrders() && !IsPulling())
    {
        Unit* pOrdered = me->GetMap()->GetUnit(me->GetAttackOrders());
        if (!pOrdered || !pOrdered->IsAlive() || pOrdered->IsInCombat())
            me->SetAttackOrders(ObjectGuid());
    }

    // Ahead of the auto shot branch below, which returns on every tick that a ranged attack is
    // running. The puller fires one, so leaving this until later would strand it shooting from the
    // spot it pulled from and it would never reach the step where it stops and walks back.
    if (IsPulling())
    {
        if (UpdatePullSequence())
            return;
    }
    else if (m_holdPosition && ShouldBreakHold())
        ReleaseHold();

    // A shot or a wand already in flight is no reason to stop thinking, and this used to return as
    // though it were. Everything below was skipped for as long as the autorepeat lasted, which is
    // most of a fight for a hunter: the pet was never given a target, because the command that does
    // that is further down, and a priest that had started wanding stopped healing until the wand
    // stopped. Only hunters got a rotation out of it, and only by calling one from in here.
    //
    // The interrupts stay, because both cases below are shots that will never leave: nothing to
    // shoot at, or a target inside the minimum range a bow needs. Dropping the autorepeat lets the
    // rotation reach for something it can actually do.
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        if (!me->GetVictim() ||
            (me->GetClass() == CLASS_HUNTER && me->GetCombatDistance(me->GetVictim()) < 8.0f))
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
    }

    if (Spell* pCurrentSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        // Interrupt pre casted heals if target is not injured.
        if (pCurrentSpell->getState() == SPELL_STATE_PREPARING &&
            pCurrentSpell->m_spellInfo->IsHealSpell())
        {
            if (Unit* pTarget = pCurrentSpell->m_targets.getUnitTarget())
            {
                if (pTarget->GetHealth() == pTarget->GetMaxHealth())
                {
                    me->InterruptSpell(CURRENT_GENERIC_SPELL, true);
                }
            }
        }
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    if (!me->IsInCombat())
    {
        // Catching up is dealt with before sitting down, and it has to be. These two were the
        // other way around, and since a bot that needs food or water never falls through the
        // drink branch, the only way one could ever reach the teleport was to stop needing
        // either: hence the full health and mana handed to anyone further than this from the
        // leader. A raid spends much of its time spread wider than a hundred yards, so that
        // covered most of the roster most of the time, and mana pressure across a strung-out
        // group could never be seen. Bring the straggler back and let it drink with everyone
        // else, at the same cost in time.
        if (!me->IsWithinDistInMap(pLeader, 100.0f) && !IsInDuel())
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            me->GetMotionMaster()->MoveIdle();
            char name[128] = {};
            snprintf(name, sizeof(name), "%s", pLeader->GetName());
            ChatHandler(me).HandleGonameCommand(name);
            return;
        }

        // Ahead of drinking, which returns for as long as it lasts. Behind it a corpse would wait
        // out the whole break and be gone by the end of it.
        UpdateCorpseLooting();

        // Same reason, and the reason this is not left until the group stops to rest: gear won or
        // looted is worth nothing until it is worn, and the next pull may come before any sitting
        // down happens.
        if (m_equipCheckPending)
        {
            m_equipCheckPending = false;
            EquipOrUseNewItem();
            UpdateVisualHonorRankBasedOnItems();
        }

        if (DrinkAndEat())
        {
            if (me->IsMounted())
                me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
            return;
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    if (!me->IsInCombat() && !me->IsMounted())
    {
        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        if (me->IsNonMeleeSpellCasted())
            return;
    }

    Unit* pVictim = me->GetVictim();

    if (GetRole() != ROLE_HEALER)
    {
        if (!pVictim || !IsValidHostileTarget(pVictim))
        {
            if (pVictim)
                me->AttackStop();

            if (Unit* pNewVictim = SelectAttackTarget(pLeader))
            {
                // Holding means not closing the distance. It does not mean standing there with no
                // target: acquiring one costs nothing while the bot stays put, and it lets a held
                // caster or hunter work on the mob as it comes in rather than waiting for it to
                // finish arriving. A held melee bot simply cannot reach yet, which is the wait.
                if (m_holdPosition)
                {
                    me->Attack(pNewVictim, true);
                }
                else
                {
                    AttackStart(pNewVictim);
                    return;
                }
            }
        }
    }

    if (!me->IsInCombat())
    {
        // Mount if leader is mounted and we don't have a target.
        if (pLeader->IsMounted() && !me->GetVictim())
        {
            if (!me->IsMounted())
            {
                // Leave shapeshift before mounting.
                if (me->IsInDisallowedMountForm() &&
                    me->GetDisplayId() != me->GetNativeDisplayId() &&
                    me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
                    me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

                auto auraList = pLeader->GetAurasByType(SPELL_AURA_MOUNTED);
                if (!auraList.empty())
                {
                    bool oldStateCastTime = me->HasCheatOption(PLAYER_CHEAT_NO_CAST_TIME);
                    bool oldStatePower = me->HasCheatOption(PLAYER_CHEAT_NO_POWER);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, true);
                    me->CastSpell(me, (*auraList.begin())->GetId(), true);
                    me->SetCheatOption(PLAYER_CHEAT_NO_CAST_TIME, oldStateCastTime);
                    me->SetCheatOption(PLAYER_CHEAT_NO_POWER, oldStatePower);
                }
            }
        }
        else if (me->IsMounted())
            me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    }

    // Both branches below exist to close a distance, by chasing a target or by trailing the leader,
    // and closing distances is the one thing a held bot must not do. Skipping the pair of them is
    // the whole of what the hold enforces; the combat rotation underneath carries on as normal.
    if (!me->IsMoving() && !m_holdPosition)
    {
        if (!pVictim)
        {
            // A healer with somebody hurt and out of reach has somewhere more useful to be than
            // tucked in behind the leader. This is the other half of the thirty yard heal cap:
            // even with the range read from the spell, the leader and the tank are not always in
            // the same place, and nothing here ever connected being unable to reach a heal
            // target to doing something about it. A tank that charges ahead pulls that gap open
            // by itself, and the healer used to just stand still and watch.
            Unit* pOutOfReach = (m_role == ROLE_HEALER) ? SelectHealTargetOutOfReach() : nullptr;
            Unit const* pFollowing = GetCurrentFollowTarget();

            if (pOutOfReach)
            {
                if (pFollowing != pOutOfReach)
                    me->GetMotionMaster()->MoveFollow(pOutOfReach, PB_HEAL_REPOSITION_DIST,
                                                      frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
            }
            // Testing the target and not merely the generator type, because a healer coming back
            // from a reposition is still following, just following the wrong unit, and a bare
            // type check leaves it trailing whoever it went to help for the rest of the fight.
            else if (pFollowing != pLeader)
                me->GetMotionMaster()->MoveFollow(pLeader, urand(PB_MIN_FOLLOW_DIST, PB_MAX_FOLLOW_DIST), frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
        }
        else
        {
            if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
               (GetRole() == ROLE_MELEE_DPS || m_role == ROLE_TANK) &&
                IsValidHostileTarget(pVictim) &&
                AttackStart(pVictim))
                return;

            switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            {
                case IDLE_MOTION_TYPE:
                case FOLLOW_MOTION_TYPE:
                    BeginChasing(pVictim);
                    break;
            }
        }
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}


void PartyBotAI::UpdateOutOfCombatAI()
{
    if (!IsInDuel())
    {
        if (m_resurrectionSpell)
            if (Player* pTarget = SelectResurrectionTarget(m_resurrectionSpell))
                if (CanTryToCastSpell(pTarget, m_resurrectionSpell))
                    if (DoCastSpell(pTarget, m_resurrectionSpell) == SPELL_CAST_OK)
                        return;

        if (m_role != ROLE_TANK && me->GetVictim() && CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

// One line per tick describing the situation the bot is deciding in. The cast lines from
// DoCastSpell say what it chose; this says what it was looking at, which is the only way to read
// a tick where it chose nothing. Both halves are needed: a tank line showing full rage and no
// cast beside it means something is gating the rotation, and the same line with no rage means
// the rotation is fine and the rage is not there.
void PartyBotAI::LogCombatTick() const
{
    // Throttled to roughly a line a second per bot, independently of how often the bot thinks. The
    // tick rate is a tuning knob and the log is read by eye and by script; tying the two together
    // means every change to the first silently rescales the second, and the rate that makes a good
    // rotation does not make a readable log.
    uint32 const now = WorldTimer::getMSTime();
    if (m_lastTickLog && WorldTimer::getMSTimeDiff(m_lastTickLog, now) < PB_TICK_LOG_INTERVAL_MS)
        return;

    m_lastTickLog = now;

    Unit* pVictim = me->GetVictim();

    // Rage and energy are held at ten times the displayed number, and these lines get read
    // against the rotation's own thresholds.
    Powers const powerType = me->GetPowerType();
    uint32 power = me->GetPower(powerType);
    if (powerType == POWER_RAGE || powerType == POWER_ENERGY)
        power /= 10;

    // Whether the global cooldown was running when this tick ran, which is the difference between
    // a tick that could not act and a tick that would not. Without it the two are indistinguishable
    // in the log and the idle half of every fight cannot be read: the bots update once a second
    // against a cooldown of one and a half, so a third of all ticks are gated by arithmetic alone
    // and no amount of counting empty ticks says which third. Passing no spell asks after any
    // category rather than a particular one, which is the question worth logging.
    uint32 const gcd = me->HasGCD(nullptr) ? 1 : 0;

    if (m_role == ROLE_TANK)
    {
        float myThreat = 0.0f;
        float topThreat = 0.0f;
        char const* topName = "none";
        bool holding = false;

        if (pVictim && pVictim->CanHaveThreatList())
        {
            // Neither the lookup nor the container beneath it is marked const, though both are
            // read-only here. Same reason as ShouldTauntTarget above.
            ThreatManager& threat = pVictim->GetThreatManager();
            myThreat = threat.getThreat(me);

            if (HostileReference const* pTop = threat.getCurrentVictim())
            {
                topThreat = pTop->getThreat();
                if (Unit const* pTopUnit = pTop->getTarget())
                {
                    topName = pTopUnit->GetName();
                    holding = (pTopUnit == me);
                }
            }
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] tick bot='%s' role=tank lvl=%u hp=%.0f rage=%u victim='%s' vhp=%.0f "
                 "attackers=%u nearby=%u mythreat=%.0f topthreat=%.0f top='%s' holding=%u "
                 "gcd=%u stance=%u",
                 me->GetName(), me->GetLevel(), me->GetHealthPercent(), power,
                 pVictim ? pVictim->GetName() : "none",
                 pVictim ? pVictim->GetHealthPercent() : 0.0f,
                 uint32(me->GetAttackers().size()),
                 pVictim ? uint32(me->GetEnemyCountInRadiusAround(pVictim, 8.0f)) : 0u,
                 myThreat, topThreat, topName, uint32(holding),
                 gcd, uint32(me->GetShapeshiftForm()));
        return;
    }

    if (m_role == ROLE_MELEE_DPS || m_role == ROLE_RANGE_DPS)
    {
        // Whether the two things that actually produce most of a damage bot's output are running.
        // A rotation can look busy in the cast log and still be worth very little if the swing
        // timer or the shot timer is not turning underneath it, and neither leaves a cast line.
        uint32 const autoRepeat = me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) ? 1 : 0;
        bool const meleeOn = pVictim && me->HasUnitState(UNIT_STATE_MELEE_ATTACKING);

        // Distance and reach together, since out of range is the commonest reason a bot with a
        // target is doing nothing at all, and the two roles fail it in opposite directions: a
        // melee bot stopped short of its victim, a caster driven inside its own standoff.
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] tick bot='%s' role=%s class=%u lvl=%u hp=%.0f pw=%u victim='%s' "
                 "vhp=%.0f vdist=%.1f melee=%u autorepeat=%u casting=%u moving=%u holding=%u gcd=%u",
                 me->GetName(), GetRoleName(m_role), uint32(me->GetClass()), me->GetLevel(),
                 me->GetHealthPercent(), power,
                 pVictim ? pVictim->GetName() : "none",
                 pVictim ? pVictim->GetHealthPercent() : 0.0f,
                 pVictim ? me->GetDistance(pVictim) : 0.0f,
                 uint32(meleeOn ? 1 : 0), autoRepeat,
                 uint32(me->IsNonMeleeSpellCasted() ? 1 : 0),
                 uint32(me->IsStopped() ? 0 : 1),
                 uint32(m_holdPosition ? 1 : 0), gcd);
        return;
    }

    // The worst-off member, found without reference to the thresholds the rotation heals on.
    // Reporting the rotation's own choice would only ever agree with itself; reporting the
    // truth is what makes a line showing somebody at forty percent and no cast beside it
    // legible as a fault.
    Player* pWorst = nullptr;
    float worstPct = 101.0f;

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || !pMember->IsAlive() || !pMember->IsInWorld() ||
                pMember->GetMapId() != me->GetMapId())
                continue;

            if (pMember->GetHealthPercent() < worstPct)
            {
                worstPct = pMember->GetHealthPercent();
                pWorst = pMember;
            }
        }
    }

    // Why the worst-off member is not being healed, which is the gap that let the thirty yard
    // cap hide for as long as it did. Everything logged elsewhere is an attempt: a decision
    // thrown out before DoCastSpell was ever called left no trace at all, so six seconds of a
    // healer standing over a dying tank with full mana read as six seconds of nothing
    // happening. These are the tests IsValidHealTarget applies, in its order.
    float const reach = GetMaxHealSpellRange();
    char const* reason = "none";
    if (pWorst)
    {
        if (!me->IsValidHelpfulTarget(pWorst))
            reason = "not_helpful";
        else if (!me->IsWithinLOSInMap(pWorst))
            reason = "no_los";
        else if (!me->IsWithinDist(pWorst, reach))
            reason = "out_of_range";
        else
            reason = "reachable";
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[BotCombat] tick bot='%s' role=healer lvl=%u hp=%.0f mana=%.0f worst='%s' whp=%.0f "
             "wdist=%.1f reach=%.0f wreason=%s incoming=%d casting=%u attackers=%u gcd=%u",
             me->GetName(), me->GetLevel(), me->GetHealthPercent(),
             me->GetPowerPercent(POWER_MANA),
             pWorst ? pWorst->GetName() : "none",
             pWorst ? pWorst->GetHealthPercent() : 0.0f,
             pWorst ? me->GetDistance(pWorst) : 0.0f, reach, reason,
             pWorst ? GetIncomingdamage(pWorst) : 0,
             uint32(me->IsNonMeleeSpellCasted() ? 1 : 0),
             uint32(me->GetAttackers().size()), gcd);
}

void PartyBotAI::UpdateInCombatAI()
{
    if (IsCombatLogged())
        LogCombatTick();

    // Ahead of every early return below, because a damage dealer that took a different branch
    // this tick is still swinging.
    if (Unit* pVictim = me->GetVictim())
        HoldOpeningSwings(pVictim);

    if (!IsInDuel())
    {
        if (m_role == ROLE_TANK)
        {
            Unit* pVictim = me->GetVictim();

            // Defend party members.
            if (!pVictim || pVictim->GetVictim() == me)
            {
                if (pVictim = SelectPartyAttackTarget())
                {
                    me->AttackStop(true);
                    AttackStart(pVictim);
                }
            }

            // Take the target back off whoever has it. The test used to be that the mob was
            // simply looking at someone else, which taunts it off the other tank as readily as
            // off a mage, and two tanks then spend the encounter trading it between them while
            // neither has a taunt left when a damage dealer actually needs saving.
            //
            // Returns on success, which reads like giving up the tick's cast and is not. Taunt does
            // carry StartRecoveryTime 0, so nothing here is waiting on the global cooldown, but the
            // server will not start a second cast in the same tick as the first regardless of
            // cooldowns: the one already cast still holds the caster's current-spell slot when the
            // next is checked, and that check answers SPELL_FAILED_SPELL_IN_PROGRESS. So carrying on
            // does not buy a second ability, it only spends the attempt and logs the refusal. The
            // ability is cast on the following tick instead, which is what was already happening
            // underneath the failures.
            if (pVictim && ShouldTauntTarget(pVictim))
            {
                for (const auto& pSpellEntry : m_spellListTaunt)
                {
                    if (CanTryToCastSpell(pVictim, pSpellEntry))
                    {
                        if (DoCastSpell(pVictim, pSpellEntry) == SPELL_CAST_OK)
                            return;
                    }
                }
            }
        }
        else if (CrowdControlMarkedTargets())
            return;
    }

    if (CheckForDispelTargets())
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();
}

bool PartyBotAI::CheckForDispelTargets()
{
    if (me->GetShapeshiftForm() != FORM_NONE)
        return false;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pCleanse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
                    {
                        if (DoCastSpell(pFriend, m_spells.paladin.pCleanse) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_SHAMAN:
        {
            if (m_spells.shaman.pCureDisease)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCureDisease))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.shaman.pCureDisease))
                    {
                        if (DoCastSpell(pFriend, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }

            if (m_spells.shaman.pCurePoison)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCurePoison))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.shaman.pCurePoison))
                    {
                        if (DoCastSpell(pFriend, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (m_spells.mage.pRemoveLesserCurse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.mage.pRemoveLesserCurse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.mage.pRemoveLesserCurse))
                    {
                        if (DoCastSpell(pFriend, m_spells.mage.pRemoveLesserCurse) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_PRIEST:
        {
            if (m_spells.priest.pDispelMagic)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                    {
                        if (DoCastSpell(pFriend, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            if (m_spells.priest.pAbolishDisease)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                    {
                        if (DoCastSpell(pFriend, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
        case CLASS_DRUID:
        {
            SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                m_spells.druid.pAbolishPoison :
                m_spells.druid.pCurePoison;

            if (pDispelSpell)
            {
                if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
                {
                    if (CanTryToCastSpell(pFriend, pDispelSpell))
                    {
                        if (DoCastSpell(pFriend, pDispelSpell) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }

            if (m_spells.druid.pRemoveCurse)
            {
                if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
                {
                    if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                    {
                        if (DoCastSpell(pFriend, m_spells.druid.pRemoveCurse) == SPELL_CAST_OK)
                            return true;
                    }
                }
            }
            break;
        }
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_TANK &&
        m_spells.paladin.pRighteousFury &&
        CanTryToCastSpell(me, m_spells.paladin.pRighteousFury))
    {
        if (DoCastSpell(me, m_spells.paladin.pRighteousFury) == SPELL_CAST_OK)
            return;
    }

    SpellEntry const* pBlessing = nullptr;
    if (Player* pTarget = SelectBlessingTarget(pBlessing))
    {
        if (CanTryToCastSpell(pTarget, pBlessing))
        {
            if (DoCastSpell(pTarget, pBlessing) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;
}

void PartyBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       (m_role != ROLE_TANK) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
           (me->GetHealthPercent() > 80.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }
    }

    if (!me->GetAttackers().empty())
    {
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield))
        {
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.paladin.pTurnEvil &&
            m_role != ROLE_TANK)
        {
            Unit* pAttacker = SelectAttackerDifferentFrom(me->GetVictim());
            if (pAttacker && pAttacker->GetCreatureType() == CREATURE_TYPE_UNDEAD &&
                CanTryToCastSpell(pAttacker, m_spells.paladin.pTurnEvil))
            {
                if (DoCastSpell(pAttacker, m_spells.paladin.pTurnEvil) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (GetRole() == ROLE_HEALER)
    {
        if (m_spells.paladin.pHolyShock &&
            me->GetHealthPercent() < 50.0f &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShock))
        {
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(me, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;
        }

        if (FindAndHealInjuredAlly(80.0f, 90.0f))
            return;

        if (FindAndPreHealTarget())
            return;
    }
    else
    {
        if (m_spells.paladin.pLayOnHands &&
           (me->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(me, m_spells.paladin.pLayOnHands))
        {
            if (DoCastSpell(me, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;
        }

        bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

        if (!hasSeal &&
            m_spells.paladin.pSeal &&
            CanTryToCastSpell(me, m_spells.paladin.pSeal))
        {
            me->CastSpell(me, m_spells.paladin.pSeal, false);
        }

        if (Unit* pVictim = me->GetVictim())
        {
            if (hasSeal && m_spells.paladin.pJudgement &&
               (me->GetPowerPercent(POWER_MANA) > 30.0f) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfJustice &&
               (pVictim->IsNonMeleeSpellCasted() ||
               (me->GetHealthPercent() < 20.0f && !me->GetAttackers().empty())) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHammerOfWrath &&
                pVictim->GetHealthPercent() < 20.0f &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pConsecration &&
               (GetAttackersInRangeCount(10.0f) > 2) &&
                CanTryToCastSpell(me, m_spells.paladin.pConsecration))
            {
                if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyShock &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
            {
                if (m_spells.paladin.pDivineFavor &&
                    CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
                {
                    DoCastSpell(me, m_spells.paladin.pDivineFavor);
                }

                if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pExorcism &&
                pVictim->IsCreature() &&
                (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
                CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.paladin.pHolyWrath &&
                pVictim->IsCreature() &&
               (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD ||
                pVictim->GetCreatureType() == CREATURE_TYPE_DEMON) &&
               (me->GetAttackers().size() < 3) && // too much pushback
                CanTryToCastSpell(pVictim, m_spells.paladin.pHolyWrath))
            {
                if (DoCastSpell(pVictim, m_spells.paladin.pHolyWrath) == SPELL_CAST_OK)
                    return;
            }
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }
        }
    }

    if (m_spells.paladin.pBlessingOfFreedom &&
       (me->HasUnitState(UNIT_STATE_ROOT) || me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
        CanTryToCastSpell(me, m_spells.paladin.pBlessingOfFreedom))
    {
        if (DoCastSpell(me, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER &&
        me->GetHealthPercent() < 30.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
    {
        if (SummonShamanTotems())
            return;

        UpdateInCombatAI_Shaman();
    }
}

void PartyBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pManaTideTotem &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
    {
        if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
            return;
    }

    if (GetRole() != ROLE_HEALER)
    {
        if (Unit* pVictim = me->GetVictim())
        {
            if (m_spells.shaman.pElementalMastery &&
                me->GetAttackers().empty() &&
                CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
            {
                if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pEarthShock &&
                pVictim->IsNonMeleeSpellCasted(false, false, true) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFrostShock &&
                pVictim->IsMoving() &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pStormstrike &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pChainLightning &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pPurge &&
                IsValidDispelTarget(pVictim, m_spells.shaman.pPurge) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pPurge))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pFlameShock &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.shaman.pLightningBolt &&
               (GetRole() == ROLE_RANGE_DPS || !me->CanReachWithMeleeAutoAttack(pVictim)) &&
                CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
            {
                if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    // Healing outranks laying a totem, which it did not before: totems were summoned here and
    // returned, so a healer never reached the block below on any tick it had a totem missing.
    // A totem is worth a small steady trickle to the group and a heal is worth whoever is about
    // to die, and the capture that prompted this has a shaman putting down Strength of Earth
    // while the tank fell past forty percent on the same second.
    //
    // Pre-healing stays underneath totems, because that is a luxury and a totem is not.
    if (GetRole() == ROLE_HEALER && FindAndHealInjuredAlly(50.0f, 90.0f))
        return;

    if (SummonShamanTotems())
        return;

    if (GetRole() == ROLE_HEALER)
    {
        if (FindAndPreHealTarget())
            return;
    }
    else if (me->GetHealthPercent() < 20.0f)
        HealInjuredTarget(me);
}

void PartyBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheHawk &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
    {
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        // Not while holding. Sending the pet is the same pull as going itself, taken by proxy.
        if (Pet* pPet = m_holdPosition ? nullptr : me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Hunter();
    }
    else
        SummonPetIfNeeded();
}

void PartyBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.hunter.pVolley &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pVolley))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pVolley) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {
            // An empty quiver is allowed to mean something. A fresh stack used to be handed
            // over the instant a shot failed for want of ammo, so a hunter could never stop
            // shooting and never having restocked cost nothing. Bots are stocked when they
            // spawn instead, and one that empties its quiver mid-raid stays empty until it
            // is summoned again.
            me->CastSpell(pVictim, PB_SPELL_AUTO_SHOT, false);
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pMultiShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;
        }

        if (GetAttackersInRangeCount(8.0f))
        {
            Unit* pAttacker = *me->GetAttackers().begin();

            if (m_spells.hunter.pScareBeast &&
               !WouldFearPullExtraEnemies() &&
                CanTryToCastSpell(pAttacker, m_spells.hunter.pScareBeast))
            {
                if (DoCastSpell(pAttacker, m_spells.hunter.pScareBeast) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pDisengage &&
                CanTryToCastSpell(pAttacker, m_spells.hunter.pDisengage))
            {
                if (DoCastSpell(pAttacker, m_spells.hunter.pDisengage) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pAspectOfTheMonkey &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.hunter.pFeignDeath &&
               (me->GetHealthPercent() < 20.0f) &&
                CanTryToCastSpell(me, m_spells.hunter.pFeignDeath))
            {
                if (DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK)
                    return;
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {
            if (m_spells.hunter.pWingClip &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
            {
                DoCastSpell(pVictim, m_spells.hunter.pWingClip);
            }

            if (m_spells.hunter.pMongooseBite &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
            {
                DoCastSpell(pVictim, m_spells.hunter.pMongooseBite);
            }

            if (m_spells.hunter.pRaptorStrike &&
                CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
            {
                DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike);
            }
        }
        else
        {
            if (m_spells.hunter.pAspectOfTheHawk &&
                CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
            {
                if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasUnitState(UNIT_STATE_ROOT) &&
            (me->GetCombatDistance(pVictim) < 8.0f) &&
            (GetRole() != ROLE_MELEE_DPS) &&
             me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear();
            if (RunAwayFromTarget(pVictim))
                return;

            // Backing off was refused, which the aggro rule makes ordinary in a corridor, and the
            // mob is inside the range the bow will not fire at. Without this the hunter has nothing
            // left it is willing to do and stands there for the rest of the fight, having stopped
            // itself moving on the line above. Melee is poor for a hunter and beats watching.
            me->Attack(pVictim, true);
            return;
        }

        // Melee is off after a pull, which asks for a shot with Attack's melee flag cleared so the
        // bot does not walk in swinging. Nothing turned it back on afterwards, so a puller that
        // ended up in melee anyway stood and watched. Only claimed at a range where a swing can
        // land, so a hunter shooting from thirty yards is not marked as meleeing something it
        // cannot reach.
        if (pVictim->CanReachWithMeleeAutoAttack(me) && !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            me->Attack(pVictim, true);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Mage()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.mage.pArcaneIntellect, m_spells.mage.pArcaneBrilliance, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.mage.pArcaneIntellect ||
        !me->HasGCD(m_spells.mage.pArcaneIntellect)))
    {
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();
}

void PartyBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.mage.pCombustion &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPyroblast &&
           ((m_spells.mage.pPresenceOfMind && me->HasAura(m_spells.mage.pPresenceOfMind->Id)) ||
            (!pVictim->IsInCombat() && (pVictim->GetMaxHealth() > me->GetMaxHealth()) && (me->GetDistance(pVictim) > 30.0f))) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }
        else if (GetAttackersInRangeCount(10.0f))
        {
            if (m_spells.mage.pManaShield &&
               (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
                CanTryToCastSpell(me, m_spells.mage.pManaShield))
            {
                if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                    return;
            }

            if ((GetRole() != ROLE_MELEE_DPS) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
            {
                if (m_spells.mage.pBlink &&
                    (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                        me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                    CanTryToCastSpell(me, m_spells.mage.pBlink))
                {
                    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                        me->GetMotionMaster()->Clear();

                    if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                        return;
                }

                if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
                {
                    if (m_spells.mage.pFrostNova &&
                       !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
                       !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                        CanTryToCastSpell(me, m_spells.mage.pFrostNova))
                    {
                        DoCastSpell(me, m_spells.mage.pFrostNova);
                    }

                    if (RunAwayFromTarget(pVictim))
                    {
                        me->SetCasterChaseDistance(25.0f);
                        return;
                    }
                }
            }
        }

        if (me->GetEnemyCountInRadiusAround(me, 10.0f) > 1)
        {
            if (m_spells.mage.pConeofCold && !me->IsMoving() &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
            return;

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pBlizzard &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pBlizzard))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pBlizzard) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPolymorph)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (pTarget->GetHealthPercent() > 20.0f &&
                    CanTryToCastSpell(pTarget, m_spells.mage.pPolymorph) &&
                    CanUseCrowdControl(m_spells.mage.pPolymorph, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
            (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPresenceOfMind &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Priest()
{
    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPowerWordFortitude, m_spells.priest.pPrayerofFortitude, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pDivineSpirit, m_spells.priest.pPrayerofSpirit, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection, m_spells.priest.pPrayerofShadowProtection, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {
        m_isBuffing = false;
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly())
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Priest();
}

// Damage a healer can add without costing the group any healing, and the wand it should be firing
// the rest of the time. Reached only after every heal has been considered and declined, so nothing
// here can ever outrank one.
//
// A healer priest with a healthy group did nothing at all before this. The branch that handles
// healing takes every healer, and the branch holding the damage spells and the wand is its else, so
// a priest with nobody to heal fell out of the function having cast nothing: no wand, no dot, no
// filler, for as long as the group stayed healthy. That is most of a trash fight.
//
// Deliberately no AttackStart. Attack sets a target without asking anything of the movement
// generators, so the priest keeps its ground and its distance. AttackStart would send it walking
// into melee, which for a healer is how it dies and how the group loses its healing.
bool PartyBotAI::AddFillerDamage(Unit* pTarget)
{
    if (!pTarget || !IsValidHostileTarget(pTarget) || !me->IsWithinLOSInMap(pTarget))
        return false;

    bool const manaToSpare = me->GetPowerPercent(POWER_MANA) >= PB_FILLER_MANA_PERCENT &&
                            !SelectHealTarget(PB_FILLER_PARTY_HEALTH, PB_FILLER_PARTY_HEALTH);

    if (manaToSpare)
    {
        // Cheapest damage per point of mana in the book and instant, so it costs the group no
        // healing latency at all. Only worth applying once: a dot recast on top of itself throws
        // away every tick it had left.
        if (m_spells.priest.pShadowWordPain &&
           !pTarget->HasAura(m_spells.priest.pShadowWordPain->Id) &&
            CanTryToCastSpell(pTarget, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pTarget, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return true;
        }

        if (m_spells.priest.pSmite &&
            CanTryToCastSpell(pTarget, m_spells.priest.pSmite))
        {
            if (DoCastSpell(pTarget, m_spells.priest.pSmite) == SPELL_CAST_OK)
                return true;
        }
    }

    // The wand asks for no mana whatsoever, so it is not gated on having any to spare: a healer
    // saving every point for the tank should still be firing it. It is an autorepeat, so it sits on
    // its own timer and gives way to a heal the moment one is wanted, then picks itself back up.
    if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
       !me->IsMoving() &&
       !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        SpellEntry const* pWand = sSpellMgr.GetSpellEntry(PB_SPELL_SHOOT_WAND);
        if (pWand && pWand->IsTargetInRange(me, pTarget))
        {
            me->SetFacingToObject(pTarget);
            me->Attack(pTarget, false);
            return me->CastSpell(pTarget, PB_SPELL_SHOOT_WAND, false) == SPELL_CAST_OK;
        }
    }

    return false;
}

// Whether the spell currently going out is one of the fillers above. Cheaper than tracking a flag
// through the cast and it cannot fall out of step with what was actually cast.
bool PartyBotAI::IsCastingFillerDamage() const
{
    Spell const* pSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!pSpell || !pSpell->m_spellInfo)
        return false;

    uint32 const id = pSpell->m_spellInfo->Id;
    return (m_spells.priest.pSmite && id == m_spells.priest.pSmite->Id) ||
           (m_spells.priest.pShadowWordPain && id == m_spells.priest.pShadowWordPain->Id);
}

void PartyBotAI::UpdateInCombatAI_Priest()
{
    // A filler nuke is worth abandoning the instant somebody actually needs healing. Nothing else
    // can be cast while one is going out, so a Smite started against a healthy group and left to
    // run holds up the first heal of a fight that has just turned.
    if (GetRole() == ROLE_HEALER &&
        IsCastingFillerDamage() &&
        SelectHealTarget(PB_FILLER_ABANDON_HEALTH, PB_FILLER_ABANDON_HEALTH))
    {
        me->InterruptNonMeleeSpells(false);
    }

    // Shielding itself was the first thing this function did, on no condition beyond owning the
    // spell, and it returned, so the tick was spent. A priest standing safely at the back at full
    // health put the shield straight back up every time it lapsed, all fight. The capture that
    // prompted this has Power Word: Shield cast eighty six times against sixty actual heals, with
    // the healer at zero mana while an ally sat under half health, and a four second median gap
    // between somebody dropping below seventy percent and any heal arriving.
    //
    // Taking damage is the condition, since absorbing damage is the whole of what the spell does.
    // Health is tested as well as attackers, because a caster being shot at from across the room
    // has nothing in melee with it and is exactly what this is for.
    //
    // And a healer with somebody genuinely dying has better use for the tick even when it is being
    // hit itself, so the heal block further down now outranks this. The thresholds are that
    // block's own, rather than a second opinion about what counts as urgent.
    if (m_spells.priest.pPowerWordShield &&
        (!me->GetAttackers().empty() || me->GetHealthPercent() < 90.0f) &&
        !(GetRole() == ROLE_HEALER && SelectHealTarget(60.0f, 80.0f)) &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;
    }

    if (!me->GetAttackers().empty() &&
        m_role != ROLE_TANK)
    {
        if (m_spells.priest.pFade &&
            CanTryToCastSpell(me, m_spells.priest.pFade))
        {
            if (DoCastSpell(me, m_spells.priest.pFade) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShackleUndead)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.priest.pShackleUndead) &&
                CanUseCrowdControl(m_spells.priest.pShackleUndead, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.priest.pShackleUndead) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    if (GetRole() == ROLE_HEALER || (!me->GetVictim() && me->GetShapeshiftForm() == FORM_NONE))
    {
        // Shield allies being attacked.
        if (m_spells.priest.pPowerWordShield)
        {
            if (Player* pTarget = SelectShieldTarget())
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPowerWordShield))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        // Direct heal more seriously injured.
        if (Unit* pTarget = SelectHealTarget(60.0f, 80.0f))
            if (HealInjuredTargetDirect(pTarget))
                return;

        // Apply HoT aura for small injuries.
        if (Unit* pTarget = SelectPeriodicHealTarget(80.0f, 90.0f))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        if (GetRole() == ROLE_HEALER && FindAndPreHealTarget())
            return;

        // Every heal has now been declined, and for a healer this used to be where the tick ended.
        // A healer holds no victim of its own -- target acquisition in UpdateAI skips the role
        // outright -- so the group's current target has to be looked up rather than read off, and
        // it is only borrowed to shoot at, never chased.
        // The leader is guaranteed by UpdateAI, which drops the bot outright without one, but
        // SelectAttackTarget dereferences it, so it is not left to that guarantee holding.
        if (GetRole() == ROLE_HEALER)
        {
            if (Player* pLeader = GetPartyLeader())
                if (AddFillerDamage(SelectAttackTarget(pLeader)))
                    return;
        }
    }
    else if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pSilence &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;
        }

        // Psychic Scream is the worst of the five for this, because it fears everything in melee at
        // once and it fires on nothing more than being hit. A priest being beaten on has a real
        // problem and this is a real answer to it, but a room full of loose mobs is a worse problem
        // than a dead priest, and the priest usually lives anyway.
        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
           !WouldFearPullExtraEnemies() &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warlock()
{
    if (m_spells.warlock.pDetectInvisibility)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
        {
            if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
            {
                if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            me->ClearTarget();
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        // Not while holding. Sending the pet is the same pull as going itself, taken by proxy.
        if (Pet* pPet = m_holdPosition ? nullptr : me->GetPet())
        {
            if (!pPet->GetVictim())
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();
}

// Whether a damage-over-time spell will live long enough on this target to earn back the cast that
// applies it. On trash it does not: the mob dies inside a couple of direct casts, so the dot is a
// cast that delivered a fraction of its damage and a mob that took longer to die for it. Curse of
// Agony is the plainest case, ramping so that most of its total lands in its final third, but
// Corruption and Immolate lose on a short fight too, and between them they were taking every cast
// the rotation had -- Shadow Bolt sits last, behind all three, and never got a look in.
bool PartyBotAI::IsWorthDotting(Unit const* pVictim) const
{
    // Anything that is not an ordinary mob lives long enough for anything.
    if (Creature const* pCreature = pVictim->ToCreature())
    {
        if (pCreature->GetCreatureInfo()->rank != CREATURE_ELITE_NORMAL)
            return true;
    }

    if (pVictim->GetHealthPercent() < PB_DOT_WORTH_TARGET_HEALTH)
        return false;

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(pVictim, PB_DOT_PACK_RADIUS, enemies);

    // Seeded with the target rather than trusting it to come back in its own neighbour search.
    uint32 engaged = 1;
    uint64 healthPool = pVictim->GetHealth();

    for (Unit const* pEnemy : enemies)
    {
        if (pEnemy == pVictim || !pEnemy->IsInCombat())
            continue;

        ++engaged;
        healthPool += pEnemy->GetHealth();
    }

    if (engaged >= PB_DOT_WORTH_ENEMY_COUNT)
        return true;

    return healthPool > uint64(me->GetMaxHealth() * PB_DOT_WORTH_HEALTH_MULTIPLE);
}

void PartyBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pBanish &&
            me->GetAttackers().size() > 1)
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if ((pAttacker->GetHealth() > me->GetHealth()) &&
                CanTryToCastSpell(pAttacker, m_spells.warlock.pBanish))
            {
                if (DoCastSpell(pAttacker, m_spells.warlock.pBanish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warlock.pRainOfFire &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pRainOfFire))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pRainOfFire) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDemonicSacrifice)
        {
            if (Pet* pPet = me->GetPet())
            {
                if (pPet->IsAlive() &&
                    CanTryToCastSpell(pPet, m_spells.warlock.pDemonicSacrifice))
                {
                    if (DoCastSpell(pPet, m_spells.warlock.pDemonicSacrifice) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.warlock.pImmolate &&
            IsWorthDotting(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCorruption &&
            IsWorthDotting(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            IsWorthDotting(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pFear &&
            pVictim->GetVictim() == me &&
           !WouldFearPullExtraEnemies() &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pFear))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pFear) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pCurseofAgony &&
            IsWorthDotting(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofAgony))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCurseofAgony) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.warlock.pHowlofTerror &&
            GetAttackersInRangeCount(10.0f) > 1 &&
           !WouldFearPullExtraEnemies() &&
            CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror))
        {
            if (DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(PB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, PB_SPELL_SHOOT_WAND, false);
    }
}

void PartyBotAI::UpdateOutOfCombatAI_Warrior()
{
    // A tank belongs in Defensive Stance and is left there, rather than being walked out to
    // Battle Stance for a Charge and walked back the moment the fight starts.
    //
    // Changing stance sets rage to zero. Not reduces, zero: SpellAuras.cpp caps it at the rank of
    // Tactical Mastery held, which a levelling bot has none of. So the old sequence cost the tank
    // its rage twice per pull, and the second wipe landed on the Charge rage the first wipe had
    // been paid for. The capture that prompted this has thirty one Battle Stances, thirty one
    // Defensive Stances and twenty nine Charges in half an hour, which is a tank starting every
    // single fight of a dungeon on nothing.
    //
    // Losing the gap closer costs a tank bot very little, because it is following the party rather
    // than initiating, and Bloodrage at the top of the tank rotation is the real opener. A warrior
    // here to do damage keeps Battle Stance and keeps Charge, since Battle Stance is where it
    // fights anyway and there is no swap back to pay for.
    bool const tanking = GetRole() == ROLE_TANK;

    if (tanking && m_spells.warrior.pDefensiveStance)
    {
        if (me->GetShapeshiftForm() != FORM_DEFENSIVESTANCE &&
            CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
        {
            if (DoCastSpell(me, m_spells.warrior.pDefensiveStance) == SPELL_CAST_OK)
                return;
        }
    }
    else if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);
        else if (m_spells.warrior.pBloodrage &&
            (me->GetPower(POWER_RAGE) < 100) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    // Charge is a gap closer, so a held warrior would leave the spot it was told to wait on and
    // arrive in the pack alone. The hold acquires a target without approaching it deliberately, and
    // this is the one rotation step that turns having a target into crossing the room.
    if (m_holdPosition)
        return;

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::ShouldTauntTarget(Unit const* pVictim) const
{
    Unit const* pHolder = pVictim->GetVictim();
    if (!pHolder || pHolder == me)
        return false;

    // Only take it back off someone who is not supposed to have it. Pulling a target off the
    // other tank is how two tanks spend an encounter trading it between them, and a taunt spent
    // there is a taunt not available ten seconds later when a damage dealer actually needs
    // saving.
    Player const* pPlayer = pHolder->ToPlayer();
    if (!pPlayer || !me->IsInSameGroupWith(pPlayer))
        return false;

    PlayerBotEntry const* pEntry = pPlayer->GetSession() ? pPlayer->GetSession()->GetBot() : nullptr;
    if (PartyBotAI const* pAI = pEntry ? dynamic_cast<PartyBotAI const*>(pEntry->ai.get()) : nullptr)
        if (pAI->m_role == ROLE_TANK)
            return false;

    return true;
}

// The rage floors above are what a level sixty tank in a raid can hold, and a levelling one
// cannot come near them. Rage is earned as a share of damage dealt and taken weighed against the
// character's own level, and the pool a low level warrior can build inside one fight is a
// fraction of the same numbers: the capture that prompted this has a level fifteen tank spend a
// whole dungeon under thirty rage, so Shield Block never fired once and the surplus dump never
// fired at all, which are two thirds of what the rotation is.
//
// Scaled rather than lowered, so that max level is left exactly as it was measured, and scaled
// off a floor rather than straight off the level so the thresholds still mean something at
// fifteen instead of collapsing to nearly zero and handing back the pooling they exist to do.
uint32 PartyBotAI::ScaleTankRage(uint32 rage) const
{
    uint32 const maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
    if (!maxLevel || me->GetLevel() >= maxLevel)
        return rage;

    return uint32(rage * (0.4f + 0.6f * float(me->GetLevel()) / float(maxLevel)));
}

void PartyBotAI::UpdateInCombatAI_WarriorTank(Unit* pVictim)
{
    // Defensive Stance first and before anything else is attempted, because most of what
    // follows cannot be cast outside it: Taunt, Revenge and Shield Block are all stance locked,
    // and the stance itself is worth a third again on every point of threat made in it. A
    // warrior that opens with Charge is in Battle Stance when the fight starts and has to be
    // moved across at once rather than eleven checks later, which is where the shared list did
    // it and is why the opening was being fought in the wrong stance.
    if (m_spells.warrior.pDefensiveStance &&
        me->GetShapeshiftForm() != FORM_DEFENSIVESTANCE &&
        CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pDefensiveStance) == SPELL_CAST_OK)
            return;
    }

    // Staying alive outranks holding the target, since a dead tank holds nothing.
    if (me->GetHealthPercent() < 35.0f)
    {
        if (m_spells.warrior.pShieldWall && IsWearingShield(me) &&
            CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
        {
            if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pLastStand &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;
        }
    }

    // Everything from here to the global cooldown block is off the global cooldown, which the
    // spell data is explicit about: Taunt, Bloodrage, Shield Block, Heroic Strike and Cleave all
    // carry StartRecoveryTime 0. In the game they are therefore not alternatives to the ability
    // that fills the cooldown, and this block used to fall through on that reasoning so a free
    // ability was never spent in place of a cast of threat.
    //
    // The server does not allow it. Whatever was cast first still holds the caster's current-spell
    // slot when the next cast is checked in the same tick, and that check refuses it with
    // SPELL_FAILED_SPELL_IN_PROGRESS no matter what the cooldowns say. Falling through bought no
    // second ability, only a rejected attempt: thirty five minutes of one Wailing Caverns tank
    // produced sixty nine of them, every one immediately after a cast that had just succeeded, and
    // every one landing anyway on the following tick. So these return, and the second ability
    // arrives a tick later exactly as it already did underneath the noise.
    //
    // What that costs is ordering. Because only one cast per tick survives, position in this list
    // now decides which ability wins the tick, and the free ones are listed first. They are gated
    // tightly enough to stay out of the way -- Bloodrage wants low rage and has a minute cooldown,
    // Shield Block wants a rage floor and has five seconds, the dump wants a surplus -- but a
    // Shield Block does now delay a threat cast by a tick where before it merely failed to add one.

    // Taunt is not here. It is handled once in UpdateInCombatAI for every tank class off the
    // list of everything carrying SPELL_EFFECT_ATTACK_ME, which covers a druid's Growl as well
    // as this, and a second copy here would only be a second way to get the guard wrong.

    // Rage is the whole constraint on a tank's opening. It starts a fight with almost none,
    // earns it only by being hit, and everything that makes threat costs some, so the first
    // seconds are spent waiting unless this is used. It is free and it was previously cast only
    // out of combat, and then only when Battle Shout happened to be up already, which is to say
    // almost never.
    if (m_spells.warrior.pBloodrage &&
        me->GetPower(POWER_RAGE) < ScaleTankRage(PB_TANK_RAGE_LOW) &&
        CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
    {
        if (DoCastSpell(me, m_spells.warrior.pBloodrage) == SPELL_CAST_OK)
            return;
    }

    // Mitigation, and also the supply of Revenge below, which only unlocks off a block, dodge or
    // parry: a guaranteed block is the only one of those a tank can arrange for itself. Held
    // above a rage floor so that the ten it costs is never the ten Shield Slam needed.
    if (m_spells.warrior.pShieldBlock && IsWearingShield(me) &&
       !me->GetAttackers().empty() &&
        me->GetPower(POWER_RAGE) >= ScaleTankRage(PB_TANK_RAGE_BLOCK) &&
        CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
    {
        if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
            return;
    }

    // Spend the surplus. These land on the next swing instead of costing a cast, so the only
    // thing they compete for is rage, and rage a tank is sitting on is threat it has decided not
    // to make. The floor is set high enough that what is spent here is genuinely spare. The test
    // for this used to be inverted, dumping only below thirty rage and pooling in silence above
    // it, so a tank being hit hard enough to be flush was also the one doing least with it.
    if (me->GetPower(POWER_RAGE) >= ScaleTankRage(PB_TANK_RAGE_DUMP))
    {
        if (m_spells.warrior.pCleave && me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCleave))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCleave) == SPELL_CAST_OK)
                return;
        }
        else if (m_spells.warrior.pHeroicStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                return;
        }
    }

    // The global cooldown, best threat per rage first. Shield Slam leads it and works in any
    // stance despite reading like a Defensive ability; Revenge is nearly free and its own
    // cooldown means it is never what gets crowded out.
    if (m_spells.warrior.pShieldSlam && IsWearingShield(me) &&
        CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
    {
        if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pRevenge &&
        CanTryToCastSpell(pVictim, m_spells.warrior.pRevenge))
    {
        if (DoCastSpell(pVictim, m_spells.warrior.pRevenge) == SPELL_CAST_OK)
            return;
    }

    // Both shouts sit above Sunder Armor rather than below it, and only because Sunder has no
    // cooldown and never fails. Anything placed under it is unreachable, which is what happened
    // to Demoralizing Shout in the shared list. Each is held behind its own aura, so being
    // higher costs a cast only on the tick the effect is actually missing.
    //
    // Both are now also held behind a rage floor, which is what being above Sunder has to be paid
    // for. Sitting there unconditionally, the pair outcast the filler they sit on top of: the
    // capture that prompted this has Demoralizing Shout at sixty six casts against Sunder Armor's
    // sixty, because a tank swapping targets around a pack meets a fresh victim without the debuff
    // every few seconds and re-buys it every time. Ten rage for a debuff is a fair trade out of a
    // surplus and a bad one out of the only ten rage a level fifteen tank has, where the same ten
    // is a Sunder and the threat that comes with it. Reordering them under Sunder instead would
    // just make them dead code, for the reason above.
    bool const rageToSpare = me->GetPower(POWER_RAGE) >= ScaleTankRage(PB_TANK_RAGE_BLOCK);

    if (rageToSpare)
    {
        if (m_spells.warrior.pDemoralizingShout &&
           !pVictim->HasAura(m_spells.warrior.pDemoralizingShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pDemoralizingShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pDemoralizingShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pBattleShout &&
           !me->HasAura(m_spells.warrior.pBattleShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }
    }

    // The filler, and the floor of the list: no cooldown, so it runs whenever the two above are
    // spent. Re-applying it at five stacks still makes its full threat, and it leaves the armor
    // debuff that the rest of the raid's damage is scaling off.
    if (m_spells.warrior.pSunderArmor &&
        CanTryToCastSpell(pVictim, m_spells.warrior.pSunderArmor))
    {
        if (DoCastSpell(pVictim, m_spells.warrior.pSunderArmor) == SPELL_CAST_OK)
            return;
    }

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
       !me->CanReachWithMeleeAutoAttack(pVictim))
        BeginChasing(pVictim);
}

void PartyBotAI::UpdateInCombatAI_Warrior()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;
            }
        }

        // A tank wants a different list in a different order from a warrior who is there to do
        // damage, and sharing one costs it most of what it has: the order below is priority,
        // since the first thing that casts returns, and the shared version spends the early
        // slots on Execute, Overpower and Rend while Sunder Armor waits behind them.
        if (m_role == ROLE_TANK)
        {
            UpdateInCombatAI_WarriorTank(pVictim);
            return;
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {
            if (!me->GetAttackers().empty())
            {
                if (m_spells.warrior.pShieldBlock &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.warrior.pShieldWall &&
                   (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
            {
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pThunderClap &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pThunderClap))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pThunderClap) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pSunderArmor &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pSunderArmor))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pSunderArmor) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pHamstring &&
            pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRend &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
           !WouldFearPullExtraEnemies() &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pRetaliation &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pSweepingStrikes &&
            CanTryToCastSpell(me, m_spells.warrior.pSweepingStrikes) &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2))
        {
            if (DoCastSpell(me, m_spells.warrior.pSweepingStrikes) == SPELL_CAST_OK)
                return;
        }

        if (m_role != ROLE_TANK &&
           (me->GetHealthPercent() > 60.0f) && (pVictim->GetHealthPercent() > 40.0f) &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;
        }

        if ((me->GetHealthPercent() < 20.0f) ||
            (m_role == ROLE_TANK && pVictim->GetLevel() >= me->GetLevel()))
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else
        {
            if (m_spells.warrior.pBerserkerStance &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
            {
                DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }

        // Another gap closer, so another way out of a hold. See the Charge gate out of combat.
        if (!m_holdPosition &&
            m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pWhirlwind &&
            CanTryToCastSpell(me, m_spells.warrior.pWhirlwind))
        {
            if (DoCastSpell(me, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pDemoralizingShout &&
            m_role == ROLE_TANK &&
            CanTryToCastSpell(me, m_spells.warrior.pDemoralizingShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pDemoralizingShout) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            BeginChasing(pVictim);
        }

        if (me->GetPower(POWER_RAGE) > 300)
        {
            if (m_spells.warrior.pCleave && me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1)
            {
                if (CanTryToCastSpell(pVictim, m_spells.warrior.pCleave))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pCleave) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.warrior.pHeroicStrike &&
                    CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
                {
                    if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                        return;
                }
            }
        }
    }
    else // no victim
    {
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::ShouldEnterStealth() const
{
    if (me->IsMounted())
        return false;

    if (me->GetVictim() || me->InBattleGround() || me->IsFFAPvP())
        return true;

    if (me->GetHealthPercent() < 10.0f)
        return true;

    if (Player* pLeader = GetPartyLeader())
    {
        if (pLeader->IsDead() || pLeader->IsFeigningDeathSuccessfully() ||
            pLeader->HasAuraType(SPELL_AURA_MOD_STEALTH) ||
            pLeader->HasAuraType(SPELL_AURA_MOD_INVISIBILITY))
            return true;
    }

    return false;
}

bool PartyBotAI::EnterStealthIfNeeded(SpellEntry const* pStealthSpell)
{
    if (pStealthSpell)
    {
        bool const shouldStealth = ShouldEnterStealth();

        if (me->HasAura(pStealthSpell->Id))
        {
            if (!shouldStealth)
                me->RemoveAurasDueToSpellByCancel(pStealthSpell->Id);
        }
        else
        {
            if (shouldStealth &&
                CanTryToCastSpell(me, pStealthSpell) &&
                DoCastSpell(me, pStealthSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;
    }

    if (EnterStealthIfNeeded(m_spells.rogue.pStealth))
        return;

    if (me->GetVictim())
        UpdateInCombatAI_Rogue();
}

void PartyBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else
        {
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {
                        if (RunAwayFromTarget(pVictim))
                            return;
                    }
                }
            }
        }

        if (me->GetComboPoints() > 4)
        {
            std::vector<SpellEntry const*> vSpells;

            // Give priority to Slice and Dice over other finishing moves.
            if (m_spells.rogue.pSliceAndDice &&
               !me->HasAura(m_spells.rogue.pSliceAndDice->Id) &&
                pVictim->GetHealthPercent() > 10.0f)
                vSpells.push_back(m_spells.rogue.pSliceAndDice);
            else
            {
                if (m_spells.rogue.pEviscerate)
                    vSpells.push_back(m_spells.rogue.pEviscerate);
                if (m_spells.rogue.pKidneyShot && !pVictim->IsImmuneToMechanic(MECHANIC_STUN))
                    vSpells.push_back(m_spells.rogue.pKidneyShot);
                if (m_spells.rogue.pExposeArmor)
                    vSpells.push_back(m_spells.rogue.pExposeArmor);
                if (m_spells.rogue.pRupture)
                    vSpells.push_back(m_spells.rogue.pRupture);
            }

            if (!vSpells.empty())
            {
                SpellEntry const* pComboSpell = SelectRandomContainerElement(vSpells);
                if (CanTryToCastSpell(pVictim, pComboSpell))
                {
                    if (DoCastSpell(pVictim, pComboSpell) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (m_spells.rogue.pBlind)
        {
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind) &&
                    CanUseCrowdControl(m_spells.rogue.pBlind, pTarget))
                {
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {
            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pColdBlood &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;
        }
    }
}

bool PartyBotAI::EnterCombatDruidForm()
{
    if (m_spells.druid.pCatForm &&
        GetRole() == ROLE_MELEE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pCatForm))
    {
        if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pBearForm &&
       (m_role == ROLE_TANK || GetRole() == ROLE_MELEE_DPS) &&
        CanTryToCastSpell(me, m_spells.druid.pBearForm))
    {
        if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
            return true;
    }

    if (m_spells.druid.pMoonkinForm &&
        GetRole() == ROLE_RANGE_DPS &&
        CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
    {
        if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

void PartyBotAI::UpdateOutOfCombatAI_Druid()
{
    // Make sure bot leaves combat form if his role is changed to healer.
    if (GetRole() == ROLE_HEALER && me->GetShapeshiftForm() != FORM_NONE &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return;
    }

    SpellEntry const* pBuffSpell = nullptr;
    if (Player* pTarget = SelectBuffTarget(m_spells.druid.pMarkoftheWild, m_spells.druid.pGiftoftheWild, pBuffSpell))
    {
        if (CanTryToCastSpell(pTarget, pBuffSpell))
        {
            if (DoCastSpell(pTarget, pBuffSpell) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                me->ClearTarget();
                return;
            }
        }
    }

    if (m_spells.druid.pThorns)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
        {
            if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
            {
                if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    me->ClearTarget();
                    return;
                }
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (EnterCombatDruidForm())
            return;

        if ((me->GetPowerPercent(POWER_MANA) > 80.0f) &&
            FindAndHealInjuredAlly())
            return;
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {
        if (EnterStealthIfNeeded(m_spells.druid.pProwl))
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Druid();
}

void PartyBotAI::UpdateInCombatAI_Druid()
{
    ShapeshiftForm const form = me->GetShapeshiftForm();

    if (m_spells.druid.pBarkskin &&
        (form == FORM_NONE || form == FORM_MOONKIN) &&
        (me->GetHealthPercent() < 50.0f) &&
        CanTryToCastSpell(me, m_spells.druid.pBarkskin))
    {
        if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
            return;
    }

    // The only resurrection in the game that can be cast during a fight, and until now the one
    // spell in the druid's list that was read at spawn and never cast. Anyone else who dies
    // mid-encounter is out of it until the pull ends, so this is the difference between losing
    // a healer and losing the attempt, and it is checked before the rotation because a
    // cooldown measured in minutes cannot wait for a quiet moment the way a heal can.
    //
    // Rebirth cannot be cast in any form, and a druid that spends the fight in one is the
    // common case rather than the exception, so the form goes. It costs a global cooldown and
    // is paid back by EnterCombatDruidForm further down on a later tick. Not for a tank: a bear
    // that stands up in the middle of a pull hands the boss to whoever is next on the list.
    if (m_spells.druid.pRebirth && m_role != ROLE_TANK)
    {
        if (Player* pTarget = SelectResurrectionTarget(m_spells.druid.pRebirth))
        {
            if (form != FORM_NONE && me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
            {
                me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
                return;
            }

            if (CanTryToCastSpell(pTarget, m_spells.druid.pRebirth))
                if (DoCastSpell(pTarget, m_spells.druid.pRebirth) == SPELL_CAST_OK)
                    return;
        }
    }

    if (form == FORM_NONE)
    {
        if (m_spells.druid.pHibernate &&
            m_role != ROLE_TANK &&
            !me->GetAttackers().empty())
        {
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate) &&
                CanUseCrowdControl(m_spells.druid.pHibernate, pAttacker))
            {
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;
            }
        }

        // Prioritize applying HoTs.
        if (Unit* pTarget = SelectPeriodicHealTarget(80.0f, 90.0f))
            if (HealInjuredTargetPeriodic(pTarget))
                return;

        // Direct heal.
        if (Unit* pTarget = SelectHealTarget(60.0f, 70.0f))
            if (HealInjuredTargetDirect(pTarget))
                return;

        if (m_spells.druid.pInnervate &&
           (me->GetHealthPercent() > 40.0f) &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pInnervate))
        {
            if (DoCastSpell(me, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;
        }

        if (GetRole() == ROLE_HEALER && FindAndPreHealTarget())
            return;

        if (EnterCombatDruidForm())
            return;
    }

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return;

    if (form != FORM_NONE &&
        me->HasUnitState(UNIT_STATE_ROOT) &&
        me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) &&
        (m_role != ROLE_TANK || !me->CanReachWithMeleeAutoAttack(pVictim)))
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

    if (GetRole() == ROLE_HEALER)
        return;

    switch (form)
    {
        case FORM_CAT:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
            {
                if (m_spells.druid.pPounce &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pRavage &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                        return;
                }
                if (m_spells.druid.pTigersFury &&
                    CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                {
                    if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                        return;
                }
                return;
            }

            if (m_spells.druid.pCower &&
                GetAttackersInRangeCount(8.0f))
            {
                Unit* pAttacker = *me->GetAttackers().begin();
                if (CanTryToCastSpell(me, m_spells.druid.pCower))
                {
                    if (DoCastSpell(me, m_spells.druid.pCower) == SPELL_CAST_OK)
                        return;
                }
            }

            if (me->GetComboPoints() > 4)
            {
                if (m_spells.druid.pFerociousBite &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pRip &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                        return;
                }
            }

            if (!me->CanReachWithMeleeAutoAttack(pVictim))
            {
                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pDash &&
                    pVictim->IsMoving() &&
                    CanTryToCastSpell(me, m_spells.druid.pDash))
                {
                    if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pShred &&
                CanTryToCastSpell(pVictim, m_spells.druid.pShred))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pRake &&
                CanTryToCastSpell(pVictim, m_spells.druid.pRake))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pClaw &&
                CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
        case FORM_BEAR:
        case FORM_DIREBEAR:
        {
            if (me->HasDistanceCasterMovement())
                me->SetCasterChaseDistance(0.0f);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                BeginChasing(pVictim);
            }

            // The third gap closer, and the last way a held bot could cross the room. See the
            // Charge gate in the warrior's out of combat rotation.
            if (!m_holdPosition &&
                m_spells.druid.pFeralCharge &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pBash &&
                CanTryToCastSpell(pVictim, m_spells.druid.pBash))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFrenziedRegeneration &&
                (me->GetHealthPercent() < 30.0f) &&
                CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
            {
                if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pFaerieFireFeral &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                    return;
            }

            if ((me->GetPower(POWER_RAGE) > 800) ||
                (GetAttackersInRangeCount(10.0f) > 1))
            {
                if (m_spells.druid.pDemoralizingRoar &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pSwipe &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;
                }
            }

            if (m_spells.druid.pMaul &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                    return;
            }
            break;
        }
        case FORM_NONE:
        case FORM_MOONKIN:
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                me->GetDistance(pVictim) > 30.0f)
            {
                BeginChasing(pVictim);
            }
            else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                    (pVictim->GetVictim() == me) &&
                    !me->HasUnitState(UNIT_STATE_ROOT) &&
                    (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
            {
                if (m_spells.druid.pEntanglingRoots &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                        return;
                }
                me->SetCasterChaseDistance(25.0f);
                if (RunAwayFromTarget(pVictim))
                    return;
            }

            if (m_spells.druid.pFaerieFire &&
               (pVictim->GetClass() == CLASS_ROGUE) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pInsectSwarm &&
                CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                    return;
            }

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                return;

            if (m_spells.druid.pHurricane &&
               (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pHurricane))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pHurricane) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pMoonfire &&
                CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pStarfire &&
               (pVictim->GetHealthPercent() > 50.0f) &&
                CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pWrath &&
                CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
            {
                if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                    return;
            }

            break;
        }
    }
}
