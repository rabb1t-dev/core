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

// Razorfen Downs.
//
// The instance is here for one event: Belnistrasz channelling the idol shutdown for four minutes
// while four waves of adds arrive, which is the quest and the reason a group comes at all.
//
// He is the hardest kind of unit for a bot to keep alive. He is not in the group, so no heal
// selector can see him. He does not retaliate -- his script returns out of AttackStart for the
// whole of the ritual -- so nothing he is fighting ever becomes a bot's threat problem either. He
// stands still with combat movement switched off, does not heal himself, and holds a level 36
// caster's health pool against waves of four.
enum RazorfenDownsCreatures
{
    NPC_WITHERED_QUILGUARD      = 7329,
    NPC_WITHERED_BATTLE_BOAR    = 7333,
    NPC_DEATHS_HEAD_GEOMANCER   = 7335,
    NPC_BELNISTRASZ             = 8516,
};

// Zul'Farrak.
//
// Two things here that nothing else in the table has had to describe. The first is the pyramid
// event: fifty five trolls summoned at the foot of the stairs in three waves and released up them
// two, three, four, five at a time every ten seconds, with every wave gated on all of it being
// dead. The group holds the landing at the top; a bot that walks down to meet one arrives among
// the forty that are still waiting. The second is that the allies are plural -- Bly frees four
// others with him, and they are as much the group's problem to keep alive as Belnistrasz was.
enum ZulFarrakCreatures
{
    NPC_CHIEF_UKORZ_SANDSCALP   = 7267,
    NPC_WITCH_DOCTOR_ZUMRAH     = 7271,
    NPC_GAHZRILLA               = 7273,
    NPC_SHADOWPRIEST_SEZZZIZ    = 7275,
    NPC_ZULFARRAK_DEAD_HERO     = 7276,
    NPC_ZULFARRAK_ZOMBIE        = 7286,
    NPC_SERGEANT_BLY            = 7604,
    NPC_RAVEN                   = 7605,
    NPC_ORO_EYEGOUGE            = 7606,
    NPC_WEEGLI_BLASTFUSE        = 7607,
    NPC_MURTA_GRIMGUT           = 7608,
    NPC_EARTHGRAB_TOTEM         = 6066,
    NPC_WARD_OF_ZUMRAH          = 7785,
    NPC_SKELETON_OF_ZUMRAH      = 7786,
    NPC_HYDROMANCER_VELRATHA    = 7795,
    NPC_NEKRUM_GUTCHEWER        = 7796,
    NPC_FIRE_NOVA_TOTEM_IV      = 7844,
    NPC_ANTUSUL                 = 8127,
    NPC_SULLITHUZ_BROODLING     = 8138,
    NPC_SERVANT_OF_ANTUSUL      = 8156,
    NPC_GREATER_HEALING_WARD    = 8179,
    NPC_SANDFURY_ACOLYTE        = 8876,
};

// Maraudon.
//
// Four fights' worth of facts that no guide gets right and two that no creature carries. The
// guide to this instance says to stack the group on Tinkerer Gizlock and on Lord Vyletongue to
// deny them their ranged abilities, which is correct for players and wrong for bots, and the
// reason is in the spell data rather than in the advice -- see the entries below.
//
// The two that belong to no creature are the ground hazard and the untargetable phase:
//
//   Noxious Cloud sits on the floor of half the instance dealing a hundred and fifty a second,
//   and there is nothing in a bot's view of the world that can see it. See groundHazardSpellIds.
//
//   Noxxion spends fifteen seconds of every forty invisible, friendly-factioned and flagged
//   unselectable while his five spawns are up. Nothing here has to say so: IsValidAttackTarget
//   already refuses him and the party drops him on its own. It is written down because from
//   outside it looks exactly like a party that has stopped fighting, and a test that latches an
//   attack order on him rather than re-issuing it every poll will wait out its whole timeout.
enum MaraudonCreatures
{
    NPC_THERADRIM_SHARDLING      = 11783,
    NPC_PRINCESS_THERADRAS       = 12201,
    NPC_LANDSLIDE                = 12203,
    NPC_PRIMORDIAL_BEHEMOTH      = 12206,
    NPC_THESSALA_HYDRA           = 12207,
    NPC_BARBED_LASHER            = 12219,
    NPC_CONSTRICTOR_VINE         = 12220,
    NPC_NOXIOUS_SLIME            = 12221,
    NPC_CREEPING_SLUDGE          = 12222,
    NPC_CELEBRAS_THE_CURSED      = 12225,
    NPC_LORD_VYLETONGUE          = 12236,
    NPC_NOXXION                  = 13282,
    NPC_SUBTERRANEAN_DIEMETRADON = 13323,
    NPC_NOXXIONS_SPAWN           = 13456,
    NPC_STOLID_SNAPJAW           = 13599,
    NPC_TINKERER_GIZLOCK         = 13601,
    NPC_CORRUPT_FORCE_OF_NATURE  = 13743,
};

enum MaraudonSpells
{
    // Noxious Cloud. A persistent area aura five yards across, a hundred and fifty nature damage
    // a second for twenty seconds. Noxious Slime casts it as it dies, which puts it under
    // whoever killed it; Creeping Sludge drops one every twenty two to twenty six seconds while
    // it fights. Ninety nine of the two between them, spread across Foulspore Cavern and Poison
    // Falls, which is most of the walking in this instance.
    SPELL_NOXIOUS_CLOUD          = 21070,
};

// Outside the ten yard band, which is the radius Earth Song Falls and Poison Falls have settled
// on and the one number that covers most of the walk to Princess Theradras. Landslide's Trample,
// the Primordial Behemoths' Trample, Thessala Hydra's Aqua Jet knockback, the Diemetradons'
// Sonic Burst silence and Tinkerer Gizlock's Goblin Dragon Gun cone are all radius index 13 -- a
// flat ten yards -- so a caster standing at twelve is outside every one of them and still well
// inside its own thirty.
//
// Twelve rather than eleven because the radius is measured from the creature's centre and these
// are large models: a Behemoth's bounding radius alone is most of the spare yard.
constexpr float MARA_TEN_YARD_AOE_STANDOFF = 12.0f;

// Outside Dust Field, which is twenty.
//
// Princess Theradras's own spell is a self-cast eight second aura that fires the real one --
// 21868, also called Dust Field -- once a second underneath her: a hundred and thirty one to a
// hundred and sixty eight nature damage and a knockback, twenty yards, eight times, every ten to
// thirty seconds. Repulsive Gaze, the fear, is only eight yards, so anything that clears the
// field clears the fear as well.
//
// This is the number the caster ladder would otherwise throw away. Twenty five is what a caster
// already wants, but CB_CASTER_CHASE_DISTANCES falls back to twenty, fifteen and ten whenever
// twenty five is judged unsafe, and every one of those three is inside the field. Writing it
// down as a floor is what stops the fallback.
constexpr float MARA_THERADRAS_STANDOFF = 25.0f;

// Sunken Temple.
//
// Read off the world database rather than the scripts, because there are no boss scripts to read:
// src/scripts/**/sunken_temple holds the instance script and Malfurion and nothing else. Every boss
// and every trash mob here is EventAI or a bare spell list, so the creatures below were enumerated
// from the creature table for map 109 and each one accounted for -- the Magmadar trap, where a search
// of the scripts finds nothing and the instance reads as harmless, applies to the whole dungeon.
//
// Three things in it that the shared AI cannot see on its own:
//
//   Totems and wards. Four of the six spell lists that summon anything summon something stationary
//   that out-produces the group: Zolo's Atal'ai Totem raises a skeleton every five seconds, Mijan's
//   Healing Ward V is the same Greater Healing Ward Antu'sul drops, and Jammal'an carries Earthgrab
//   Totem on an eight to fifteen second repeat. None of them ever appears in an attacker list.
//
//   Healers in nearly every pack. Atal'ai Witch Doctor (twenty eight spawns), Atal'ai Priest and
//   Atal'ai High Priest all heal other mobs -- castTarget 17, friendly missing health -- and the
//   High Priest does it every five to eight seconds.
//
//   The Avatar of Hakkar event, which is lost rather than wiped. Nightmare Suppressors walk in from
//   the doors and channel Suppression on the Shade of Hakkar; three hits fail the event outright.
//
// And two zone pulls that are not tactics for the bots but are the reason a run here can go wrong
// before any of the above matters. Jammal'an's aggro sets TYPE_JAMMALAN in progress, and the instance
// script then puts every DB-spawned Mummified Atal'ai, Atal'ai Deathwalker and Atal'ai High Priest
// within a hundred and fifty yards of him into combat with the zone. Shade of Eranikus does the same
// for every Nightmare Scalebane, Wyrmkin, Whelp and Wanderer within *three hundred*. Both rooms have
// to be cleared before their boss is pulled; nothing in this table can make that safe.
enum SunkenTempleCreatures
{
    NPC_ATALAI_DEATHWALKER       = 5271,
    NPC_ATALAI_HIGH_PRIEST       = 5273,
    NPC_ATALAI_WITCH_DOCTOR      = 5259,
    NPC_ATALAI_PRIEST            = 5269,
    NPC_HAKKARI_FROSTWING        = 5291,
    NPC_MORPHAZ                  = 5719,
    NPC_HAZZAS                   = 5722,
    NPC_WEAVER                   = 5720,
    NPC_DREAMSCYTHE              = 5721,
    NPC_JAMMALAN_THE_PROPHET     = 5710,
    NPC_HUKKU                    = 5715,
    NPC_MIJAN                    = 5717,
    NPC_ATALALARION              = 8580,
    NPC_DEEP_LURKER              = 8384,
    NPC_ATALAI_SKELETON          = 8324,
    NPC_ATALAI_TOTEM             = 8510,
    NPC_HAKKARI_BLOODKEEPER      = 8438,
    NPC_NIGHTMARE_SUPPRESSOR     = 8497,
    NPC_HUKKUS_VOIDWALKER        = 8656,
    NPC_HUKKUS_SUCCUBUS          = 8657,
    NPC_HUKKUS_IMP               = 8658,
};

// Outside the ten yard band, for the same reason and with the same arithmetic as Maraudon's: radius
// index 13 is a flat ten yards, and the large models need the spare yard. In this instance it is the
// green dragonkin's Acid Breath and Wing Flap cones, the Deep Lurkers' Trample and the Frostwings'
// Frost Nova.
constexpr float ST_TEN_YARD_AOE_STANDOFF = MARA_TEN_YARD_AOE_STANDOFF;

// Outside Ground Tremor, which is twenty.
//
// Atal'alarion's 6524 is a stun centred on himself -- implicit target 22, radius index 9 -- on a
// twenty to thirty two second repeat, cast with CF_INTERRUPT_PREVIOUS so nothing he is doing delays
// it. Twenty five for the reason MARA_THERADRAS_STANDOFF spells out: the caster ladder falls back to
// twenty, fifteen and ten, and all three are inside the stun.
constexpr float ST_ATALALARION_STANDOFF = 25.0f;

// Molten Core.
//
// The first raid entry in this table, and the two things in it worth writing down are both things
// a bot cannot reach from a creature's own data: one add that cannot be killed and must not be
// moved, and one trash mob that heals the pack from the moment it is pulled.
enum MoltenCoreCreatures
{
    NPC_FLAMEWAKER_PRIEST       = 11662,
    NPC_CORE_RAGER              = 11672,
};

DungeonTactics const g_dungeonTactics[] =
{
    {
        MAP_SHADOWFANG_KEEP,
        "Shadowfang Keep",

        // Nothing in the instance sleeps, charms or fears often enough to cost a shaman its earth
        // totem for the whole run. The one reliable fear belongs to Sever, who is not on the way
        // to anything, and Tremor would do nothing at all for the rest of it.
        /* wantsTremorTotem */ false,

        /* escortNpcEntries */ {},
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
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
            },
        },

        /* holdLines */ {},
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
        /* escortNpcEntries */ { NPC_DISCIPLE_OF_NARALEX },
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
            { NPC_VERDAN_THE_EVERLIVING, WC_MELEE_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },
            // Thundercrack, plus a fight fought while adds are still arriving.
            { NPC_MUTANUS_THE_DEVOURER, WC_MELEE_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },
            // Thunderclap, which is the one thing separating Pythas from the other three
            // Fanglords.
            { NPC_LORD_PYTHAS, WC_MELEE_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },
        },

        /* holdLines */ {},
    },

    {
        MAP_RAZORFEN_DOWNS,
        "Razorfen Downs",

        // Nothing in here sleeps or fears often enough to be worth an earth totem slot.
        /* wantsTremorTotem */ false,

        // Belnistrasz, for the whole of the ritual. See the note above the entries.
        /* escortNpcEntries */ { NPC_BELNISTRASZ },
        // The waves arrive from three fixed spawner points scattered around the idol room, and the
        // group fights them where they land rather than on top of him, so this has to cover the
        // room rather than the few yards around him.
        /* escortGuardRadius */ 40.0f,

        // The Geomancer is the caster in a wave that is otherwise two boars and a quilguard, and
        // the one part of it that damages Belnistrasz without walking up to him.
        /* focusFirst */ { NPC_DEATHS_HEAD_GEOMANCER },


        /* creatures */
        {
        },

        // The idol room is fought across rather than held at a choke, and the waves land in it
        // from three directions, so there is no line to hold.
        /* holdLines */ {},
    },

    {
        MAP_ZUL_FARRAK,
        "Zul'Farrak",

        // Psychic Scream, on a twenty two second cooldown, on one caster -- which on frequency
        // alone would not clear the bar Shadowfang set. It earns the slot on where it lands
        // instead: Shadowpriest Sezz'ziz arrives in the third pyramid wave, and a fear at the top
        // of those stairs scatters the group *downwards*, into the twenty trolls that have not
        // been released yet. The cost of the one that lands is the event.
        /* wantsTremorTotem */ true,

        // Bly and his four. They are freed by the group, fight beside it for the whole pyramid
        // event, and are not group members, so nothing in the heal selectors could see them.
        // Weegli is the one that matters beyond the event itself: he is the only thing that opens
        // the end door, and he stands furthest forward of the five during wave three.
        // Weegli first, and it is not a close call: he is the only thing in the instance that
        // opens the end door, so the group can lose any of the other four and still finish, and
        // cannot lose him. Bly second, because the crew fight after the event is started from his
        // gossip and a dead Bly means there is no fight to have.
        /* escortNpcEntries */ { NPC_WEEGLI_BLASTFUSE, NPC_SERGEANT_BLY, NPC_MURTA_GRIMGUT,
                                 NPC_ORO_EYEGOUGE, NPC_RAVEN },
        // The five are spread across the foot of the stairs during wave three and across the
        // landing after it, so this covers the ground they hold rather than the yards around any
        // one of them.
        /* escortGuardRadius */ 50.0f,

        // Written in kill order. Two blocks of it describe a fight where more than one entry is
        // standing in front of the group at once, and in those the order is the tactic:
        //
        //   Third pyramid wave: Nekrum Gutchewer before Shadowpriest Sezz'ziz. Both are on the
        //   list and both arrive together, and the flat list left the group choosing between them
        //   on health and then on guid. Nekrum is the damage and has no answer to being focused;
        //   Sezz'ziz is dealt with by the interrupts rather than by the damage, which the group
        //   already does for any caster in range of a Kick whatever it is currently hitting. So
        //   the party kills Nekrum while the melee take the heals off Sezz'ziz, and Sezz'ziz is
        //   still ahead of the ordinary trolls for the stretch after Nekrum dies.
        //
        //   Bly's crew, after the event: Oro Eyegouge and Murta Grimgut before Sergeant Bly. Bly
        //   is deliberately absent from the list rather than placed last on it -- he is what is
        //   left when the other two are down, and nothing needs to prefer him over anything.
        /* focusFirst */
        {
            // Raises a Skeleton of Zum'rah every five seconds until it is killed, and both
            // Zum'rah and Velratha re-summon it on a fifteen and a thirteen second timer. Nothing
            // in the target selection would otherwise touch a stationary summon.
            //
            // First in the instance and first in the Zum'rah fight, which is the whole tactic
            // rather than a preference. The Ward is the source: every five seconds it raises
            // another skeleton, for as long as it stands, so a group that kills skeletons instead
            // is doing work the Ward replaces faster than the group can clear it and never gets
            // back to the boss. It is also the cheapest thing in the fight to remove -- a
            // stationary non-elite while the group is forty six -- so the ordering costs nothing
            // and ends the stream outright.
            NPC_WARD_OF_ZUMRAH,
            // The skeletons themselves, behind the Ward that makes them and ahead of Zum'rah.
            //
            // Level thirty five non-elites against a group eleven levels above them, so turning
            // for one is a couple of swings rather than a detour, and they are the reason the
            // healer runs out of room: they arrive continuously and land on whoever is nearest,
            // which in a fight with a thirty yard area nuke in it is usually the casters. Ahead
            // of the boss for that reason, not because any one of them is dangerous.
            NPC_SKELETON_OF_ZUMRAH,
            // And what comes out of the graves. Using one summons a Zul'Farrak Zombie sixty five
            // percent of the time or a Zul'Farrak Dead Hero ten percent of it -- both elite,
            // forty three to forty six, on a thirty second leash.
            //
            // Player-triggered rather than part of Zum'rah's script, which is exactly why the
            // bots need to be told: nothing about the encounter predicts them, they appear behind
            // the group while it is facing the boss, and an unheld elite on the healer is how this
            // fight is actually lost. The Dead Hero first of the two, being the higher level of
            // them and the only one with a spell list.
            //
            // Safe to rank instance wide even though it is only Zum'rah's fight these belong to.
            // All seventy one graves sit in his half of the instance, between twenty two and
            // eighty one yards of him, and the closest is two hundred and eighteen yards from the
            // pyramid -- so there is no pull anywhere else where these could outrank a wave.
            NPC_ZULFARRAK_DEAD_HERO,
            NPC_ZULFARRAK_ZOMBIE,
            // Fire Nova Totem IV. Same shape, same fix: a stationary summon nothing in the
            // ordinary target selection would ever walk over to.
            NPC_FIRE_NOVA_TOTEM_IV,

            // Antu'sul, whose whole fight is the three things below rather than his own damage.
            //
            // He alternates two summons on one eleven to twenty two second timer, and carries
            // three self heals on top of them -- Flash Heal below sixty percent, Healing Wave of
            // Antu'sul below thirty and again at twenty. The heals the interrupt logic already
            // rates top priority and needs no help with. The summons it cannot see at all, and
            // they are what makes the fight unwinnable rather than merely long:
            //
            // The Greater Healing Ward heals him and everything around him every few seconds for
            // its whole thirty second life. Left standing it out-heals a five man group's damage
            // outright, so the boss never dies no matter how the rest of the fight goes -- which
            // is the shape of a wipe that looks like a damage problem and is not one.
            NPC_GREATER_HEALING_WARD,
            // And the Earthgrab Totem roots everyone within ten yards for four seconds out of
            // every five. That is most of a melee bot's uptime gone, and worse, a rooted bot
            // cannot walk out of anything -- so the totem that kills the group is the one nobody
            // turned around to hit.
            NPC_EARTHGRAB_TOTEM,
            // The adds are elite and he sends one at seventy five percent and two more at twenty
            // five, on top of the four broodlings the area trigger hatches on the way in. Three
            // loose elites on a healer ends the attempt, and the guides are unanimous that they
            // come before the boss does.
            NPC_SERVANT_OF_ANTUSUL,
            // Sul'lithuz Broodling is deliberately absent. The four the area trigger hatches on
            // the way in are the player's to deal with, and every second the group spends turning
            // for one is a second Antu'sul is not being damaged -- which matters more than usual
            // here, because his heals are on timers rather than on his health, so a slower fight
            // is a fight with more healing in it. Leaving the entry out means nothing prefers a
            // broodling; a bot still defends itself if one attacks it.

            // The third pyramid wave, in the order it is fought. Nekrum is the whole of that
            // wave's damage -- an elite arriving on a landing the group has already been holding
            // for two waves, with nothing in his book that undoes being focused. He dies first
            // for that reason rather than because he is the more dangerous of the two in the
            // abstract.
            NPC_NEKRUM_GUTCHEWER,
            // Heal on a fourteen second timer plus Renew, and he arrives in wave three where the
            // group is already holding against everything else. Behind Nekrum, because a healer
            // the group can interrupt is not a healer the group has to kill first, and the
            // interrupt scan already reaches him whatever the party happens to be hitting -- see
            // healsBelowPercent on his entry below, which is what makes the melee hold a global
            // for it. Still ahead of everything unlisted, so he is next once Nekrum is down.
            NPC_SHADOWPRIEST_SEZZZIZ,

            // Bly's crew, once the event is over and the five of them turn on the group. The
            // order is the one every guide gives and the creature data agrees with it: Oro
            // carries the only area damage in the fight and Murta is the only thing healing any
            // of them, so both come before Bly himself.
            //
            // Oro ahead of Murta. The opposite reading -- the healer always dies first -- is what
            // the rest of this list does and is arguable here too; the difference is that Murta
            // heals one target at a time while Oro is hitting the whole party at once, and a five
            // man taking area damage for the whole fight has less time than one whose target
            // keeps being topped up.
            NPC_ORO_EYEGOUGE,
            NPC_MURTA_GRIMGUT,
            // Sergeant Bly is deliberately not on this list. He is what is left when those two are
            // down, and an entry for him would only order him against the two things that are
            // supposed to be killed before him. Raven and Weegli are absent for the same reason:
            // nothing about either is worth preferring, and Weegli leaves the fight on his own to
            // go and blow the door.

            // Healing Wave every nine seconds.
            NPC_HYDROMANCER_VELRATHA,
            // Heals himself every fifteen seconds.
            NPC_WITCH_DOCTOR_ZUMRAH,
            // Mana Burn every fifteen seconds, and there are four of them in each of the first two
            // waves. A healer burned dry partway through fifty five mobs is the run, and it is the
            // one form of damage no amount of healing answers.
            NPC_SANDFURY_ACOLYTE,
        },


        /* creatures */
        {
            {
                NPC_GAHZRILLA,

                // Outside the Slam. It is an area knockback centred on him -- implicit targets 22
                // and 15, caster source and every enemy at it -- rather than something aimed at
                // the tank, so anybody who wanders into the band is thrown with the melee. The
                // caster chase would otherwise settle at whatever distance was merely safe from
                // other packs, and in an empty pool room that is as close as it likes.
                /* rangedStandoff */ 15.0f,
                /* breakSightSpellId */ 0,

                // The north-east bank of his pool, measured with `.harness ground` at two yard
                // spacing over x 1650..1682 by y 1174..1202.
                //
                // The pool is a channel running north-west to south-east, floor flat at -2.9 and
                // twelve to fourteen yards across, with banks at +9.4 -- a twelve yard rise over
                // two yards of ground, which is a wall rather than a slope. He is summoned by
                // event 2488 into the middle of it at (1664.65, 1187.67, -2.89).
                //
                // (1670, 1185) is five and a half yards from that, on the channel floor, with the
                // bank four to six yards behind it on the bearing directly away from him. So a
                // Slam throws the melee into the bank instead of across the room, which is the
                // whole of what a player means by fighting him with their back to the wall.
                //
                // Six yards of radius, which is enough to hold a tank and two melee without them
                // standing inside one another and small enough that the bank stays behind all of
                // them.
                /* anchor */ 1670.0f, 1185.0f, -2.9f, 6.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
            },
            {
                NPC_ANTUSUL,
                // Twenty five yards, and this is the entry that should have been carrying the
                // healer positioning all along.
                //
                // Every Antu'sul attempt so far has the priest logging standfast from two to four
                // yards and dying there, and each time the answer reached for was a new rule in
                // the shared AI -- a melee floor, a backout, a huddle -- when the table already
                // had a field for exactly this and it was set to zero. Antu'sul runs at eight to
                // the group's seven, so a healer that lets him close can never open the gap again;
                // the only distance that helps is the one it never gives up.
                //
                // Twenty five rather than the ten yard floor because the floor is about surviving
                // a melee swing and this is about not being reachable in the first place, with
                // the Servant and two Minions also loose in the same room.
                /* rangedStandoff */ 25.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // Sixty, because that is where Flash Heal becomes available to him and the cost of
                // looking away changes sign. Above it he has no heal at all, so the Servant he
                // sends at seventy five percent is free to kill and the group should. Below it
                // every pause is undone: Flash Heal on a seventeen to twenty second repeat, then
                // Healing Wave of Antu'sul every twelve seconds under thirty percent, by which
                // point two more Servants have arrived at once. A group that stops for those two
                // can watch him climb back out of range of the burn it just spent.
                //
                // Read off his own script rather than a guide, which says only "kill adds first"
                // and is right for every boss that cannot heal.
                // Eighty, not sixty. The first Servant arrives at seventy five percent, so a gate at sixty
        // let the group turn for it, and a Servant is roughly a third of the boss's health bar:
        // twenty seconds spent killing one is two Healing Waves of Antu'sul, about forty eight
        // hundred health handed back, which is more than the add would ever have dealt. Above
        // eighty there is no Servant yet, so this reads as "never leave him for one".
        /* burnBelowPercent */ 80.0f,
                /* summonEntries */ { NPC_SERVANT_OF_ANTUSUL, NPC_SULLITHUZ_BROODLING },

                // Flash Heal at sixty percent, Healing Wave of Antu'sul at thirty and again at
                // twenty. Sixty is where the group needs its interrupts ready, so this is where
                // a bot holding one stops spending the tick on filler.
                /* healsBelowPercent */ 60.0f,
            },
            {
                NPC_WITCH_DOCTOR_ZUMRAH,

                // No standoff, and the number is worth writing down because this is the one fight
                // where it looks like there should be one.
                //
                // Shadow Bolt Volley is an area nuke centred on him -- implicit target 22, caster
                // source, the same shape as Gahz'rilla's Slam, which earned a fifteen yard band on
                // this very table. The difference is the radius. Gahz'rilla's reaches far enough to
                // throw the melee and no further; Zum'rah's is radius index ten, the thirty yard
                // band Prayer of Healing and the paladin auras share, so it covers everything that
                // can see him. A caster's own spells reach thirty, so there is no distance that is
                // both outside the volley and inside its own range: pushing the casters out only
                // stops them attacking. The volley is the healer's problem and the interrupt's, and
                // there is no positioning answer to it.
                /* rangedStandoff */ 0.0f,

                // And no break-sight spell either. The volley is the only cast here worth avoiding
                // and it is party wide, which the field explicitly cannot help with -- one bot
                // walking cannot take an area spell away from the party. His Shadow Bolt is single
                // target and would qualify on shape, but it repeats every three to five seconds and
                // the pit he stands in is open ground with no corner to step behind, so a bot told
                // to break sight for it would spend the fight walking instead of casting.
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // Zero, which switches the gate off: the group always turns for his adds. That is
                // the opposite of what Antu'sul is told twenty lines above, so the reason matters.
                //
                // Antu'sul's gate exists because the delay is unrecoverable there -- his adds are
                // level forty eight elites worth a third of his health bar, and his heals are
                // timed, so a long pause is handed straight back as healing. Neither holds here.
                // Zum'rah's own adds are level thirty five non-elites, so the pause is a couple of
                // swings rather than twenty seconds, and his heal is a single interruptible Healing
                // Wave the party is already told to hold a global for -- see healsBelowPercent
                // below. When the answer to a boss healing himself is the interrupt, refusing to
                // kill the adds buys nothing and leaves loose elites on the healer.
                /* burnBelowPercent */ 0.0f,

                // Everything in this fight that walks, and deliberately not the Ward.
                //
                // This list does two jobs and the second is the one that matters here: an entry on
                // it is barred from being posted at by a hunter or warlock pet. The Ward is a
                // stationary non-elite and is exactly what a pet should be sent to break alone, so
                // it stays off this list and stays top of focusFirst, which together are what put
                // the pet on it -- the same arrangement Antu'sul's Greater Healing Ward and
                // Earthgrab Totem already have. The skeletons and the grave elites go on it,
                // because a pet sent at a forty six elite Dead Hero by itself is a dead pet and a
                // Ward still standing.
                /* summonEntries */ { NPC_SKELETON_OF_ZUMRAH, NPC_ZULFARRAK_ZOMBIE,
                                      NPC_ZULFARRAK_DEAD_HERO },

                // A hundred, meaning from the pull, for the same reason Sezz'ziz below has it:
                // Healing Wave is on a fifteen to twenty three second repeat off his script rather
                // than gated on his health, so there is no stretch of this fight where a held
                // interrupt is being wasted.
                //
                // This is also the field that does most of the work for the volley. Zum'rah has two
                // interruptible casts, and a bot that has spent its global on filler has an answer
                // to neither; holding the tick free from the pull means whichever of the two he
                // starts, somebody can take it away. The interrupt scan already prefers the heal
                // over the nuke on its own, which is the right way round -- the heal undoes the
                // kill, the volley only costs health the healer can put back.
                /* healsBelowPercent */ 100.0f,
            },
            {
                NPC_SHADOWPRIEST_SEZZZIZ,
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},

                // A hundred, which reads as "from the pull", and is the case the field was
                // written to allow for. Antu'sul heals at a point in his health bar and so has a
                // number worth watching for; Sezz'ziz heals on a fourteen second timer from the
                // moment he arrives, so there is no stretch of the fight where holding a global
                // for the interrupt is wasted. That is the whole of what the group is asked to do
                // about him while it kills Nekrum.
                /* healsBelowPercent */ 100.0f,
            },
        },

        /* holdLines */
        {
            // The landing at the top of the pyramid stairs, measured with `.harness ground` over
            // x 1870..1898 by y 1248..1292 rather than read off the spawn table.
            //
            // The landing is flat at z 41.5 to 42.1 and runs x 1874 to 1896 by y 1262 to 1278.
            // South of it the stairway drops away fast -- 40 at y 1260, 35.5 at 1256, 31.5 at
            // 1252, 27.4 at 1248 -- and the courtyard the adds are summoned into sits at 8.88
            // from y 1230 all the way back to 1190. Thirty three yards of descent in twenty of
            // ground.
            //
            // Twenty six, and the radius rather than the floor is what keeps the party off the
            // courtyard.
            //
            // Thirty nine and a half was the first answer -- above the highest reading below the
            // top step (39.30 at the outside corner of y 1256) and below the lowest on the
            // landing (40.36) -- and it deadlocked the event against the release cap. Trolls do
            // not reliably finish the climb: a measured run has every one of three hundred
            // refusals aimed at something between z 27 and z 39, stalled on the staircase a yard
            // or two under the line. The party will not come down to them, they do not come up,
            // and the cap counts them as released and alive, so nothing further is released
            // either. Fifteen minutes of a tank at full health shouting at itself.
            //
            // The floor is therefore set below where they stall rather than above it, and the
            // courtyard is refused by distance instead: at y 1248 the stairway is 22 yards out in
            // plan and inside the zone, while the nearest ground the unreleased trolls stand on,
            // y 1230, is 40 -- well outside a radius of 25. So the party can step down the top of
            // the stairs to finish something that stopped there, and still cannot reach the forty
            // that are waiting.
            //
            // Sixty yards of recovery, which is the other half of the rule and the half the event
            // punishes. Sezz'ziz arrives in wave three carrying Psychic Scream, and a fear at the
            // top of these stairs throws bots down them -- at which point they are outside the
            // zone, the refusal to leave it stops applying to them by construction, and they
            // finish the event at the bottom among the trolls that have not been released. Sixty
            // reaches the courtyard floor from the middle of the landing, so a bot thrown the
            // whole length of the stairway still walks back up; a bot the leader has deliberately
            // walked further than that is left alone, which is what keeps this from dragging the
            // group back up the pyramid for the Bly fight at the bottom of it.
            { 1885.0f, 1270.0f, 42.0f, 25.0f, 26.0f, 60.0f },
        },

        // The player asked for these specifically, and the measurement agrees with the ask: the
        // Broodlings are level thirty nine against a level forty two group, they deal almost
        // nothing, and every second the tank spends on one is a second Antu'sul spends building a
        // threat lead on the healer that the tank never gets back.
        /* ignoreEntries */ { NPC_SULLITHUZ_BROODLING },
    },

    {
        MAP_MARAUDON,
        "Maraudon",

        // Two fears on two of the last three bosses, and one of them is close to continuous.
        //
        // Princess Theradras carries Repulsive Gaze, an eight yard fear every thirty five to
        // forty five seconds, which only reaches the melee -- the standoff below keeps everybody
        // else out of it. On its own that would not clear the bar Shadowfang set, and the ground
        // it lands on is what changes the answer: her cavern is the lip of Zaetar's, and a feared
        // melee bot runs toward the drop into Rotgrip's water with the Thessala Hydras in it.
        //
        // Tinkerer Gizlock is the one that settles it. On this server he is patch nine or later,
        // which is spell list 136011 and not 136010, and that list opens with Flash Bomb (29419):
        // a five yard fear thrown at a random player within twenty yards, ten seconds long, on a
        // seven to eight second repeat with CF_AURA_NOT_PRESENT -- meaning he re-throws it the
        // moment it falls off. That is a rolling fear on the whole party for the length of the
        // fight, and Tremor Totem pulsing every three seconds is the only answer a group at this
        // level has to it.
        /* wantsTremorTotem */ true,

        // Celebras the Redeemed walks his escort after the boss is dead and fights nothing on the
        // way. There is no escort to keep alive in this instance.
        /* escortNpcEntries */ {},
        /* escortGuardRadius */ 0.0f,

        // Written in kill order.
        /* focusFirst */
        {
            // Noxxion's Spawn, and this is the whole of that fight rather than a preference.
            //
            // Every forty seconds Noxxion interrupts himself, sets faction 35, flags himself
            // unselectable, swaps to an invisible model and spawns five of these; fifteen seconds
            // later he comes back. So for fifteen seconds out of every forty there is nothing
            // else in the room the party can legally attack, and a group that has not been told
            // to kill the spawns stands still through all of it.
            //
            // They are cheap to remove -- level forty six, non elite, health multiplier 0.40
            // against a boss at 7.00 -- so the ordering costs nothing, which is why they come
            // first outright rather than under a burn gate the way the other two summons below
            // do.
            NPC_NOXXIONS_SPAWN,

            // Barbed Lasher, ahead of everything else standing in a Foulspore Cavern pack.
            //
            // Thorn Volley is a thirty yard area nuke centred on the Lasher -- a hundred and
            // fifty nature damage and a two second stun on everything that can see it -- on a six
            // to eleven second repeat, and it is instant, so no interrupt exists for it. A pack
            // with a Lasher in it has the whole party stunned for roughly a fifth of the fight
            // and there is no position that helps: thirty yards is further than a caster's own
            // range. The only answer is to take the Lasher out of the pack first, and there are
            // seventeen of them on the way to Noxxion.
            NPC_BARBED_LASHER,

            // Constrictor Vine, behind the Lasher and ahead of the rest of the pack. Entangling
            // Roots at a random target on the threat list, fifteen seconds, every nine to
            // seventeen -- which on a healer is the pull. It is a cast rather than an instant, so
            // the interrupt scan reaches it and already rates a root top priority; this only
            // decides who dies next once the Lasher is down.
            NPC_CONSTRICTOR_VINE,

            // Lord Vyletongue ahead of his two Putridus Shadowstalker guards, which is what every
            // guide says and what his own book agrees with. He is the only one of the three that
            // can leave: Blink (21655) is a twenty yard leap on a twenty to thirty second timer,
            // and once he is at range Multi-Shot and Shoot come off a five yard minimum that
            // melee standing on him would otherwise deny. Kill the one that runs, then the two
            // that cannot.
            //
            // The guards are deliberately absent rather than placed last. Nothing needs to prefer
            // one Shadowstalker over the other, and an entry for them would only order them
            // against the boss they are supposed to die after.
            NPC_LORD_VYLETONGUE,

            // Deliberately absent from this list, and both for the same reason: they are
            // summons under a burn gate below, which is the opposite instruction. Theradrim
            // Shardling and Corrupt Force of Nature are things the party is told to walk past,
            // not things it is told to prefer.
        },

        /* creatures */
        {
            {
                NPC_NOXXION,

                // No standoff, and the number is worth writing down because the fight looks like
                // it should have one. Toxic Volley is radius index 11 -- forty five yards, the
                // widest band in the game short of a raid-wide -- so it covers the pool, the
                // bank and everything a caster could retreat to. Uppercut is a ten yard
                // knockback but it is aimed at his current victim, which is the tank, so pushing
                // the casters out buys nobody anything.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // Zero, which switches the gate off: the party always turns for the spawns. It
                // has to, because for the fifteen seconds they are up he is not a legal target
                // at all -- see the note on focusFirst above. This is the clearest case in the
                // table for the gate being off and it is here to say so out loud, since the two
                // entries below it are both the other way round.
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ { NPC_NOXXIONS_SPAWN },
                /* healsBelowPercent */ 0.0f,
            },
            {
                NPC_CELEBRAS_THE_CURSED,

                // Nothing a distance answers. Wrath reaches forty yards at a random attacker and
                // Entangling Roots thirty at his victim, and both are casts the interrupt scan
                // already sees -- the root scores as crowd control, which is top priority, and
                // the group holds nothing back for it because the root lands on the tank, who is
                // standing next to him anyway.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // A hundred, which reads as "never leave him for one", and it is the gate this
                // fight turns on.
                //
                // Corrupt Forces of Nature is instant, summons two Corrupt Force of Nature
                // within five yards, and repeats every twenty seconds for as long as he is
                // alive. There is no point at which killing them finishes: a party that turns
                // for each pair is doing work he replaces faster than it can clear it and never
                // gets back to him, which is the exact shape of a fight that stalls at eighty
                // percent with nobody dying. The summons are level forty four non-elites against
                // a party in the high forties, so ignoring them costs very little damage taken,
                // and they expire on their own after sixty seconds.
                /* burnBelowPercent */ 100.0f,
                /* summonEntries */ { NPC_CORRUPT_FORCE_OF_NATURE },

                // No heal in his book, so nothing to reserve a global cooldown against. The
                // interrupts he is worth are spent opportunistically on whichever of Wrath and
                // Roots is in progress, which is what InterruptHostileCasters already does.
                /* healsBelowPercent */ 0.0f,
            },
            {
                NPC_LANDSLIDE,

                // Outside Trample, which is a ten yard area hit centred on him every eight
                // seconds from the pull.
                //
                // Knock Away is the other half of his book and no standoff answers it: it is ten
                // yard range, single target, aimed at his victim, so it only ever throws the
                // tank. What a player does about that is put the tank's back to a wall, and that
                // is a fact about his alcove rather than about him -- see the note below on why
                // there is no anchor here yet.
                /* rangedStandoff */ MARA_TEN_YARD_AOE_STANDOFF,
                /* breakSightSpellId */ 0,

                // No fight anchor, and it is left out rather than guessed.
                //
                // His alcove wants the Gahz'rilla treatment: a spot on the floor with the rock
                // wall four to six yards behind it on the bearing away from him, so Knock Away
                // throws the tank into the wall instead of across the room. Gahz'rilla's was
                // measured with `.harness ground` at two yard spacing over his whole pool before
                // a single coordinate was written down, and the same has to happen here -- he
                // stands at (356.7, -185.5, -59.8) and nothing in this file knows what is behind
                // that. A coordinate invented from a map screenshot is worse than no anchor,
                // because the melee will walk to it every time they are thrown.
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // A hundred: never leave him for a Shardling.
                //
                // Below fifty percent he channels Summon Shardlings for eight seconds, which
                // triggers 21809 every two seconds and puts four Theradrim Shardlings on the
                // floor, and he does it again every sixty. They are level forty six non-elites
                // whose only ability is Strike -- and they despawn the moment he dies. So every
                // second spent on one is a second spent on something that was going to remove
                // itself, against a boss with a health multiplier of seven who is making four
                // more of them a minute.
                /* burnBelowPercent */ 100.0f,
                /* summonEntries */ { NPC_THERADRIM_SHARDLING },
                /* healsBelowPercent */ 0.0f,
            },
            {
                NPC_PRINCESS_THERADRAS,

                // Twenty five, outside Dust Field. See MARA_THERADRAS_STANDOFF for the
                // arithmetic and for why the number has to be written down rather than left to
                // the caster ladder.
                //
                // This is also the entry where the guide and the data disagree and the data
                // wins in the group's favour. Boulder -- her third ability, a two second cast at
                // a random player for three hundred and ninety three to five hundred and six,
                // plus an interrupt and a two second knockdown -- has a five yard *minimum*
                // range, so a party stacked in melee would deny it outright. It is still the
                // wrong trade: Boulder is one target every ten to thirty seconds, while Dust
                // Field is the whole party, eight times, every ten to thirty. Stand out of the
                // field and eat the Boulder.
                /* rangedStandoff */ MARA_THERADRAS_STANDOFF,

                // Nothing to break sight of. Boulder is the only cast in her book long enough to
                // dodge and her cavern is open floor with nothing to step behind; the other
                // three are instant.
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // Zaetar's Spirit is summoned by her death rather than during the fight, so
                // there is no summon here to gate.
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                /* healsBelowPercent */ 0.0f,
            },
            {
                NPC_TINKERER_GIZLOCK,

                // Twelve, and this is the entry that contradicts the published strategy on
                // purpose.
                //
                // Every guide to him says to stack the whole group in melee so he cannot use his
                // ranged abilities, and for a player group that is right. It rests on something
                // bots cannot do: it assumes the tank has turned him away from everyone else,
                // because Goblin Dragon Gun fires 21910 once a second for eight seconds and
                // 21910 is a ten yard *cone* for a hundred and fifty seven to a hundred and
                // ninety two a tick. A bot chooses its standoff bearing from wherever it already
                // happens to be, so a caster walked into melee is inside that cone about as
                // often as not, and eight ticks of it is more than a level fifty caster's entire
                // health pool.
                //
                // What stacking actually buys is narrower than the advice suggests. Of his four
                // abilities only Bomb is denied by it -- a two second cast at a random hostile
                // with a five yard minimum range, dealing weapon damage in a five yard splash.
                // Shoot has the same minimum but is aimed at his current victim, so the tank
                // being in melee already denies it; and Flash Bomb reaches twenty yards with no
                // minimum at all, so nothing denies it. Trading eight ticks of cone for one
                // modest bomb is a bad trade, and twelve yards is outside the cone.
                /* rangedStandoff */ MARA_TEN_YARD_AOE_STANDOFF,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                /* healsBelueprint */ 0.0f,
            },

            // The trash that wants the same twelve yards, for the same reason: a ten yard area
            // effect on a repeat, on something the party will fight a dozen times on the way
            // through Earth Song Falls.
            {
                // Sonic Burst: ten yards, ten seconds of silence, every ten to fourteen. They
                // come in linked groups that cannot be pulled singly, so a silenced healer here
                // is a silenced healer with three of them on the tank.
                NPC_SUBTERRANEAN_DIEMETRADON,
                MARA_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f,
            },
            {
                // Aqua Jet: ten yards, damage and a knockback, every twelve to eighteen. The
                // knockback is the part that matters -- they wander loose around Earth Song
                // Falls, and being thrown here is being thrown into whatever else is wandering.
                NPC_THESSALA_HYDRA,
                MARA_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f,
            },
            {
                // Trample, the same ten yard hit Landslide has, every eight to thirteen seconds.
                // Sixteen of them stand between the group and Princess Theradras, in ones and
                // then in pairs.
                NPC_PRIMORDIAL_BEHEMOTH,
                MARA_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f,
            },
        },

        // No gauntlet in this instance and so no ground to hold. Every fight in it is fought
        // where the group finds it.
        /* holdLines */ {},

        // The turtles, which are the Sul'lithuz Broodlings of this instance.
        //
        // Stolid Snapjaws are faction 7 and stand around the approach to Princess Theradras
        // minding their own business; the guide's warning about them is that a stray area effect
        // turns twenty six neutral level forty six mobs into a second pull. They are not
        // something the party has to fight and there is no reason for a tank to peel one or for
        // a damage dealer to prefer one, so nothing here engages them. A bot that one of them
        // actually attacks still defends itself.
        /* ignoreEntries */ { NPC_STOLID_SNAPJAW },

        // Noxious Cloud, from Noxious Slime's death and from Creeping Sludge on a timer. See the
        // field's own comment in DungeonTactics.h for why this is the instance that needed it.
        /* groundHazardSpellIds */ { SPELL_NOXIOUS_CLOUD },
    },

    {
        MAP_SUNKEN_TEMPLE,
        "Sunken Temple",

        // Three sources, each confirmed by mechanic in spell_template rather than by name, and
        // between them most of the instance.
        //
        // Atal'ai Deathwalker casts Fear (12096, mechanic 5) every fourteen to twenty seconds and
        // there are eighteen of them, mostly on the way to and around Jammal'an -- whose zone pull
        // brings every one still standing. Nightmare Wyrmkin casts Sleep (12098, mechanic 10) every
        // twenty three to twenty seven seconds on the dragonkin floor. Hukku's Succubus casts
        // Seduction (6358, mechanic 1) and Hukku summons a fresh one every fifteen to eighteen.
        //
        // Not on the list, so nobody counts on the totem for them: Hex of Jammal'an ends in 12483,
        // a charm with no mechanic and no dispel type, and the Avatar's Cause Insanity (12888) is a
        // charm with no mechanic either. Tremor removes neither. Cause Insanity is a magic dispel,
        // which the existing dispel path already handles for a charmed party member.
        /* wantsTremorTotem */ true,

        // The Shade of Hakkar is what the Avatar event protects, but it is faction 35 and nothing
        // attacks it -- the Suppressors channel at it rather than fight it -- so the escort defence,
        // which keys off a creature's victim and attacker list, would never fire. The answer to the
        // event is the kill order below, not this field.
        /* escortNpcEntries */ {},
        /* escortGuardRadius */ 0.0f,

        // Written in kill order.
        /* focusFirst */
        {
            // The one mob in the instance whose survival loses something other than health.
            //
            // The Shade of Hakkar summons one every sixty to a hundred and ten seconds at one of the
            // two doors; it walks to its point and casts Suppression (12623) on the Shade, and three
            // of those fail the event. It is a level fifty elite with health multiplier four, so it
            // is not quick, and it arrives while the room is already full of Hakkari Minions --
            // which is exactly when the group's own target selection would pick on health and guid.
            NPC_NIGHTMARE_SUPPRESSOR,

            // Stationary summons, ahead of everything that walks. The same shape and the same reason
            // as Zul'Farrak's Ward of Zum'rah and Greater Healing Ward: each one out-produces the
            // group for as long as it stands, each one has a health multiplier of 0.01, and none of
            // them ever appears in an attacker list, so without an entry nothing turns for them.
            //
            // Mijan's Healing Ward V summons the same 8179 Greater Healing Ward Antu'sul uses, three
            // second pulse, on a fifteen to twenty second repeat.
            NPC_GREATER_HEALING_WARD,
            // Zolo's Atal'ai Totem fires 12504 every five seconds, which raises an Atal'ai Skeleton.
            // He recasts it every ten to twelve, so the totem is the source and the skeletons are
            // the symptom.
            NPC_ATALAI_TOTEM,
            // Jammal'an's, every eight to fifteen seconds. Earthgrab roots everything within ten
            // yards -- see the Zul'Farrak entry for why a rooted melee bot is worse than a slow one.
            NPC_EARTHGRAB_TOTEM,

            // The healers, in order of how often they heal.
            //
            // High Priest first and not close: Heal (12039, three second cast) on a five to eight
            // second repeat at whichever friend is most hurt, Renew on top, a skeleton every minute,
            // and Shadow Shield at thirty percent. There are two, and Jammal'an's zone pull brings
            // both of them if they are still standing.
            NPC_ATALAI_HIGH_PRIEST,
            // Heal every ten to twenty seconds and Shadow Shield on the minute.
            NPC_ATALAI_PRIEST,
            // Healing Wave every twenty five to thirty five seconds and Hex, and twenty eight of
            // them -- they are in more packs than any other caster here, which is why they are on
            // the list despite healing least often.
            NPC_ATALAI_WITCH_DOCTOR,

            // Jammal'an ahead of Ogom the Wretched, whom he is fought beside. Jammal'an heals
            // friends with a thousand health missing every three to ten seconds and himself below
            // eighty percent, and he is the source of the hex, the totem and the Flamestrike. Ogom
            // has none of that -- Shadow Bolt, Shadow Word: Pain, Curse of Weakness -- and a group
            // that starts on Ogom watches Jammal'an heal him. Ogom is left off the list so that he
            // is simply what remains.
            NPC_JAMMALAN_THE_PROPHET,

            // The Avatar event's other summon, behind the Suppressor and nothing else in it. The
            // Bloodkeeper carries the Hakkari Blood that puts out the Eternal Flames, so the event
            // cannot progress until one dies, but unlike the Suppressor it is not on a clock.
            NPC_HAKKARI_BLOODKEEPER,

            // The skeletons themselves, from Zolo's totem and from the High Priests. Level forty six
            // non-elites against a group in the fifties, so a couple of swings each, and they land
            // on whoever is nearest. Behind everything that makes them.
            NPC_ATALAI_SKELETON,

            // Deliberately absent: Hukku's three pets. They are summons under a burn gate below,
            // which is the opposite instruction.
        },

        /* creatures */
        {
            {
                NPC_ATALALARION,
                // Twenty five, outside Ground Tremor. See ST_ATALALARION_STANDOFF.
                //
                // Sweeping Slam, his other cast, is a five yard frontal cone with a knockback, which
                // only ever reaches the tank. No anchor for it: his room is unmeasured, and the
                // Landslide note in the Maraudon entry is why a guessed one is worse than none.
                /* rangedStandoff */ ST_ATALALARION_STANDOFF,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                /* healsBelowPercent */ 0.0f,
            },
            {
                NPC_JAMMALAN_THE_PROPHET,

                // No standoff. Flamestrike is a three second cast at a random player and Hex of
                // Jammal'an a single target, so there is no distance that answers either.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},

                // A hundred, meaning from the pull. His friendly heal is gated on Ogom's health
                // rather than his own -- creature_ai_events 571005, any friend a thousand health
                // down, every three to ten seconds -- so there is no point in his own bar below which
                // the interrupt becomes worth holding. Healing Wave (12492) is a three second cast.
                /* healsBelowPercent */ 100.0f,
            },
            {
                NPC_MIJAN,
                // The healer of the six Protectors. Renew is a two second cast in this database and
                // Healing Wave three, on ten to twenty second repeats between them, from the pull.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                /* healsBelowPercent */ 100.0f,
            },
            {
                NPC_HUKKU,

                // Shadow Bolt Volley is radius index 10, thirty yards, so as with Zum'rah there is
                // no distance that is outside it and inside a caster's own range.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,

                // A hundred: never leave him for a pet. This is the Celebras shape rather than the
                // Antu'sul one.
                //
                // Hukku's Guardians (12790) summons a Voidwalker, a Succubus and an Imp at once, eight
                // to ten seconds after the pull and every fifteen to eighteen after that. Being an
                // NPC caster, nothing unsummons the previous three -- EffectSummonGuardian only does
                // that for players -- so the count only goes up, and a group that turns for each set
                // is clearing three non-elites every fifteen seconds while he makes three more. The
                // number of pets the group ends up fighting is set by how long Hukku lives, so the
                // shortest fight is the one where he dies first.
                //
                // They do not vanish when he does -- a guardian whose owner is dead only despawns
                // once it is out of combat -- so the pets are still the next fight. They are just a
                // finite one.
                /* burnBelowPercent */ 100.0f,
                /* summonEntries */ { NPC_HUKKUS_VOIDWALKER, NPC_HUKKUS_SUCCUBUS, NPC_HUKKUS_IMP },
                /* healsBelowPercent */ 0.0f,
            },
            {
                NPC_ATALAI_HIGH_PRIEST,
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                // Heal on a five to eight second repeat with no health gate: from the pull.
                /* healsBelowPercent */ 100.0f,
            },
            {
                NPC_ATALAI_PRIEST,
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                /* healsBelowPercent */ 100.0f,
            },
            {
                NPC_ATALAI_WITCH_DOCTOR,
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                /* healsBelowPercent */ 100.0f,
            },

            // The four green dragonkin. Morphaz and Hazzas stand guard by Eranikus; Weaver and
            // Dreamscythe are hidden until Jammal'an dies and then patrol in. All four share one
            // book: Acid Breath (12884), a ten yard frontal cone with an armor-reducing DoT, and
            // Wing Flap (12882), a ten yard cone with a knockback. Cones are the tank's problem only
            // if the tank has turned the dragon away from everyone else, and the Gizlock note in
            // the Maraudon entry is why that cannot be assumed of a bot.
            { NPC_MORPHAZ,    ST_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },
            { NPC_HAZZAS,     ST_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },
            { NPC_WEAVER,     ST_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },
            { NPC_DREAMSCYTHE, ST_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f },

            // Trash with a ten yard area effect on a repeat.
            {
                // Trample, every ten to twenty four seconds. Seventeen of them in the flooded
                // central pit, which is where a group crossing between the upper ring and the lower
                // floor spends its time.
                NPC_DEEP_LURKER,
                ST_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f,
            },
            {
                // Frost Nova, a ten yard root every sixteen to twenty nine seconds. Its Frostbolt
                // Volley is twenty yards and nothing short of leaving range avoids it; the nova is
                // the part a standoff buys, and a rooted healer cannot walk out of anything else.
                NPC_HAKKARI_FROSTWING,
                ST_TEN_YARD_AOE_STANDOFF, 0, 0.0f, 0.0f, 0.0f, 0.0f,
            },
        },

        // No gauntlet. The Avatar event is fought around the altar with adds arriving from both
        // doors, which is the Razorfen Downs shape -- a room fought across rather than a choke held.
        /* holdLines */ {},

        // Nothing to ignore. The non-elite Atal'ai Slaves and Slime Maggots are hostile and fight
        // back, unlike Maraudon's neutral turtles.
        //
        // The Atal'ai Deathwalker's Spirit was considered and left off. It is an elite summoned by
        // every Deathwalker's death that zone-aggroes and despawns after fifteen seconds, so damage
        // on it is wasted -- but an ignored creature is also one the tank will not peel, and a level
        // fifty elite loose on the healer for fifteen seconds is worse than the wasted damage.
        /* ignoreEntries */ {},

        // None. Jammal'an's Flamestrike is the only persistent area aura in the instance and its
        // ground effect is twenty four fire damage every two seconds for eight -- under a hundred
        // in all, against Noxious Cloud's three thousand. Walking out costs more than standing in it.
        /* groundHazardSpellIds */ {},
    },

    {
        MAP_MOLTEN_CORE,
        "Molten Core",

        // Magmadar's Panic, and nothing else in the instance comes close to justifying it.
        //
        // Panic (19408) is an area fear -- SPELL_AURA_MOD_FEAR, confirmed off spell_template
        // rather than off its name -- on a thirty second repeat, and a feared raid in Magmadar's
        // room runs into the packs on either side of it. That is the whole case, and it is worth
        // the earth totem slot for the run even though only one fight in ten spends it.
        //
        // He is also the reason this was first written the other way round. Magmadar has no
        // script at all: he is EventAI driven from spell list 119820, so a search of
        // src/scripts/**/molten_core for a fear finds nothing and the instance looks fear-free.
        // The fight that every guide cites for Tremor here is Lucifron's Dominate Mind, and that
        // one really is absent -- 20604 is a charm in spell_template and nothing in the source
        // casts it, on this server or on CMaNGOS. So the guides point at the wrong boss, and the
        // scripts do not mention the right one.
        /* wantsTremorTotem */ true,

        /* escortNpcEntries */ {},
        /* escortGuardRadius */ 0.0f,

        // The Flamewaker Priest is this instance's Druid of the Fang: a healer standing in a pack
        // of things that are not healers, and the one mob in it that undoes the group's work on
        // whatever is beside it. Read off its spell list, 116620, rather than off the boss it
        // belongs to -- boss_sulfuron_harbinger.cpp says "Adds NYI" and does not drive them at all.
        /* focusFirst */ { NPC_FLAMEWAKER_PRIEST },


        /* creatures */
        {
            {
                NPC_FLAMEWAKER_PRIEST,
                // No standoff and no sight break. Dark Mending is the cast worth stopping and it
                // is stopped by interrupting it, not by standing somewhere else.
                /* rangedStandoff */ 0.0f,
                /* breakSightSpellId */ 0,
                /* anchor */ 0.0f, 0.0f, 0.0f, 0.0f,
                /* burnBelowPercent */ 0.0f,
                /* summonEntries */ {},
                // A hundred, because it heals from the pull rather than from a threshold. Dark
                // Mending is the first slot of its list on a flat five second repeat with no
                // health gate anywhere in it, so there is no point below which the interrupt
                // reserve should switch on: it is worth holding a global from the opening cast.
                /* healsBelowPercent */ 100.0f,
            },
        },

        // None, and this is the field it was tempting to use.
        //
        // Golemagg's Core Ragers leash him: boss_golemagg.cpp checks every three seconds whether
        // a Rager has been taken more than a hundred yards from him and sends him to evade if one
        // has, which resets the encounter. A hold line centred on his spawn at (793.2 -998.3
        // -206.8) would bound that, and it is not written here because a hold line is a measured
        // patch of floor and this one has not been measured -- an unmeasured radius in a room
        // this size pens bots against a wall or off a ledge, which costs more than it saves. The
        // ignore list below removes the only way a bot was going to move a Rager in the first
        // place, so the leash is covered until the room is walked.
        /* holdLines */ {},

        // Core Ragers, which cannot be killed and must not be dragged.
        //
        // The clearest case for this field in the game so far. boss_golemagg.cpp zeroes all
        // incoming damage on a Rager below fifty percent and heals it back to full for as long as
        // Golemagg is alive, and when he dies KillAdds despawns both of them -- so a Rager is
        // never worth a single global cooldown at any point in the encounter. A group left to its
        // own target selection parks on one: it is an elite standing next to the boss, it is
        // hitting people, and every rule the bots have for picking a target says fight it.
        //
        // The second half is worse than the wasted damage. Moving one is what resets the fight,
        // and a bot that has decided to fight a Rager is a bot that will chase it.
        /* ignoreEntries */ { NPC_CORE_RAGER },
    },
};

} // namespace

bool DungeonTactics::GetFightAnchor(uint32 creatureEntry, float& x, float& y, float& z,
                                    float& radius) const
{
    for (auto const& tactic : creatures)
    {
        if (tactic.creatureEntry != creatureEntry || tactic.anchorRadius <= 0.0f)
            continue;

        x = tactic.anchorX;
        y = tactic.anchorY;
        z = tactic.anchorZ;
        radius = tactic.anchorRadius;
        return true;
    }

    return false;
}

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

uint32 DungeonTactics::GetFocusRank(uint32 creatureEntry) const
{
    for (size_t i = 0; i < focusFirst.size(); ++i)
        if (focusFirst[i] == creatureEntry)
            return uint32(i) + 1;

    return 0;
}

bool DungeonTactics::IsEscortNpc(uint32 creatureEntry) const
{
    for (uint32 entry : escortNpcEntries)
        if (entry == creatureEntry)
            return true;

    return false;
}

DungeonHoldLine const* DungeonTactics::GetHoldLineAt(float x, float y, float z) const
{
    for (auto const& line : holdLines)
    {
        if (z < line.minZ)
            continue;

        float const dx = x - line.x;
        float const dy = y - line.y;
        if ((dx * dx + dy * dy) > (line.radius * line.radius))
            continue;

        return &line;
    }

    return nullptr;
}

uint32 DungeonTactics::GetEscortRank(uint32 creatureEntry) const
{
    for (size_t i = 0; i < escortNpcEntries.size(); ++i)
        if (escortNpcEntries[i] == creatureEntry)
            return uint32(i) + 1;

    return 0;
}

DungeonHoldLine const* DungeonTactics::GetHoldLineToRecover(float x, float y, float z) const
{
    // Inside one already, so there is nothing to recover from. Asked first rather than folded into
    // the loop below, because a position can be inside one zone and within recovery range of
    // another, and walking a bot out of ground it is correctly holding would be the worst possible
    // reading of a rule whose whole purpose is to keep it there.
    if (GetHoldLineAt(x, y, z))
        return nullptr;

    for (auto const& line : holdLines)
    {
        if (line.recoverRadius <= 0.0f)
            continue;

        float const dx = x - line.x;
        float const dy = y - line.y;
        float const dz = z - line.z;
        if ((dx * dx + dy * dy + dz * dz) > (line.recoverRadius * line.recoverRadius))
            continue;

        return &line;
    }

    return nullptr;
}

DungeonTactics const* GetDungeonTactics(uint32 mapId)
{
    for (auto const& tactics : g_dungeonTactics)
        if (tactics.mapId == mapId)
            return &tactics;

    return nullptr;
}

// The creature whose health gates whether this summon is worth turning for, or zero when nothing
// gates it. The caller finds that creature and reads its health; the table cannot see the world.
// Whether this entry is one of the elite adds a creature summons, rather than something
// stationary. The distinction the table already carries: totems and wards sit in focusFirst and in
// nobody's summonEntries, because they are things anything can walk up and break, while a Servant
// of Antu'sul is a level forty eight elite that will kill whatever is sent at it alone.
bool DungeonTactics::IsSummonedAdd(uint32 creatureEntry) const
{
    for (DungeonCreatureTactic const& tactic : creatures)
        for (uint32 entry : tactic.summonEntries)
            if (entry == creatureEntry)
                return true;

    return false;
}

float DungeonTactics::GetHealWatchPercent(uint32 creatureEntry) const
{
    for (DungeonCreatureTactic const& tactic : creatures)
        if (tactic.creatureEntry == creatureEntry)
            return tactic.healsBelowPercent;

    return 0.0f;
}

bool DungeonTactics::IsGroundHazard(uint32 spellId) const
{
    for (uint32 id : groundHazardSpellIds)
        if (id == spellId)
            return true;

    return false;
}

bool DungeonTactics::IsIgnoredByParty(uint32 creatureEntry) const
{
    for (uint32 entry : ignoreEntries)
        if (entry == creatureEntry)
            return true;

    return false;
}

uint32 DungeonTactics::GetBurnGateFor(uint32 summonEntry, float& belowPercent) const
{
    for (DungeonCreatureTactic const& tactic : creatures)
    {
        if (tactic.burnBelowPercent <= 0.0f)
            continue;

        for (uint32 entry : tactic.summonEntries)
        {
            if (entry == summonEntry)
            {
                belowPercent = tactic.burnBelowPercent;
                return tactic.creatureEntry;
            }
        }
    }

    return 0;
}
