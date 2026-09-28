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

#ifndef MANGOS_DUNGEONTACTICS_H
#define MANGOS_DUNGEONTACTICS_H

#include "Platform/Define.h"
#include <vector>

// What a party bot is told about the instance it is standing in.
//
// Everything here is a judgement that cannot be reached from the creature's own data. A bot can see
// that a mob is casting, that it is elite, and how far away it is; it cannot see that this
// particular mob heals itself back to full unless the group stops it, that the room it is in is a
// series of blind corners, or that the pull it is about to take arrives in waves. Those are facts
// about a dungeon, they differ from one dungeon to the next, and they are the ones worth writing
// down per map rather than trying to infer.
//
// Kept deliberately small. Each field earns its place by changing a decision the shared AI already
// makes, so that an instance with no entry here behaves exactly as it did before this existed, and
// adding a dungeon is a table entry rather than a branch in the rotations.

// A creature the group has to stand somewhere particular for, rather than one it merely has to
// out-damage.
struct DungeonCreatureTactic
{
    uint32 creatureEntry;
    // How far a ranged bot or a healer keeps off this creature, overriding the standoff the caster
    // chase would otherwise pick. Zero leaves that choice alone.
    //
    // Only ever used to push a bot further out, never to pull one in: a standoff shorter than the
    // default would be a bot walking towards something, and nothing in this table is worth that.
    float rangedStandoff;

    // One spell of this creature's that a bot aimed at is better off stepping out of sight of than
    // eating. Zero, which is every creature without an entry here, leaves the bot standing still.
    //
    // Has to be named rather than inferred, because the three things that qualify a spell are not
    // in its own data. It must be aimed at a single unit, since one bot moving cannot take a party
    // wide spell away from the party. It must have enough cast time left to notice it, walk out of
    // sight and be back before the next one. And the fight has to be one where walking out of the
    // caster's sight is possible at all, which is a fact about the room and not about the spell.
    uint32 breakSightSpellId;

    // Where melee stands to fight this creature, and how far off it they may drift.
    //
    // For creatures that move the group rather than damage it. Gahz'rilla's Slam is a knockback
    // centred on himself on a three second repeat, so a melee bot spends the fight being thrown
    // and then walking back in from wherever it landed -- and the walk back is the whole cost,
    // because it is time not spent swinging and ground not chosen. The answer players reached for
    // in this fight is to put a wall at their back so the throw goes nowhere, and that is a fact
    // about one particular patch of floor: it cannot be derived from the creature, only measured
    // and written down.
    //
    // A radius of zero, which is every creature without an entry, leaves positioning alone.
    float anchorX;
    float anchorY;
    float anchorZ;
    float anchorRadius;

    // The summons this creature calls in, and the point in its own health bar past which killing
    // them stops being worth the time.
    //
    // The usual rule -- adds first, always -- is written for bosses that cannot undo the delay.
    // A boss that heals itself can. Antu'sul sends one Servant at seventy five percent, when he
    // has no heal available and the pause is free, and two more at twenty five, by which point
    // Healing Wave of Antu'sul is on a twelve second repeat and every second spent off him is
    // healing that has to be done again. Turning to kill both adds there can hand back more
    // health than the adds would ever have dealt in damage.
    //
    // Only the summons listed here are suspended. A healing totem is never on this list, because
    // killing one does not cost time, it gives time back.
    float burnBelowPercent;
    std::vector<uint32> summonEntries;

    // The share of its own health below which this creature starts healing itself, and so the
    // point from which a bot holding an interrupt should stop spending its global cooldown on
    // filler and keep the tick free.
    //
    // A fact about one creature's script and nothing else. Antu'sul opens Flash Heal at sixty
    // percent and Healing Wave of Antu'sul at thirty; a boss with no heal at all has no such
    // point, and a boss that heals from the pull has it at a hundred. There is no global number
    // that is right for all three, which is exactly why this stopped being a global: the shared
    // constant was set to sixty five because that suited Antu'sul, which made every other
    // encounter in the game hold its rotation from sixty five percent for no reason.
    //
    // Zero, which is every creature without an entry, means the reserve never engages for it.
    float healsBelowPercent;
};

// A patch of ground worth holding, and the bounds of it.
struct DungeonHoldLine
{
    // The spot being held: where the group stands to fight the event.
    float x;
    float y;
    float z;

    // How far from it a bot may take a fight. Generous enough to cover the whole of the landing
    // the group is standing on, since the point is to stop a bot leaving the ground, not to keep
    // it on one flagstone.
    float radius;

    // And never below this height, whatever the radius allows. The radius alone cannot describe a
    // staircase: the foot of Zul'Farrak's is well within any radius that covers the top of it, and
    // it is thirty yards further down.
    float minZ;

    // How far outside the zone a bot that has been pushed out of it will walk back from.
    //
    // Refusing to walk off held ground is only half the rule, and the missing half is the one
    // Zul'Farrak punishes. Shadowpriest Sezz'ziz arrives in the third wave with Psychic Scream,
    // and a fear at the top of those stairs throws bots *down* them -- at which point they are
    // outside every hold line, the refusal stops applying to them by design, and they fight out
    // the rest of the event at the bottom among the trolls that have not been released. The same
    // goes for a knockback, or for a bot that was following the leader when the fight started.
    //
    // Generous, because the distance a fear covers is not small and the ground it lands on is the
    // worst in the instance. Zero, which is any zone without one, leaves a bot that has left the
    // zone alone -- the rule then binds only what is already standing on the spot.
    float recoverRadius;
};

struct DungeonTactics
{
    uint32 mapId;
    char const* name;

    // Whether the group's sleeps, fears and charms are frequent enough that a shaman should give up
    // its earth totem slot to Tremor Totem for the whole instance.
    //
    // Tremor is otherwise last in the earth list, behind Strength of Earth and Stoneskin, and
    // correctly so: it does nothing at all in a dungeon that never crowd-controls anybody, and the
    // two ahead of it always do something. This flips that ordering where the opposite is true.
    bool wantsTremorTotem;

    // An escort the group has to keep alive, and how far from it the bots treat an attack on it as
    // an attack on the party.
    //
    // A bot's threat rules are all built around the group: it defends party members, and an escort
    // NPC is not one. Left alone the group watches the thing it is supposed to be protecting get
    // eaten by adds that never touched a player.
    //
    // A list rather than one entry, because an escort is not always one NPC. Zul'Farrak hands
    // the group five at once -- Bly and his four -- and they fight alongside it for the whole of
    // the pyramid event; one of them, Weegli, is the only thing that opens the end door.
    // **The order is significant**, and is the order the healer prefers them in when more than
    // one is hurt at once. Zul'Farrak is why: the group is handed five at once and they are not
    // interchangeable. Weegli Blastfuse is the only one that opens the end door, so his death is
    // the one that stops the instance being finished at all, and a flat list left him to it --
    // one measured run spent twenty nine of its forty six escort heals on Oro Eyegouge and
    // reached Weegli twice, at fourteen and nineteen percent health, because the selector only
    // ever looked at whoever was lowest that instant.
    //
    // A tie-break and not an override. See CB_ESCORT_RANK_BONUS: being first on this list is
    // worth a fixed number of health percentage points, so a scratched Weegli still loses to an
    // Oro who is actually dying.
    std::vector<uint32> escortNpcEntries;
    float escortGuardRadius;

    // Creature entries worth killing ahead of whatever else is standing next to them, when the
    // group's own target selection is otherwise free to pick either.
    //
    // This is not a taunt list and not a crowd-control list. It only breaks ties: a bot with no
    // marked focus and no attacker of its own prefers one of these to an arbitrary member of the
    // pack.
    //
    // **The order is significant**, and is the kill order within the list. Two entries standing in
    // the same room is the normal case rather than the exception -- Zul'Farrak's third pyramid
    // wave arrives as Nekrum Gutchewer and Shadowpriest Sezz'ziz together, and its last fight is
    // Bly with Oro and Murta beside him -- and a flat list left the group to pick between them on
    // health and guid, which is to say at random. Earlier in the list wins.
    //
    // Only ever compared between two entries that are both on the list. A creature that is not on
    // it has no rank and is not ordered against one that has.
    std::vector<uint32> focusFirst;

    std::vector<DungeonCreatureTactic> creatures;

    // Ground a bot will not fight its way off.
    //
    // The rule every gauntlet event in the game is built on and the one thing a bot cannot work
    // out for itself: that the fight happens *here*, at a choke, and that following a target off
    // the spot loses the event however well the chase itself goes. Zul'Farrak's pyramid is the
    // clearest case -- fifty five trolls spawn at the foot of the stairs and are released up them
    // a handful at a time, so a bot that walks down to meet one arrives among the forty that have
    // not been released yet.
    //
    // Self-activating by position rather than driven by the instance script's event state: a bot
    // standing inside the zone holds it, a bot outside is unaffected. That keeps the table free of
    // script enums, and means the rule switches itself off the moment the leader walks the group
    // out -- because this constrains combat movement only, never the follow.
    std::vector<DungeonHoldLine> holdLines;

    // Creatures the group is to leave alone: not fetched off a caster, not shouted onto the tank,
    // not chased. Deliberately separate from focusFirst, which only breaks ties between things the
    // group was going to fight anyway -- this says do not engage at all.
    //
    // Zul'Farrak's Sul'lithuz Broodlings are the case it exists for. They are standing in Antu'sul's
    // room before the pull rather than summoned by him, they are three levels under the group, and
    // the player handles them. Left in the tank's peel list they cost the whole encounter: one
    // attempt has the tank open the fight forty six yards from the boss, spend five seconds
    // fetching a Broodling off the healer and an AoE taunt collecting the rest, and arrive at
    // Antu'sul with fourteen threat -- by which time the healer, already healing, is top of the
    // table at five hundred and four and stays there. The boss then spends the next sixty seconds
    // parked on the priest.
    std::vector<uint32> ignoreEntries;

    // Ground that hurts whatever is standing on it, named by the spell that laid it down.
    //
    // The one thing in a dungeon a bot has no way at all to see. A persistent area aura is a
    // DynamicObject sitting on the floor with a radius, and from inside the bot it is
    // indistinguishable from being damaged by something invisible: there is no attacker, no cast
    // to interrupt, no threat entry, and every positioning rule in the AI is written against a
    // creature. So the bot stands in it and dies, and the log says only that its health went
    // down.
    //
    // Maraudon is the instance that forced this. Noxious Cloud (21070) is five yards across and
    // deals a hundred and fifty nature damage a second for twenty seconds -- three thousand
    // health, against a level forty seven clothie's sixteen hundred -- and there are ninety nine
    // spawns between them of the two things that drop it: Noxious Slime leaves one behind when it
    // dies, which is to say directly under whoever killed it, and Creeping Sludge drops one every
    // twenty two to twenty six seconds during the fight.
    //
    // Named per instance rather than inferred from the spell, because "damaging area aura" also
    // describes a Blizzard the party's own mage is standing in and every consecration and every
    // totem pulse. The thing that makes a cloud worth walking out of is the arithmetic above,
    // which is a fact about one spell, so it is written down for the one spell.
    //
    // The radius is read off the DynamicObject itself and not stored here: the object knows how
    // big it is, and a second copy of that number in this table is a second thing to get wrong.
    std::vector<uint32> groundHazardSpellIds;

    float GetRangedStandoff(uint32 creatureEntry) const;

    // The anchor for this creature, if it has one. False leaves the caller's positioning alone.
    bool GetFightAnchor(uint32 creatureEntry, float& x, float& y, float& z, float& radius) const;
    bool IsFocusFirst(uint32 creatureEntry) const;

    // This creature's place in the kill order, counting from one, or zero for one that is not on
    // the focusFirst list at all. Lower is killed first.
    uint32 GetFocusRank(uint32 creatureEntry) const;

    uint32 GetBreakSightSpell(uint32 creatureEntry) const;
    bool IsEscortNpc(uint32 creatureEntry) const;

    // This escort's place in the order above, counting from one, or zero for a creature that is
    // not an escort here.
    uint32 GetEscortRank(uint32 creatureEntry) const;
    uint32 GetBurnGateFor(uint32 summonEntry, float& belowPercent) const;

    // The health share below which this creature's heals switch on, or zero for one that has
    // none worth reserving a global cooldown against.
    float GetHealWatchPercent(uint32 creatureEntry) const;
    bool IsSummonedAdd(uint32 creatureEntry) const;
    bool IsIgnoredByParty(uint32 creatureEntry) const;

    // Whether a patch of ground laid down by this spell is one to walk out of.
    bool IsGroundHazard(uint32 spellId) const;

    // The zone this position is inside, or nullptr if it is in none of them.
    DungeonHoldLine const* GetHoldLineAt(float x, float y, float z) const;

    // The zone this position is near enough to have been pushed out of, or nullptr. Answers
    // nullptr for a position that is already inside a zone, so the caller never walks a bot that
    // is where it should be.
    DungeonHoldLine const* GetHoldLineToRecover(float x, float y, float z) const;
};

// The tactics for a map, or nullptr for the great majority of maps that have none. Callers are
// expected to hold the answer for as long as they stay on the map: the table is static data and the
// pointers into it outlive any bot.
DungeonTactics const* GetDungeonTactics(uint32 mapId);

#endif
