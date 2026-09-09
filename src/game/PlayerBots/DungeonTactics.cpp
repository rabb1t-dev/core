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

// Shadowfang Keep.
enum ShadowfangKeepCreatures
{
    NPC_ARCHMAGE_ARUGAL         = 4275,
};

enum ShadowfangKeepSpells
{
    // Arugal's only cast worth moving for, and the only one of his three that can be moved for at
    // all. Three seconds, single target, two hundred and twenty to two hundred and fifty eight
    // shadow damage at a level where the tank has well under two thousand health, and no
    // SPELL_ATTR_EX2_IGNORE_LINE_OF_SIGHT, so the sight check at the end of the cast applies.
    //
    // The other two are instant and cannot be dodged by anybody: Arugal's Curse (7621) picks a
    // random player, and Thundershock (7803) hits everything around him.
    //
    // Frequency is the reason this matters more here than the raw damage suggests. His spell list
    // carries Void Bolt twice, once with CF_ONLY_IN_MELEE on a five to seven second timer and once
    // with CF_NOT_IN_MELEE on a one second timer. Kiting him without cover is therefore far worse
    // than standing still; kiting him into cover is the whole of the fight.
    SPELL_VOID_BOLT             = 7588,
};

// Why there is no fight position for Arugal, having measured one and tried it.
//
// The measurement said there should be. Over his whole room, terrain and vmaps: standing anywhere
// on his platform nothing breaks his sight except the eleven yard drop off its edge, which is not a
// dodge; standing on the sunken floor below it there are two dozen places within six yards that do,
// and the floor is out of sight of the platform to begin with. So the tank was made to walk him down
// the stairs, a melee length at a time, and it did -- the legs descend one fifty one, one forty
// eight, one forty five, and it arrives.
//
// Then he leaves. He carries three separate Shadow Ports, 7136 to (-85.8 2150.2 155.6), 7586 to
// (-105.9 2154.9 156.4) and 7587 to (-102.9 2124.3 155.6), on timers of twenty two, thirty four and
// forty eight seconds and again whenever a root wears off. Every one of the three is up on the
// raised level and not one is on the floor, so the drag is undone within seconds of finishing and
// the tank spends the fight walking him down a staircase he ports back up. Two attempts, five drag
// legs each, and the fight ends up further from the position than it started.
//
// Which is the mechanical version of the advice every guide to him gives: do not chase him, he
// teleports back. Nothing can hold him, so nothing here tries to.

DungeonTactics const g_dungeonTactics[] =
{
    {
        MAP_SHADOWFANG_KEEP,
        "Shadowfang Keep",

        // Nothing in the instance sleeps, charms or fears often enough to cost a shaman its earth
        // totem for the whole run. The one reliable fear belongs to Sever, who is not on the way
        // to anything, and Tremor would do nothing at all for the rest of it.
        /* wantsTremorTotem */ false,

        /* escortNpcEntry */ 0,
        /* escortGuardRadius */ 0.0f,

        /* focusFirst */ {},

        /* creatures */
        {
            {
                NPC_ARCHMAGE_ARUGAL,
                // No standoff of its own. Thundershock reaches whoever is near him and Void Bolt
                // reaches everybody, so there is no distance that helps, and his room is a sunken
                // floor with a raised platform where pushing the casters out means pushing them
                // off it.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ SPELL_VOID_BOLT,

            },
        },
    },

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
            { NPC_VERDAN_THE_EVERLIVING, WC_MELEE_AOE_STANDOFF, 0 },
            // Thundercrack, plus a fight fought while adds are still arriving.
            { NPC_MUTANUS_THE_DEVOURER, WC_MELEE_AOE_STANDOFF, 0 },
            // Thunderclap, which is the one thing separating Pythas from the other three
            // Fanglords.
            { NPC_LORD_PYTHAS, WC_MELEE_AOE_STANDOFF, 0 },
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

uint32 DungeonTactics::GetBreakSightSpell(uint32 creatureEntry) const
{
    for (auto const& tactic : creatures)
        if (tactic.creatureEntry == creatureEntry)
            return tactic.breakSightSpellId;

    return 0;
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
