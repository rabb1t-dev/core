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
    uint32 escortNpcEntry;
    float escortGuardRadius;

    // Creature entries worth killing ahead of whatever else is standing next to them, when the
    // group's own target selection is otherwise free to pick either.
    //
    // This is not a taunt list and not a crowd-control list. It only breaks ties: a bot with no
    // marked focus and no attacker of its own prefers one of these to an arbitrary member of the
    // pack.
    std::vector<uint32> focusFirst;

    std::vector<DungeonCreatureTactic> creatures;

    float GetRangedStandoff(uint32 creatureEntry) const;
    bool IsFocusFirst(uint32 creatureEntry) const;
};

// The tactics for a map, or nullptr for the great majority of maps that have none. Callers are
// expected to hold the answer for as long as they stay on the map: the table is static data and the
// pointers into it outlive any bot.
DungeonTactics const* GetDungeonTactics(uint32 mapId);

#endif
