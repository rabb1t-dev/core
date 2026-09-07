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

#include "DungeonTactics.h"
#include "SharedDefines.h"

namespace
{

// Wailing Caverns.
//
// The instance is a DPS check against healing and a positioning check against blind corners, and
// the bots failed both. From one logged run: of roughly eleven hundred refused damage casts, eight
// hundred and twenty six were SPELL_FAILED_LINE_OF_SIGHT, and the fight that ended the run has a
// Druid of the Fang healing itself uninterrupted while the group's only shaman, being the healer,
// was forbidden from touching Earth Shock at all.
//
// Entries are the ones the instance script already names where it names them - 3671 Anacondra, 3673
// Serpentis, 3653 Kresh, 3654 Mutanus, 3840 Druid of the Fang, 3678 Disciple of Naralex - and the
// remaining Fanglords in the same block.
enum WailingCavernsCreatures
{
    NPC_KRESH                   = 3653,
    NPC_MUTANUS_THE_DEVOURER    = 3654,
    NPC_LORD_COBRAHN            = 3669,
    NPC_LORD_PYTHAS             = 3670,
    NPC_LADY_ANACONDRA          = 3671,
    NPC_LORD_SERPENTIS          = 3673,
    NPC_SKUM                    = 3674,
    NPC_DISCIPLE_OF_NARALEX     = 3678,
    NPC_DRUID_OF_THE_FANG       = 3840,
    NPC_VERDAN_THE_EVERLIVING   = 5775,
};

// Far enough out to sit outside Grasping Vines, which reaches ten yards and roots and stuns
// everything inside it, and outside Thunderclap and Thundercrack. Not further, because the rooms
// these are fought in are small and the extra yard buys nothing but another chance of a wall.
constexpr float WC_MELEE_AOE_STANDOFF = 14.0f;

DungeonTactics const g_dungeonTactics[] =
{
    {
        MAP_WAILING_CAVERNS,
        "Wailing Caverns",

        // Sleep on the four Fanglords, Druid's Slumber on the trash druids, and Naralex's Nightmare
        // on Mutanus. A slept healer at this level is a wipe, and Tremor Totem is the only answer a
        // Horde group has before a priest learns Dispel Magic.
        /* wantsTremorTotem */ true,

        // The Disciple has to survive the walk to Naralex and the waves that spawn once he starts
        // the ritual, and he does not defend himself well enough to be left to it.
        /* escortNpcEntry */ NPC_DISCIPLE_OF_NARALEX,
        // Wide enough to cover the whole of the ritual chamber, where the summons come in from
        // nine scattered points rather than from one direction.
        /* escortGuardRadius */ 40.0f,

        // A Druid of the Fang standing next to anything else is the target, because it is the one
        // that undoes the group's work on the other mob.
        /* focusFirst */ { NPC_DRUID_OF_THE_FANG },

        /* creatures */
        {
            // Grasping Vines: roots and stuns everything within ten yards, and cannot be
            // interrupted. Verdan also has the largest health pool in the instance, so the fight
            // lasts long enough for a rooted healer to matter.
            { NPC_VERDAN_THE_EVERLIVING, WC_MELEE_AOE_STANDOFF },
            // Thundercrack, plus a fight fought while adds are still arriving.
            { NPC_MUTANUS_THE_DEVOURER, WC_MELEE_AOE_STANDOFF },
            // Thunderclap, which is the one thing separating Pythas from the other three
            // Fanglords.
            { NPC_LORD_PYTHAS, WC_MELEE_AOE_STANDOFF },
        },
    },
};

} // namespace

float DungeonTactics::GetRangedStandoff(uint32 creatureEntry) const
{
    for (auto const& tactic : creatures)
        if (tactic.creatureEntry == creatureEntry)
            return tactic.rangedStandoff;

    return 0.0f;
}

bool DungeonTactics::IsFocusFirst(uint32 creatureEntry) const
{
    for (uint32 entry : focusFirst)
        if (entry == creatureEntry)
            return true;

    return false;
}

DungeonTactics const* GetDungeonTactics(uint32 mapId)
{
    for (auto const& tactics : g_dungeonTactics)
        if (tactics.mapId == mapId)
            return &tactics;

    return nullptr;
}
