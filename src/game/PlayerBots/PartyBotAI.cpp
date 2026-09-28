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
#include "AI/CreatureEventAIMgr.h"
#include "ScriptMgr.h"
#include "Database/DBCStores.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "Packets/Loot.h"
#include "Geometry.h"
#include "Maps/PathFinder.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "DynamicObject.h"
#include "Utilities/Random.h"

#include <random>
#include <unordered_map>
#include <algorithm>

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
//
// And they were far wider than that, because the paragraph above measures the overshoot in
// points of threat and the constants are a fraction of the tank's whole pool. Those are not the
// same quantity and they diverge as the fight runs: against a tank on five thousand threat,
// "fifteen points" became a thousand. Subtracted from the 1.10 melee pull ratio it left melee
// capped at ninety percent of the tank forever -- below the tank, not just below the pull.
//
// The measured cost, from the Antu'sul attempt that stalled at forty seven percent: the rogue
// sat at one hundred energy with the global cooldown free, standing behind the boss, and cast
// nothing. Thirteen Sinister Strikes and five Backstabs in eighty seconds against a rotation
// that should manage nearer forty. PickRankForThreat was refusing every rank of every ability
// because the whole rogue was over a ceiling drawn at ninety percent of a rage starved tank.
// The bot looked idle and was in fact obeying a rule about threat.
//
// Set to what melee and ranged actually run at when a real group is holding a boss: just over
// the tank for melee, comfortably under the 1.30 flip for ranged. The pull ratios above are
// untouched, so this still cannot take a mob off a tank that is doing its job.
static constexpr float PB_THREAT_HEADROOM_MELEE = 0.05f;
static constexpr float PB_THREAT_HEADROOM_RANGED = 0.10f;
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

// How often a bot reports that threat is holding its rotation back. The refusals themselves are
// counted every time; this only governs how often the running total is written out.
static constexpr time_t PB_THREAT_LOG_INTERVAL = 3;
// Rage, in the tenths the field is stored in. Below the first a tank has too little to run its
// list at all and reaches for Bloodrage. The second is a floor under Shield Block, which costs
// rage without using the global cooldown, and it exists so that spending there can never be what
// leaves Shield Slam short: Shield Slam is twenty, so anything taken outside the cooldown has to
// leave that behind it.
//
// There was a third for the Heroic Strike and Cleave dump. It is gone, because a tuned number was
// the wrong tool: sixty rage scaled to forty four at level thirty four and no tank ever came close
// to it. That floor now comes from the spell costs themselves, at the dump's own call site.
static constexpr uint32 PB_TANK_RAGE_LOW = 200;

// Shield Block waits for a real surplus now, because on a rage starved tank it was eating the
// threat budget. The Antu'sul attempt that stalled has the tank cast Shield Block seven times
// and Demoralizing Shout four -- a hundred and ten rage on mitigation and a debuff -- against
// three Sunder Armors in ninety seconds, finishing on 695 damage and about a thousand threat.
// Everything downstream of that number was throttled by it: the melee ceiling is a share of the
// tank's threat, so a tank that cannot build threat caps the whole group's damage.
static constexpr uint32 PB_TANK_RAGE_BLOCK = 450;

// How often a rogue with nothing to poison its weapons with says so.
static constexpr time_t PB_POISON_LOG_INTERVAL = 60;

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
// How far back each job trails, and how much room it gets of its own.
//
// Melee close, because they have to reach the fight anyway. Ranged and healers further out and to
// the flanks: a caster with nowhere to stand is a caster inside every area effect aimed at the
// melee, and a healer stacked on the tank is one that dies to the same cleave. The spread is a slot
// per bot rather than a fresh random roll per call - a roll can hand two bots the same spot, and
// re-rolling it on every reposition means the formation never settles.
static constexpr float PB_FORMATION_MELEE_DIST = 4.0f;
static constexpr float PB_FORMATION_RANGED_DIST = 9.0f;
static constexpr float PB_FORMATION_HEALER_DIST = 8.0f;
// Widened while fighting, when standing apart is worth most and nobody is walking through a
// doorway single file.
static constexpr float PB_FORMATION_COMBAT_BONUS = 3.0f;
// The widest a slot may sit off directly behind the leader. Wider than the old spread, and still
// inside the rear arc for the reason the arc exists: abreast of the leader is a second aggro
// radius dragged along the wall.
static constexpr float PB_FORMATION_MAX_OFFSET = 1.15f;
// How much a slot tightens each time the spot it asks for turns out to be one that would wake
// something up. Three tries and it gives up and tucks in behind, which is where it used to stand
// all the time.
static constexpr float PB_FORMATION_TIGHTEN_STEP = 0.35f;
static constexpr uint32 PB_FORMATION_TIGHTEN_TRIES = 3;
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

// How much mana a buffer keeps back for the fight rather than spending on the buffs themselves.
//
// Group buffs are charged per target and the good ranks are not cheap: Power Word: Fortitude rank
// five is seven hundred and forty four mana, so a level forty two priest buffing five people
// spends more than its whole pool on one spell. Logged doing exactly that -- 3739 mana down to 762
// in six seconds, five casts, and the last of them landed on a player at a third health who wanted
// a heal instead.
//
// Stopping here does not skip the buff. Out of combat the bot drinks back to full and finishes on
// the next pass, so the only thing given up is doing it all in one breath.
static constexpr float PB_BUFF_MANA_FLOOR = 55.0f;

// How far a pet is sent to break a totem. Generous, because the totem is usually at the boss and
// the owner is usually not, and a pet that will not cross the room is no use for this.
static constexpr float PB_PET_TOTEM_RADIUS = 40.0f;

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

// How far to look for a controlled mob left over from a finished fight, and how often. The radius
// is wide because the mob may have wandered while sheeped; the interval keeps the search off every
// idle tick.
static constexpr float PB_LEFTOVER_SEARCH_RADIUS = 60.0f;
static constexpr time_t PB_LEFTOVER_SCAN_INTERVAL = 1;

// Throttle for the pet order log. The order itself is only issued on a target mismatch, but a
// refused order leaves the mismatch in place, so an order that is not taking effect repeats at
// tick rate -- which is the case most worth reading and the one that would flood the file.
static constexpr time_t PB_PET_LOG_INTERVAL = 2;

// How long the current target has left to live, estimated from how fast its health is actually
// falling rather than from any property of the target itself.
//
// Every rotation that has to choose between a small effect now and a larger one later is really
// choosing between the two against a deadline, and the deadline is the only part neither the
// spell data nor the creature template knows: the same boar dies in four seconds to a full party
// and forty to a lone healer. Measuring it costs one subtraction a tick and replaces a pile of
// rules about health percentages and creature rank that were each wrong about a different fight.
static constexpr uint32 PB_TTL_SAMPLE_INTERVAL_MS = 500;
// Weight on the newest sample. Low enough that one unlucky second of misses does not convince a
// bot the fight has stalled, high enough to follow a real change within a few seconds.
static constexpr float PB_TTL_SMOOTHING = 0.35f;
// What to report before enough of a fight has been watched to say anything, and what to report
// for a target nothing is damaging. Long, so a bot with no evidence behaves as it would on a
// fight worth investing in: assuming the mob is nearly dead spends the opening on the wrong thing
// every time, and a bot that has just acquired a target is in exactly that position.
static constexpr float PB_TTL_UNKNOWN_SECONDS = 3600.0f;

// A global cooldown, the floor under what one more combo point can possibly cost however full the
// energy bar is.
static constexpr float PB_ROGUE_GCD_SECONDS = 1.5f;
// A full bar. Another builder past this overcaps and the point is thrown away, so a rogue holding
// five has no reason left to wait whatever the fight is doing.
static constexpr uint32 PB_ROGUE_MAX_COMBO = 5;
// Below this a finisher is worse than the builder it replaces: one point of Eviscerate spends
// thirty five energy to do less than the Sinister Strike that would have made a second point.
static constexpr uint32 PB_ROGUE_FINISHER_MIN_COMBO = 2;
// Margin on the time-to-live comparison, covering the global cooldown the finisher itself has to
// wait out. Without it a rogue decides it has just enough time, starts building, and watches the
// mob die on the cast it was one tick short of.
static constexpr float PB_ROGUE_FINISHER_LEAD_SECONDS = 1.5f;
// How long a fight has to promise before Slice and Dice is worth a bar of combo points. It buys
// attack speed for the rest of the fight and no damage at all up front, so on anything dying
// inside its own shortest duration it is a finisher that did nothing. This is what keeps it off
// dungeon trash, where being gated on combo points alone had it beating Eviscerate to every point
// a rogue ever earned.
static constexpr float PB_ROGUE_SND_MIN_FIGHT_SECONDS = 12.0f;
// Points to put behind Slice and Dice once a fight has earned it. Its duration scales with them,
// so buying it cheap means buying it again, and every refresh is a bar not spent on damage.
//
// Four was unreachable, which made the whole thing dead code rather than conservative. Seven
// rogues across two runs reached four points on between half a percent and nine percent of their
// engaged ticks, and four points together with a fight long enough to want the haste on nought to
// two percent - so Slice and Dice was cast once in five hundred casts by one rogue and never at
// all by two others. The fight-length gate above was the part that was doing the real work.
//
// Three buys fifteen seconds, which comfortably covers the twelve second floor, and is reached
// often enough to land once on every fight that lasts. Note this is a floor and not a target: the
// bar keeps building when the fight is long enough to spend it twice.
static constexpr uint32 PB_ROGUE_SND_MIN_COMBO = 3;
// How little Slice and Dice may have left before it is worth refreshing early. Waiting for it to
// lapse means the haste is already gone by the time the rogue starts paying for it again.
static constexpr float PB_ROGUE_SND_REFRESH_SECONDS = 3.0f;
// Rupture wants a fight that comfortably outlasts it rather than merely outlasts it, which is
// what keeps it off the mob that dies two ticks into it.
static constexpr float PB_ROGUE_RUPTURE_FIGHT_MULTIPLE = 1.25f;

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

// How far the mob has to move before the close-in walk is re-aimed at it. Below this the walk is
// left alone, since re-issuing a point move restarts the path and a mob shifting a yard is not
// worth a fresh route.
static constexpr float PB_PULL_CLOSE_REAIM = 5.0f;

// How far a bot backs off when it flees melee. Shared by the move and by the check that runs
// ahead of it, so the position tested is always the position taken.
static constexpr float PB_DISTANCING_RANGE = 15.0f;

// How close a hostile creature may get to a caster or healer before it walks away from it. Set at
// the melee floor rather than at the standoff it would like, so this fires only when the bot is
// genuinely in the swing and not every time a mob drifts a yard nearer than ideal.
static constexpr float PB_CASTER_MELEE_FLOOR = 8.0f;

// How far out HoldTacticalStandoff looks for a creature whose standoff this bot is inside. The
// largest standoff in the table is twenty five, so this only has to cover that plus the distance
// a bot could be from something it is not yet outside of.
static constexpr float PB_STANDOFF_SCAN = 40.0f;
// How much past the asked-for standoff the bot aims, so that the next knockback or the next step
// of the chase does not put it straight back inside. Small, because every yard past the standoff
// is a yard closer to being out of its own range.
static constexpr float PB_STANDOFF_MARGIN = 2.0f;
// And the outer bound on where it may end up. A caster's own spells reach thirty; walking to
// thirty five to be safe is walking out of the fight.
static constexpr float PB_STANDOFF_CEILING = 29.0f;
// The longest walk worth making for it, and the shortest gap between two of them.
static constexpr float PB_STANDOFF_MAX_TRAVEL = 18.0f;
static constexpr time_t PB_STANDOFF_INTERVAL = 4;
// How far inside the band is worth walking for. Without this the rule thrashes: one measured
// fight has a hunter walk out five times in twenty two seconds, and one of those was from 23.3
// yards of a twenty five yard band -- a gain of one and a half yards, paid for with a cast. The
// band exists to keep a bot out of an area effect, and a bot a yard inside the edge of one is
// not where the damage is.
static constexpr float PB_STANDOFF_DEADBAND = 3.0f;

// How close to the tank a healer that cannot escape wants to be before it stops walking. Inside
// this it is already in the tank's lap and whatever is chasing it is in taunt range; outside it,
// the healer is a second place for the boss to stand and every taunt keeps being undone.
static constexpr float PB_HEALER_TANK_HUDDLE_RANGE = 8.0f;

// How far out a bot looks for the cast it is supposed to hide from. Wide enough to cover any
// single target nuke a boss has, and it is only ever asked on a map whose tactics name such a
// spell, so the sweep is not something an ordinary fight pays for.
static constexpr float PB_BREAK_SIGHT_SCAN = 50.0f;
// How much cast has to be left before walking out of sight is worth starting. Under this the bot
// arrives as the spell lands, having given up its position for nothing.
static constexpr uint32 PB_BREAK_SIGHT_MIN_WINDOW_MS = 1200;
// What fraction of the remaining cast is treated as usable for walking. The rest pays for the
// bot's own reaction, for a path that is longer than the straight line, and for the spline taking
// a moment to start.
static constexpr float PB_BREAK_SIGHT_TIME_BUDGET = 0.6f;
// The shortest move worth making. Below this the bot is shuffling on the spot and cover that
// close to where it already stands is cover it was already behind.
static constexpr float PB_BREAK_SIGHT_MIN_MOVE = 4.0f;
// The health below which a bot stops dodging and stands where it can be healed. A dodge costs
// nothing but position, right up to the point where position is what the heal needs, and a bot
// that ducks behind a wall at a tenth of its health has traded one hit it would have survived for
// a heal it will not.
static constexpr float PB_BREAK_SIGHT_MIN_HEALTH = 35.0f;
// The longest a walk to cover may be the only thing a bot is doing. Set past the longest cast
// worth dodging plus the time to cross the widest ring the search will offer, so it never cuts a
// genuine walk short, and short enough that a walk which is never going to arrive costs one dodge
// rather than the fight.
static constexpr uint32 PB_BREAK_SIGHT_HOLD_MS = 4000;

// How far out a bot looks for a patch of hostile ground. A dynamic object is only ever a problem
// while the bot is standing in it, and the widest one this table names is five yards across, so
// this is a sweep for the thing underfoot rather than a survey of the room. Fifteen so that a
// cloud dropped a step away is already known about by the time the bot drifts into it.
static constexpr float PB_GROUND_HAZARD_SCAN = 15.0f;
// How much further out of the patch than its own edge the bot aims for. The cloud does not move,
// but the bot's spline overshoots and stops a little short by turns, and a step that ends on the
// boundary is a step that spent a tick and is still being damaged.
static constexpr float PB_GROUND_HAZARD_MARGIN = 3.0f;
// The longest step out worth taking. Short, because the thing being escaped is small and the
// fight is still going on: a bot that walks twenty yards out of a five yard cloud has left the
// fight to avoid a tick of damage.
static constexpr float PB_GROUND_HAZARD_MAX_TRAVEL = 14.0f;
// The longest a walk out may be the whole of a tick, for the same reason PB_BREAK_SIGHT_HOLD_MS
// exists: a walk that cannot finish has to release the bot rather than freeze it.
static constexpr uint32 PB_GROUND_HAZARD_HOLD_MS = 2500;

// How long after the pull rule declines a route a gap closer stays off the table. The rule is asked
// again on every recomputed path, so a bot that is still being held keeps refreshing this, and one
// that has genuinely been let through stops. Long enough to outlast the gap between two of those
// recomputes and short enough that a bot released mid-chase is not still declining to hurry a
// second later.
static constexpr uint32 PB_SPRINT_AFTER_REFUSAL_MS = 2000;


// How badly a hostile cast wants taking away. Ordered, and compared against, so the gaps between
// them carry no meaning beyond the ordering.
enum PbInterruptPriority : uint32
{
    PB_INTERRUPT_NONE = 0,
    // Something the mob is doing to itself. Lowest of the tiers that are worth anything, so an
    // interrupt is spent here only when the mob is not known to have worse in its book, and never
    // in preference to a heal or a crowd control.
    PB_INTERRUPT_BUFF = 1,
    PB_INTERRUPT_DAMAGE = 2,
    // A heal small enough that taking it away is not worth a cooldown the caster's own bigger heal
    // is about to need. Antu'sul is the case that named this tier: below sixty percent he casts
    // Flash Heal for about five hundred every seventeen seconds, and below thirty he casts Healing
    // Wave of Antu'sul for about two thousand four hundred every twelve, on a separate timer. Both
    // scored HEAL, so a group with two interrupts spent them both on the Flash Heals and the Wave
    // -- a third of his health bar, on a twelve second repeat -- landed unopposed every time.
    //
    // Judged against the caster's own maximum health rather than against a flat number, because
    // the question is what fraction of the fight the heal undoes, and that is the only form of it
    // that carries from one dungeon to the next.
    PB_INTERRUPT_MINOR_HEAL = 3,
    PB_INTERRUPT_HEAL = 4,
    PB_INTERRUPT_CONTROL = 5,
};

// The share of a caster's own health bar a heal has to return before it outranks everything else a
// cooldown could be spent on. Below it the heal is real but small, and the hold in
// SelectInterruptTarget keeps the interrupt back for whatever bigger heal the mob is known to own.
float const PB_INTERRUPT_BIG_HEAL_SHARE = 0.15f;

// How far off a caster can be and still be worth keeping the energy or rage to stop.
float const PB_INTERRUPT_WATCH_RADIUS = 30.0f;

// How close to the intended target a held mob has to be before an ability that picks its own extra
// targets is worth withholding. Generous on purpose: the cost of holding one Multi-Shot is a
// fraction of one global cooldown, and the cost of waking the crowd control is the whole of it.
float const PB_AOE_CC_SAFETY_RADIUS = 15.0f;

// The band Charge works in. Read as plain numbers rather than out of the spell, because the
// minimum is not stored on it: a spell's range index carries the maximum, and Charge's eight yard
// floor is enforced in the cast handler.
// The cooldown past which a crowd control is a once-per-fight resource rather than a rotational
// one, and so is worth keeping for the target the instance names.
// The share of a healer's mana below which absorbs stop being worth buying, and every remaining
// point goes into healing that has already been needed rather than damage that might not arrive.
float const PB_SHIELD_MANA_FLOOR = 40.0f;

uint32 const PB_CC_LONG_COOLDOWN_MS = 60000;

// How far to look for the boss that summoned an add, when deciding whether the add is the one
// worth spending that cooldown on. Generous: the summon can land anywhere in the room.
float const PB_CC_SUMMONER_SEARCH_RADIUS = 100.0f;

// How long the whole feign-and-trap sequence gets before it is abandoned. Two casts and a global
// cooldown, with room for one missed tick -- long enough to complete, far too short to be mistaken
// for the hunter sitting out the fight.
// How long a Feign Death is allowed to stay on. Nothing else takes it off, so these are not
// tuning knobs, they are the only thing between a threat dump and a hunter lying down for the
// rest of the fight.
uint32 const PB_FEIGN_THREAT_DUMP_MS = 1500;
uint32 const PB_FEIGN_PANIC_MS = 6000;

uint32 const PB_TRAP_ATTEMPT_BUDGET_MS = 4000;

// And how long the hunter leaves it alone afterwards, whether the trap went down or not. Stops a
// sequence that cannot complete from being retried every tick, which is the other shape the old
// livelock could have taken.
uint32 const PB_TRAP_RETRY_MS = 30000;

// How close the add has to be to walk onto a trap laid at the hunter's feet.
float const PB_TRAP_SUMMON_RADIUS = 15.0f;

// The health below which a mob's big heal is assumed to be live, and so worth keeping a global
// cooldown free for. Bosses switch their heals on somewhere in this region -- Antu'sul's Healing
// Wave at thirty percent, his Flash Heal at sixty -- and above it the group's job is damage.
// The health band the interrupt reserve engages in now lives in DungeonTactics, per creature,
// as healsBelowPercent. It was a constant here and its value was Antu'sul's Flash Heal threshold,
// which is a fact about one boss's script and was being applied to every creature in the game.

// A tank this low stops holding its global for anything. Its survival abilities sit below the
// reserve in the rotation, and a tank that dies waiting to interrupt has lost the fight outright.
// And the tank stops reserving only when its own life is the more pressing question.
//
// Generic on purpose: a tank below a third of its health has a problem the group's damage does
// not solve, whatever it is fighting. The number was moved down from fifty after a tank sat at
// fifty three percent through a heal it was holding an ability for, but it is not read from any
// one encounter -- the question of when a tank should stop helping and start surviving does not
// depend on which boss is in front of it.
float const PB_TANK_RESERVE_HEALTH_FLOOR = 35.0f;

// The health below which a hunter gives up attack power for dodge. Low, because Aspect of the
// Monkey contributes no damage whatsoever and the tank is supposed to be the reason the hunter
// is not being hit.
float const PB_HUNTER_MONKEY_HEALTH = 35.0f;

// The mana share above which a caster should be casting rather than wanding. A wand is the empty
// bar option; anything above this and the bot owns a spell that is strictly better.
float const PB_WAND_MANA_FLOOR = 20.0f;

// The bar above which a healer has nothing better to do with a tick than shoot, and the health
// below which it is worth three hundred mana to stop being hit. Both exist because the same
// healer, in the same fight, did each of these things at exactly the wrong moment.
float const PB_HEALER_WAND_MANA_CEILING = 90.0f;
float const PB_HEALER_SELF_SHIELD_HEALTH = 70.0f;

float const PB_CHARGE_MIN_RANGE = 8.0f;
float const PB_CHARGE_MAX_RANGE = 25.0f;

// The tauren racial. Granted at character creation rather than trained, so it is not in any class
// spell list and has to be named.
static constexpr uint32 PB_SPELL_WAR_STOMP = 20549;

// Collecting loose enemies onto a warrior and walking them back to the group.
//
// The search range is how far a warrior will look for something worth fetching, and the stray
// bound is measured from the anchor rather than from the warrior, because what needs preventing is
// a warrior wandering off after an add and taking the fight with it. The shout radius is what
// Demoralizing Shout is believed to reach; the safety radius is what gets scanned for anything not
// yet in the fight, deliberately wider, since being wrong about the first costs a global cooldown
// and being wrong about the second costs a pack.
static constexpr float PB_GATHER_SEARCH_RANGE = 30.0f;
static constexpr float PB_GATHER_SHOUT_RADIUS = 10.0f;
static constexpr float PB_GATHER_SHOUT_SAFETY_RADIUS = 14.0f;
static constexpr uint32 PB_GATHER_SHOUT_MIN_TARGETS = 2;
static constexpr float PB_GATHER_MAX_STRAY = 25.0f;
static constexpr float PB_GATHER_ANCHOR_MAX_RANGE = 60.0f;
// How nearly dead the current target has to be before a warrior refuses to leave it, and how long
// it must then stay on whatever it switched to. Both exist to stop the collecting behaviour turning
// into a warrior that changes its mind every tick and finishes nothing.
static constexpr float PB_GATHER_FINISH_PERCENT = 25.0f;
static constexpr time_t PB_GATHER_SWITCH_INTERVAL = 5;
// The longest a warrior will stay on an add it left its target to peel. Aggro normally lands in a
// swing or two, so this is not the usual way back - it is the backstop for the add that dies to
// somebody else, walks out of reach, or otherwise never gets around to attacking the warrior.
static constexpr time_t PB_GATHER_PEEL_MAX_SECONDS = 8;
// How long a warrior leaves an add alone after a peel on it was abandoned. A body peel that ran
// out of time ran out for a structural reason -- the add is a caster and stays at range, it is
// rooted, it is behind something -- and none of those clear within a tick, so re-taking it
// immediately is what produced the shuttling. Measured in Zul'Farrak: a tank took a Sandfury
// Shadowcaster off the healer from 7.2y, walked out, waited the full eight seconds without the
// add ever turning, walked back, and re-took the same Shadowcaster in the same second it arrived.
// Comfortably longer than the walk itself, so an add worth a second attempt still gets one.
static constexpr time_t PB_GATHER_PEEL_RETRY_INTERVAL = 20;
static constexpr float PB_GATHER_RETURN_DISTANCE = 12.0f;

// Stepping out of an aggro radius the bot is already standing inside.
//
// The margin is small on purpose. This is not asking whether the bot is comfortably clear, it is
// asking whether it is inside the band at all, and every yard added here is a yard of ground the
// group gives up in a corridor for a creature that was never going to notice. The interval is what
// keeps this from becoming a shuffle: one step, then the follow or the chase gets its say, and if
// the bot is still inside a band two seconds later it steps again.
//
// The travel cap is generous by comparison with the other repositioning steps, because unlike them
// this one has a floor under how far it has to go: a spot half way out of a seventeen yard radius
// is not out of it, so a short cap means the search finds nothing and the bot stays where it is.
static constexpr float PB_NEIGHBOUR_STEP_MARGIN = 2.0f;
static constexpr time_t PB_NEIGHBOUR_STEP_INTERVAL = 2;
static constexpr float PB_NEIGHBOUR_STEP_MAX_TRAVEL = 22.0f;

// Backing a fight away from a neighbouring camp.
//
// The margin is wider than the ordinary pull check: this is asking whether the fight is being had
// uncomfortably close to something, not whether one step would wake it, and the whole point is to
// act before anybody's repositioning does. The step is short because a tank that drags a mob too
// far loses it to its leash, and the gap bounds keep the tank from walking a boss into the group
// or wandering off after a leader who is halfway across the instance.
static constexpr float PB_DRAG_BACK_MARGIN = 8.0f;
static constexpr float PB_DRAG_BACK_STEP = 8.0f;
static constexpr float PB_DRAG_BACK_MIN_GAP = 10.0f;
static constexpr float PB_DRAG_BACK_MAX_GAP = 40.0f;

// A damage warrior's rage economy, in internal units: rage is stored at ten times its displayed
// value, so 150 is fifteen rage. Bloodrage is reached for below the cost of the cheapest thing
// worth casting, and the dump fires above the cost of Heroic Strike plus a little.
static constexpr uint32 PB_WARRIOR_DPS_RAGE_LOW = 150;
static constexpr uint32 PB_WARRIOR_DPS_RAGE_DUMP = 200;

// How often a melee bot rechecks whether it can get back behind its target. Short enough that it
// returns to the rear promptly once the tank has moved the fight, long enough not to re-issue a
// chase every tick.
static constexpr time_t PB_MELEE_FACING_INTERVAL = 3;
// Going back behind a target asks for this much more clearance than leaving did, and the answer has
// to come back the same way twice in a row before anybody walks. Both exist because the rear point
// is computed from the target's own facing, and a tanked mob turns: without a margin and a
// confirmation, a rear spot sitting near a camp's edge reads safe and unsafe alternately as the mob
// rotates, and the bot walks a semicircle every time it changes its mind.
static constexpr float PB_MELEE_REAR_RETURN_MARGIN = 4.0f;
static constexpr uint32 PB_MELEE_FACING_CONFIRMATIONS = 2;

// How long between any two repositionings of a bot in combat, shared by every system that does it.
// Long enough that a short walk completes and the bot settles before anything re-decides.
static constexpr time_t PB_COMBAT_MOVE_INTERVAL = 3;

// Walking out of the reach of something rooted or stunned on top of us.
//
// A minimum of what has to be left on the hold, so the walk buys more than the swing it costs: a
// caster that steps out of a root with half a second to run has given up its position for nothing
// and will be chased down immediately. Two seconds is about one swing plus the walk.
static constexpr uint32 PB_HELD_STEP_MIN_REMAINING_MS = 2000;

// How long a bot waits before trying again to summon or revive a pet that is missing or dead. The
// retry exists for the mid-fight case, where the reason a summon was refused is usually a cast
// already in flight, so it wants to be short enough to land inside the same fight.
static constexpr time_t PB_PET_SUMMON_INTERVAL = 5;

// And a floor on how often, so one root produces one step rather than a step every tick for as
// long as it lasts.
static constexpr time_t PB_HELD_STEP_INTERVAL = 3;

// Floor between two attempts to walk out of melee, so a caster that keeps being followed spends
// the fight casting rather than stepping. Same value as the held step above.
static constexpr time_t PB_BACKOUT_INTERVAL = 3;

// How often a caster that has decided to stand and take it says so. Once per fight is the useful
// rate; the decision itself is remade every tick.
static constexpr time_t PB_STAND_LOG_INTERVAL = 5;
// What being first and second on an instance's escort list is worth, in health percentage points,
// when the healer is choosing between several hurt escorts. Enough to reach the one the encounter
// turns on before it is critical; far too little to ignore one that is dying.
static constexpr float CB_ESCORT_FIRST_RANK_BONUS = 15.0f;
static constexpr float CB_ESCORT_SECOND_RANK_BONUS = 8.0f;
// How long a caster will decline its wand on the grounds that the rotation ought to have had
// something better. Long enough that a gated rotation is still visible as idleness in the log,
// short enough that it cannot become a stalemate nothing can break.
static constexpr uint32 PB_WAND_HOLD_MAX_MS = 10000;

// How many consecutive ticks a bot spends unable to see its own target before it stops arguing
// with the wall and walks. Four ticks is one second. Not one tick: a mob crossing behind a pillar
// is out of sight for a moment and clears on its own, and moving for that would have a bot
// chasing every rock in the room.
static constexpr uint32 PB_BLIND_TICKS_BEFORE_MOVING = 4;
// Seconds between attempts to find a firing line. Long enough for the walk to finish, since the
// refusals keep arriving while it is under way and would otherwise re-launch it every tick.
static constexpr time_t PB_BLIND_STEP_INTERVAL = 3;
// The closest a bot will put itself while looking for one. Inside every ranged attack in the game
// and outside the melee a caster has no business standing in.
static constexpr float PB_BLIND_STEP_MIN_DISTANCE = 15.0f;
// Beyond this the bot does not try to find a firing line at all. Comfortably past every spell
// range a levelling bot has, so anything further off is a target it should not be holding rather
// than one it cannot see.
static constexpr float PB_BLIND_MAX_TARGET_DISTANCE = 40.0f;
// The most ground one attempt will cover. Short, because only the destination of a move is checked
// for what it might wake and the route is not, so the shorter the move the smaller the exposure.
static constexpr float PB_BLIND_STEP_MAX_TRAVEL = 12.0f;

// How far a healer will walk to get sight of somebody it is already in range of. Short, because
// this is meant to be the sidestep that clears a doorframe: a healer that walks further than this
// to see one member has left the spot from which it could see the rest of them.
static constexpr float PB_HEAL_SIGHT_STEP_TRAVEL = 10.0f;

// How far below the tank's own level a creature has to be before peeling it off the healer is not
// worth a taunt. Generous, because a low level mob in a dungeon is still usually part of a pull;
// what this excludes is the ambient wildlife.
static constexpr uint32 PB_PEEL_LEVEL_FLOOR = 8;
// Seconds before the same creature may be peeled again. Longer than the run from the healer to the
// tank, so a mob already on its way is not taunted a second time for not having arrived yet.
static constexpr time_t PB_PEEL_REPEAT_INTERVAL = 8;

// How long an interrupt already spent on a creature keeps the next bot off it.
//
// An interrupt resolves when the spell lands, not when it is cast, so for a moment afterwards the
// creature is still in SPELL_STATE_PREPARING and still looks like it needs interrupting. Every bot
// evaluating inside that moment reaches the same conclusion and pays for the same cast.
//
// One capture has eight interrupts stopping six casts: Shield Bash and Kick landing on one Druid's
// Slumber in the same second, and Earth Shock and Kick on another. Kick is a ten second cooldown,
// so each of those left the group an interrupt short for the next ten seconds, which is where the
// sleeps that got through came from.
//
// Long enough to cover the resolution delay and nothing beyond it, because past that point the
// window is no longer absorbing a duplicate -- it is hiding a failure.
//
// It was a second and a half, on the reasoning that "the school lockout an interrupt applies is
// longer than this by itself". That is only true of an interrupt that landed. Kick and Shield Bash
// are melee class abilities and roll the attack table: a level forty two rogue kicking a level
// forty eight boss whiffs better than one time in seven. Three Antu'sul attempts in a row have the
// rogue Kick a Healing Wave of Antu'sul with eight hundred milliseconds left, the boss gain a full
// Wave one second later, and the tank -- holding a ready Shield Bash the whole time -- never take
// its own attempt, because the window from the Kick outlasted the cast the Kick had failed to
// stop. One miss ended the attempt.
//
// Four hundred milliseconds is one or two ticks at the rate bots evaluate, which still catches the
// case this exists for: two bots reaching the same conclusion inside the same batch. What it no
// longer does is spend the rest of the cast bar waiting on an ability that already missed.
static constexpr uint32 PB_INTERRUPT_SHARE_WINDOW_MS = 400;

// Which creatures the group has just spent an interrupt on. Shared across bots because that is the
// whole point: the question is not what this bot has done but what the group has already paid for.
// Keyed on the creature rather than on the cast, because a cast has no identity to key on.
static std::unordered_map<uint64, uint32> g_recentGroupInterrupts;

static bool WasRecentlyInterrupted(ObjectGuid guid)
{
    auto itr = g_recentGroupInterrupts.find(guid.GetRawValue());
    if (itr == g_recentGroupInterrupts.end())
        return false;

    return WorldTimer::getMSTimeDiff(itr->second, WorldTimer::getMSTime()) < PB_INTERRUPT_SHARE_WINDOW_MS;
}

static void NoteGroupInterrupt(ObjectGuid guid)
{
    uint32 const now = WorldTimer::getMSTime();

    // Swept here rather than on a timer, since this is the only thing that grows the map and a
    // fight involves a handful of creatures rather than thousands.
    for (auto itr = g_recentGroupInterrupts.begin(); itr != g_recentGroupInterrupts.end();)
    {
        if (WorldTimer::getMSTimeDiff(itr->second, now) >= PB_INTERRUPT_SHARE_WINDOW_MS)
            itr = g_recentGroupInterrupts.erase(itr);
        else
            ++itr;
    }

    g_recentGroupInterrupts[guid.GetRawValue()] = now;
}
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

// How long the party waits for an owner who has gone offline. Long enough to cover a client that
// died and has to be relaunched, patched through a launcher and logged back in, and short enough
// that a party genuinely abandoned in a dungeon does not sit there for the rest of the uptime.
static constexpr time_t PB_LEADER_OFFLINE_GRACE = 15 * MINUTE;

// Whether the leader is merely offline, in which case this bot waits instead of being deleted.
//
// Offline is not gone, and treating the two the same is what made a client crash cost the whole
// run. The Player object dies with the socket, GetPartyLeader then found nobody, and every bot
// asked to be removed on the spot. That is not just the loss of the bots: Group::RemoveMember
// disbands a group that had two members or fewer, so the party came apart as the bots left it, and
// an instance's bind lives on the group -- InstanceMap::Add deliberately hands the player's own
// bind over to the group on entry and unbinds them personally. So the disband took the only bind
// to that instance with it, and Player::LoadFromDB, finding a saved instance id it can no longer
// account for, put the owner at the dungeon entrance. Walking back in minted a new copy. Five
// Shadowfangs in the characters table are what that looked like from the outside.
//
// Waiting is the whole fix, and nothing here has to be written to the database. The bots stay in
// the world, so the group keeps its members, so it never disbands, so the bind survives, so the
// owner logs back into the instance they left, at the position their own logout saved, still in
// the party they left it with.
bool PartyBotAI::WaitForOfflineLeader()
{
    Group* pGroup = me->GetGroup();

    // A leader still listed in the group is one who logged out; member slots outlive the session.
    // Anything else -- kicked from the group, replaced as leader, the bot promoted to lead -- is a
    // real refusal from GetPartyLeader and still means removal, which is why this asks about the
    // slot rather than assuming every null leader is a crash.
    if (!pGroup || !pGroup->IsMember(m_leaderGuid))
        return false;

    // Found means the refusal was about something other than presence, and the caller should act on
    // it as before rather than wait for somebody who is standing right there.
    if (ObjectAccessor::FindPlayerNotInWorld(m_leaderGuid))
        return false;

    time_t const now = time(nullptr);

    if (!m_leaderOfflineSince)
    {
        m_leaderOfflineSince = now;

        // Parked once, on the way in, rather than every tick. The bots are not going to fight well
        // with nobody to follow or heal towards, and a party left mid-pull will probably die where
        // it stands -- but it dies in the instance, with corpses to run back to, which is a great
        // deal better than being deleted along with the group and the bind.
        me->AttackStop();
        me->SetAttackOrders(ObjectGuid());
        // Unconditional, because StopMoving already asks whether there is a spline to stop and
        // clears the movement flags either way, which is the half that matters for a bot.
        me->StopMoving();
        me->GetMotionMaster()->Clear(false, true);
        me->GetMotionMaster()->MoveIdle();

        if (IsCombatLogged())
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] hold bot='%s' role=%s owner went offline, holding position on map "
                     "%u for up to %us so the group and its instance bind survive the reconnect",
                     me->GetName(), GetRoleName(GetRole()), me->GetMapId(),
                     uint32(PB_LEADER_OFFLINE_GRACE));
    }

    if ((now - m_leaderOfflineSince) >= PB_LEADER_OFFLINE_GRACE)
    {
        if (IsCombatLogged())
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] hold bot='%s' gave up on an owner offline for %us and is leaving "
                     "the group", me->GetName(), uint32(now - m_leaderOfflineSince));

        return false;
    }

    return true;
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

    // Straight back if straight back is clear, and round the side if it is not. Refusing outright
    // was the old answer and it meant a caster cornered between the thing hitting it and a camp
    // behind stood there and took it, when a few degrees off the line would have done.
    if (!WouldPathPullExtraEnemies(x, y, z))
        return me->GetMotionMaster()->MoveDistance(pEnemy, PB_DISTANCING_RANGE);

    return SafeMoveTo(x, y, z);
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

// Keep a pet working, asked every tick rather than once before the fight.
//
// Both pet classes used to command their pet from UpdateOutOfCombatAI_Hunter and _Warlock, and the
// dispatcher only runs those while the bot is not in combat. So the pet's entire opportunity to be
// told to attack was the sliver between its owner acquiring a victim and the server flagging that
// owner as in combat - a window a bot misses routinely once the group pulls with a dedicated
// puller, because it is already flagged by the time it picks a target. A pet that missed it stood
// still for the whole fight.
//
// Two more followed from the same cause. The order was latched behind "the pet has no victim", so a
// pet whose target died idled for the rest of a multi-mob pull rather than moving to the next one;
// and a pet that died could not be replaced until combat ended, because the summon sat in the same
// out-of-combat-only path.
void PartyBotAI::UpdatePetCombat()
{
    if (me->GetClass() != CLASS_HUNTER && me->GetClass() != CLASS_WARLOCK)
        return;

    if (!me->IsAlive() || IsInDuel())
        return;

    Pet* pPet = me->GetPet();

    // Missing or dead. Throttled rather than attempted every tick, because a summon refused for a
    // reason that will still hold next tick - a cast already in progress, a spell not ready - would
    // otherwise be retried four times a second for the length of the fight.
    if (!pPet || !pPet->IsAlive())
    {
        // Replacing a pet mid-fight is the point of doing this in combat at all, but only where
        // replacing it is a single cast. A hunter with no pet to its name goes through spawning a
        // beast and taming it, and a wolf materialising in the middle of a boss fight to be tamed
        // is worse than the hunter finishing the fight alone: that case waits for the fight to end.
        bool const canSummonNow = !me->IsInCombat() ||
                                  me->GetClass() == CLASS_WARLOCK ||
                                  me->GetPetGuid();

        time_t const now = time(nullptr);
        if (canSummonNow && now - m_lastPetSummon >= PB_PET_SUMMON_INTERVAL)
        {
            m_lastPetSummon = now;
            SummonPetIfNeeded();
        }

        return;
    }

    if (!pPet->GetCharmInfo())
        return;

    // Holding means the pet holds too: sending it in is the same pull as going in person, taken by
    // proxy. HoldPet has already made it passive, so this only needs to not undo that.
    if (m_holdPosition)
        return;

    // A totem is the one job a pet is better suited to than its owner.
    //
    // Everything the instance marks kill-on-sight has to be walked to and hit, and for a ranged
    // owner that means leaving its firing position and giving up its rotation to break something
    // with a few hundred health. A pet is already mobile, already expendable, and contributes far
    // less damage to the boss than its owner does -- so sending it is close to free, and the owner
    // never stops shooting.
    //
    // Only the stationary ones. The same list carries Antu'sul's Servants, which are level forty
    // eight elites, and a pet sent at one alone is a dead pet and a totem still standing.
    if (Unit* pTotem = FindFocusTotemForPet(PB_PET_TOTEM_RADIUS))
    {
        if (pPet->GetVictim() != pTotem)
            CommandPetAttack(pPet, pTotem);

        return;
    }

    Unit* pVictim = me->GetVictim();
    if (!pVictim || !IsValidHostileTarget(pVictim))
        return;

    // Only on a mismatch, so this is not an order re-sent four times a second, but it is re-sent
    // the moment the pet's target dies or the group's focus moves.
    if (pPet->GetVictim() == pVictim)
        return;

    CommandPetAttack(pPet, pVictim);
}

// Order a pet onto a target the way the game does it when a player clicks attack.
//
// Setting IsCommandAttack and calling AttackStart is not the whole of that command, and the part
// that was missing turns out to be the part that decides whether it takes effect at all.
// PetAI::CanAttack ends, for a pet in follow mode, at "return !IsReturning()". That latch is set by
// PetAI::HandleReturnMovement, which runs on every pet tick that the pet has no living victim --
// so it is set in the gap between one target dying and the next order arriving, which is exactly
// when the order arrives. It clears only when the pet reports arriving at its follow point, and a
// pet whose owner keeps moving around a fight may never report that at all.
//
// So the warlock was not failing to send its pet in. It was sending it in constantly and having
// every order refused in silence, because AttackStart calls CanAttack and returns without a word.
//
// HandleReturnMovement also calls ClearCharmInfoFlags on its way past, which unsets the very
// IsCommandAttack flag this sets, so the flag did not reliably survive to the pet's own next tick
// either.
//
// Mirrors Unit::PetCommandAttack rather than approximating it: the same five flags, in the same
// order, with the same AttackStop ahead of a retarget.
void PartyBotAI::CommandPetAttack(Pet* pPet, Unit* pTarget)
{
    CharmInfo* pCharmInfo = pPet->GetCharmInfo();
    if (!pCharmInfo || !pTarget)
        return;

    if (IsCombatLogged())
    {
        time_t const now = time(nullptr);
        if (now - m_lastPetLog >= PB_PET_LOG_INTERVAL)
        {
            m_lastPetLog = now;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] pet bot='%s' pet='%s' -> '%s' at %.1fy (was on '%s'), "
                     "returning=%u following=%u atstay=%u cmdattack=%u cmdfollow=%u passive=%u",
                     me->GetName(), pPet->GetName(), pTarget->GetName(),
                     pPet->GetDistance(pTarget),
                     pPet->GetVictim() ? pPet->GetVictim()->GetName() : "none",
                     uint32(pCharmInfo->IsReturning()), uint32(pCharmInfo->IsFollowing()),
                     uint32(pCharmInfo->IsAtStay()), uint32(pCharmInfo->IsCommandAttack()),
                     uint32(pCharmInfo->IsCommandFollow()),
                     uint32(pPet->HasReactState(REACT_PASSIVE)));
        }
    }

    pPet->ClearUnitState(UNIT_STATE_FOLLOW);

    if (pPet->GetVictim())
        pPet->AttackStop();

    pCharmInfo->SetIsCommandAttack(true);
    pCharmInfo->SetIsAtStay(false);
    pCharmInfo->SetIsFollowing(false);
    pCharmInfo->SetIsCommandFollow(false);
    pCharmInfo->SetIsReturning(false);

    pPet->AI()->AttackStart(pTarget);
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
        // A wand belongs on this list for the same reason the rest do: it is a ranged attack the
        // bot can fire, on its own timer, for no mana. It was missing, so the one class of weapon
        // carried by every bot that runs out of mana was the one this function could not name, and
        // the only code that fired a wand at all was a priest-specific branch.
        case ITEM_SUBCLASS_WEAPON_WAND:
            return PB_SPELL_SHOOT_WAND;
    }

    return 0;
}

// Something, rather than nothing.
//
// The rotations are if-chains that fall out of the bottom when nothing matched, and falling out of
// the bottom means the bot stands there. That happens far more than it sounds: a caster out of
// mana, a melee bot out of rage or energy, anybody whose one useful spell is on cooldown, and every
// healer with nobody to heal. None of those is a reason to contribute zero.
//
// Ordered by what it costs the bot to do. A wand or a bow costs nothing at all and fires on its own
// timer, so it is never the wrong answer; melee is last because it is worth little and, for a
// caster, means being somewhere unpleasant. Nothing here moves the bot: this is about using the
// position it is already in, and closing distance is a decision that belongs to the movement code
// and its aggro rules.
bool PartyBotAI::KeepBusy()
{
    Unit* pVictim = me->GetVictim();
    if (!pVictim || !IsValidHostileTarget(pVictim))
        return false;

    // Already committed to something this tick.
    if (me->IsNonMeleeSpellCasted(false, false, true) ||
        me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        return false;

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) || me->IsMounted())
        return false;

    // Keeping busy is not worth taking the boss for.
    //
    // This exists so a bot with nothing else to do contributes a little rather than standing
    // there, and a wand is the smallest contribution in the game -- but threat is charged on it
    // the same as anything else, and a healer is the one member of the group that starts a fight
    // already near the top of the table. Logged fifty nine wand shots from a priest at eighteen
    // percent mana, thirty three of them from under four yards, while it sat top of Antu'sul's
    // threat list at 1591 against a tank on 1024. The damage was rounding error; the threat was
    // the wipe.
    //
    // The same ceiling the damage rotations already respect, so this is not a new judgement about
    // threat, only the filler finally being asked to observe one.
    if (IsOverThreatCeiling(pVictim))
        return false;

    float const distance = me->GetCombatDistance(pVictim);

    // Not while there is mana to cast with. Symmetrical with the cancellation in the update
    // above, and it has to be: cancelling a wand that this would restart on the same tick is a
    // bot that never fires either one. The wand is the bottom of the list for a caster, below
    // every spell it knows, so reaching for it with a full bar means the rotation was gated --
    // and leaving the bot visibly idle is the right outcome, because the wand was hiding that.
    if (HasManaWorthCastingWith())
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        if (!m_wandHoldSince)
            m_wandHoldSince = nowMs;

        // Bounded, like every other refusal in this file, and for the reason the tank-lead hold
        // gives: every rule here that refuses something has at some point refused it forever.
        //
        // The reasoning above is right about the ordinary case and wrong about exactly one -- a
        // healer whose rotation has nothing it can aim at the thing hitting it. Zul'Farrak's
        // third wave produced it, and it is unbreakable from outside: a priest six yards from a
        // Sandfury Slave it is one yard per second too slow to outrun, on half a mana bar,
        // casting Renew on itself every five seconds while the troll's health did not move once
        // in nine minutes. No stall detector can see that either, because health is changing the
        // whole time, so from a distance it reads as a fight in progress.
        //
        // So the refusal still fires, and still leaves the bot idle long enough that a gated
        // rotation shows up in the log where it belongs, and then it stops being a principle and
        // shoots.
        if (WorldTimer::getMSTimeDiff(m_wandHoldSince, nowMs) < PB_WAND_HOLD_MAX_MS)
        {
            // Stop swinging as well as declining to shoot. Refusing the filler only removes the
            // ranged option; the melee auto attack was switched on by the attack order and stays
            // on until something clears it, so a caster that declines here and is standing in
            // range quietly becomes a melee character. That is worse than the wand this was
            // avoiding.
            if (me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);

            // Once in a while rather than every tick. At four ticks a second this line was two
            // hundred and twenty seven entries in a single fight and drowned the log it was meant
            // to explain.
            time_t const now = time(nullptr);
            if (IsCombatLogged() && now - m_lastWandHoldLog >= PB_STAND_LOG_INTERVAL)
            {
                m_lastWandHoldLog = now;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] wandhold bot='%s' role=%s declined a wand at %.0f%% mana: "
                         "the rotation should have had something to cast",
                         me->GetName(), GetRoleName(m_role), me->GetPowerPercent(POWER_MANA));
            }

            return false;
        }
    }
    else
    {
        m_wandHoldSince = 0;
    }

    // A ranged attack, if the bot is carrying one it can fire from where it stands. Moving cancels
    // an autorepeat before it ever goes off, so a bot mid-walk is left alone rather than made to
    // start one it will immediately lose.
    if (!me->IsMoving() && me->IsWithinLOSInMap(pVictim))
    {
        // The hunter dead zone. Inside it the shot is refused, and the interrupt further up would
        // cancel it again next tick, so offering one here would be a loop rather than an attack.
        bool const insideMinimumRange = (me->GetClass() == CLASS_HUNTER) && (distance < 8.0f);

        if (!insideMinimumRange)
        {
            if (uint32 const rangedSpellId = GetRangedAttackSpellId())
            {
                if (SpellEntry const* pRanged = sSpellMgr.GetSpellEntry(rangedSpellId))
                {
                    if (pRanged->IsTargetInRange(me, pVictim))
                    {
                        // Set the orientation rather than asking to be turned. SetFacingToObject
                        // launches a facing movespline, a movespline is movement, and movement
                        // cancels an autorepeat: the priest wand path used to call it before every
                        // shot, so each tick cancelled the shot the previous tick had started and
                        // the wand never actually fired. Writing the angle costs nothing and moves
                        // nothing.
                        //
                        // And told to the clients watching. SetOrientation writes the angle on
                        // the server and sends nothing, so the shot was allowed - the server had
                        // the hunter facing its target - while every client went on drawing it
                        // pointing whichever way it stopped walking. A heartbeat carries the new
                        // orientation without a spline, so the model turns without the movement
                        // flags that cancel an autorepeat, which is the whole reason
                        // SetFacingToObject cannot be used here.
                        if (!me->HasInArc(pVictim))
                        {
                            me->SetOrientation(me->GetAngle(pVictim));
                            me->SendHeartBeat();
                        }

                        me->Attack(pVictim, false);
                        if (me->CastSpell(pVictim, pRanged, false) == SPELL_CAST_OK)
                        {
                            // Logged here rather than through DoCastSpell, which is what starts an
                            // autorepeat rather than firing a shot and so would report a cast per
                            // volley instead of per start. Without this a wanding priest and an
                            // idle one are the same three lines of log, which is how the wand that
                            // never fired went unnoticed for a whole run.
                            if (IsCombatLogged())
                            {
                                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                         "[BotCombat] ranged bot='%s' role=%s started '%s' on '%s' "
                                         "at %.1fy",
                                         me->GetName(), GetRoleName(m_role),
                                         pRanged->SpellName[0].c_str(), pVictim->GetName(),
                                         me->GetDistance(pVictim));
                            }

                            return true;
                        }
                    }
                }
            }
        }
    }

    // Failing that, swing at it, but only if it is already standing in reach. This is the mage with
    // an empty mana bar and a staff in its hands: poor damage, and strictly better than watching.
    if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
        me->CanReachWithMeleeAutoAttack(pVictim))
    {
        return me->Attack(pVictim, true);
    }

    return false;
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
    SpellEntry const* pSpell = rangedSpellId ? sSpellMgr.GetSpellEntry(rangedSpellId) : nullptr;

    // A ranged weapon has a minimum range as well as a maximum, and owning one was being read as
    // a commitment to use it. A puller that had walked all the way in -- which is what the closing
    // phase does when no shot was available from the anchor -- then stood on top of the mob unable
    // to shoot it, because five yards is inside a gun's dead zone, and never swung either, because
    // the melee branch was only reachable by a bot with no ranged weapon at all. The Molten Core
    // capture has a tank do exactly that at dist=0.0 for four seconds while two level sixty two
    // giants killed it, having pulled both of them with its body and attacked neither.
    if (!pSpell || !pSpell->IsTargetInRange(me, pTarget))
    {
        // Nothing to shoot with from here, so the pull is made with a fist. Worth doing rather
        // than refusing: it still brings the mob back to a group that is standing still, which is
        // the point, and it is what a warrior without a gun would have to do anyway.
        if (!me->CanReachWithMeleeAutoAttack(pTarget))
            return false;

        if (!me->HasInArc(pTarget))
        {
            me->SetOrientation(me->GetAngle(pTarget));
            me->SendHeartBeat();
        }
        return me->Attack(pTarget, true);
    }

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

    // Written, not splined, and this line used to undo the three above it. StopMoving is here
    // precisely to clear the movement flags that stop a ranged attack firing, and
    // SetFacingToObject then launched a facing movespline, which sets them straight back. Turning
    // by assignment costs nothing and moves nothing.
    if (!me->HasInArc(pTarget))
    {
        me->SetOrientation(me->GetAngle(pTarget));
        me->SendHeartBeat();
    }
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
    m_pullCloseAimed = false;
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
    m_pullCloseAimed = false;
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
                    me->GetMotionMaster()->MovePoint(0, m_holdX, m_holdY, m_holdZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
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

            // Walked as a route to a point, not chased. A chase over this distance does not move
            // the bot at all: it aims at a point one yard from the target rather than at the
            // target, computes that point through GetNearPointAroundPosition and a walk-hit
            // adjustment off the target's own footing, and when the result is not something the
            // mesh will path to, _setTargetLocation returns before it ever launches a spline. It
            // then repeats that failure every hundred milliseconds in silence. Molten Core's first
            // pull is the case in hand: the route from the raid's staging point to the giant is a
            // clean nineteen-point path of seventy eight yards, every bearing on every ring around
            // the giant paths cleanly too, and the tank stood still through the whole thirty second
            // timeout with UNIT_STATE_CHASE set and UNIT_STATE_CHASE_MOVE never set once.
            //
            // A point move asks the pathfinder the same question the harness asks and gets the
            // same answer. It is also the cheaper of the two by a wide margin over this distance,
            // since it is one path rather than one every tick, and the target being pulled is by
            // definition standing still.
            float const targetX = pTarget->GetPositionX();
            float const targetY = pTarget->GetPositionY();

            // Arrived, and the shot above still refused, so this is a melee pull. Swinging is
            // what ends the sequence; walking any further is what filled a log with a hundred
            // "closing on foot" lines a second at dist=0.0 while the mob killed the puller.
            if (me->CanReachWithMeleeAutoAttack(pTarget))
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                {
                    me->GetMotionMaster()->Clear(false, true);
                    me->GetMotionMaster()->MoveIdle();
                }

                if (me->Attack(pTarget, true))
                {
                    m_pullPhase = PULL_PHASE_FIRE;
                    LogPull("in melee, swinging");
                }

                return true;
            }

            // Re-aimed only when the mob has actually moved, so a patroller is followed and a
            // stationary one is walked to once. Idle means the walk finished or was never
            // launched, and both want the same answer.
            bool const drifted = m_pullCloseAimed &&
                (Geometry::GetDistance2D(targetX, targetY, m_pullCloseX, m_pullCloseY) >
                 PB_PULL_CLOSE_REAIM);

            if (!m_pullCloseAimed || drifted ||
                me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                m_pullCloseX = targetX;
                m_pullCloseY = targetY;
                m_pullCloseAimed = true;

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                {
                    me->GetMotionMaster()->Clear(false, true);
                    me->GetMotionMaster()->MoveIdle();
                }

                me->GetMotionMaster()->MovePoint(0, targetX, targetY, pTarget->GetPositionZ(),
                                                 MOVE_PATHFINDING | MOVE_RUN_MODE);
                LogPull("closing on foot");
            }

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
                me->GetMotionMaster()->MovePoint(0, m_holdX, m_holdY, m_holdZ, MOVE_PATHFINDING | MOVE_RUN_MODE);

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
        me->GetMotionMaster()->MovePoint(0, x, y, z, atTheDoor ? MOVE_RUN_MODE : (MOVE_PATHFINDING | MOVE_RUN_MODE));
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

// Whether this bot is the tank whose job this particular target is.
//
// Every rule below that a tank is exempt from -- the threat ceiling, the opening hold, the
// decision to taunt -- is exempt because the tank is the one meant to be at the top of that
// target's threat list. That reasoning holds for one tank and inverts for four: an off-tank
// free-firing into the main tank's target is a damage dealer with no ceiling, which is
// precisely the thing the ceiling exists to prevent, and it was reading as a tank and
// skipping it. So the exemption follows the assignment rather than the role.
//
// An off-tank holding something else is still a tank in full: it is the assigned tank for
// whatever it is actually fighting, and answers yes here for that target.
bool PartyBotAI::IsAssignedTankFor(Unit const* pTarget) const
{
    if (m_role != ROLE_TANK || !pTarget)
        return false;

    Player* pMain = GetGroupMainTank();
    if (!pMain || pMain == me)
        return true;

    // Taken to include what the main tank is merely targeting, not only what is hitting it.
    // At the moment a pull goes wrong those are different answers, and that is the moment the
    // off-tanks most need to keep their threat off it.
    return pMain->GetVictim() != pTarget &&
           pMain->GetTargetGuid() != pTarget->GetObjectGuid();
}

// Whether this bot is carrying enough threat that dropping all of it is the right move.
//
// Feign Death's actual job, and the only one it has here. An earlier build used it as pretend
// crowd control, which it is not -- it holds nothing, controls nothing, and the hunter that cast
// it spent a whole Antu'sul attempt lying on the floor. What it does do is wipe the caster's own
// threat, and that is a real problem this group has no other answer to: the threat system can
// ration what a bot adds from here on, but nothing can take back what it has already built. One
// attempt has the hunter at 1,971 threat against the tank's 1,624, eating boss melee at 1.9 yards
// because the rationing arrived long after the lead was gone, and dying with the fight still
// winnable.
//
// Two moments qualify, and both require somebody else who can hold the target afterwards -- the
// group's main tank, alive and already on it. Without that check this hands the boss to whoever is
// second on the list, which in a five man is the healer.
bool PartyBotAI::ShouldDumpThreatWithFeignDeath(Unit* pTarget) const
{
    if (IsInDuel() || !pTarget || !pTarget->CanHaveThreatList() || !pTarget->IsInCombat())
        return false;

    if (m_role == ROLE_TANK)
        return false;

    Player* pTank = GetGroupMainTank();
    if (!pTank || pTank == me || !pTank->IsAlive())
        return false;

    // Not in the opening seconds, whatever the ratio says. Threat is a share of the tank's and the
    // tank has almost none yet, so for the first few swings every damage dealer in the group reads
    // as over the line: the first live run fired this one second into the pull at eighteen threat
    // against the tank's four, which drops nothing worth dropping and wastes the cooldown before
    // the fight that needs it has started.
    if (IsInOpeningRamp(pTarget))
        return false;

    ThreatManager& threat = pTarget->GetThreatManager();

    float const mine = threat.getThreat(me);
    float const tanks = threat.getThreat(pTank);
    if (mine <= 0.0f || tanks <= 0.0f)
        return false;

    // And not for a trivial amount. A dump is worth a thirty second cooldown when there is a real
    // lead to shed, and the bot's own health is the nearest thing to a scale for that which does
    // not need to know which encounter this is.
    if (mine < float(me->GetMaxHealth()))
        return false;

    // Already pulled it. Late, but the dump is still the fastest way to put it back, and the
    // alternative is the hunter tanking a boss in cloth-weight mail until it dies.
    if (pTarget->GetVictim() == me)
        return true;

    // Or at the line and about to. GetThreatPullRatio is the same number the rationing draws its
    // ceiling from, so this fires exactly where downranking has already run out of room.
    return mine >= tanks * GetThreatPullRatio(pTarget);
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
    if (IsAssignedTankFor(pTarget) || IsInDuel() || !IsInOpeningRamp(pTarget))
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
    if (IsAssignedTankFor(pTarget) || IsInDuel() || !pTarget->CanHaveThreatList())
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

    // An off-tank reaching here is standing in melee and is governed by the melee band, the
    // same as any other character whose threat is measured from inside the boss's reach.
    float const ceiling = GetThreatPullRatio(pTarget) -
        ((m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
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

    // A refusal here returns DONT_REPORT and so never reached the cast log, which made a caster
    // held back by threat read exactly like a caster with nothing to do. Reading a Scarlet
    // Monastery run, the mage was idle for 75% of the ticks on which it had a live target, at 70%
    // mana or better for nearly half of them, and there was no way to tell from the log whether it
    // was choosing not to cast or being refused. Counted rather than logged line by line, because
    // at four ticks a second a refused rotation writes faster than anything else in the file.
    if (!pRank || pRank != pSpellEntry)
    {
        if (pRank)
            ++m_threatDownranks;
        else
            ++m_threatRefusals;

        time_t const now = time(nullptr);
        if (IsCombatLogged() && now - m_lastThreatLog >= PB_THREAT_LOG_INTERVAL)
        {
            float const budget = GetThreatHeadroom(pTarget);
            ThreatManager& threat = pTarget->GetThreatManager();
            HostileReference const* pTop = threat.getCurrentVictim();

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] threathold bot='%s' role=%s refused=%u downranked=%u since last, "
                     "wanted '%s' on '%s' mythreat=%.0f topthreat=%.0f top='%s' budget=%.0f",
                     me->GetName(), GetRoleName(m_role), m_threatRefusals, m_threatDownranks,
                     pSpellEntry->SpellName[0].c_str(), pTarget->GetName(),
                     threat.getThreat(me), pTop ? pTop->getThreat() : 0.0f,
                     pTop && pTop->getTarget() ? pTop->getTarget()->GetName() : "none",
                     budget);

            m_lastThreatLog = now;
            m_threatRefusals = 0;
            m_threatDownranks = 0;
        }

        if (!pRank)
            return SPELL_FAILED_DONT_REPORT;
    }

    return CombatBotBaseAI::DoCastSpell(pTarget, pRank);
}

// How much of a resource must be left untouched so the interrupt is affordable when it is needed.
//
// Holding an ability for the right cast is worth nothing if the rotation has spent the rage it
// costs. The tank that missed Serpentis' sleep had Shield Bash ready and off cooldown, in Defensive
// Stance with a shield equipped, and two rage: it had put fifteen into a Sunder Armor one second
// earlier. The ranking said interrupt, the ability said yes, and the rage bar said no.
//
// Only against something actually known to cast crowd control, so this is not a permanent tax on
// every warrior in the game - and only while the interrupt is off cooldown, since there is nothing
// to save for while it is not.
// Whether anything the group is fighting is worth keeping an interrupt for.
//
// This used to ask only about the bot's own victim, and on any fight with more than one creature
// in it that is the wrong question. Antu'sul heals himself three separate ways; his adds cast
// nothing at all. A rogue sent to kill a Servant therefore held no reserve, spent every point of
// energy on Sinister Strike, and stood at six energy while the boss healed -- the combat log
// reads "Kick:power" four times in two seconds, which is a bot that knows exactly what it should
// be doing and cannot pay for it.
//
// Cached for a second because CanTryToCastSpell asks constantly, and the answer cannot
// meaningfully change faster than that.
bool PartyBotAI::IsAnythingNearbyWorthInterrupting() const
{
    time_t const now = time(nullptr);
    if (m_lastInterruptWatchCheck == now)
        return m_interruptWatchWorth;

    m_lastInterruptWatchCheck = now;
    m_interruptWatchWorth = false;

    if (Unit const* pVictim = me->GetVictim())
    {
        if (GetWorstKnownCastPriority(pVictim) >= PB_INTERRUPT_HEAL)
        {
            m_interruptWatchWorth = true;
            return true;
        }
    }

    // Anything else in the fight that casts, within the range an interrupt could reach it from.
    // Deliberately generous: walking two yards to Kick a heal is worth holding the energy for,
    // and the alternative is what happened here.
    std::list<Unit*> nearby;
    me->GetEnemyListInRadiusAround(me, PB_INTERRUPT_WATCH_RADIUS, nearby);

    for (Unit* pUnit : nearby)
    {
        if (!pUnit->IsInCombat())
            continue;

        if (GetWorstKnownCastPriority(pUnit) >= PB_INTERRUPT_HEAL)
        {
            m_interruptWatchWorth = true;
            break;
        }
    }

    return m_interruptWatchWorth;
}

uint32 PartyBotAI::GetPowerReservedForInterrupt(Powers powerType, SpellEntry const* pSpellEntry) const
{
    if (powerType != POWER_RAGE && powerType != POWER_ENERGY)
        return 0;

    if (IsInDuel())
        return 0;

    if (!IsAnythingNearbyWorthInterrupting())
        return 0;

    std::vector<SpellEntry const*> interrupts;
    GetInterruptSpells(interrupts);

    // The interrupt is never held back by its own reserve.
    for (SpellEntry const* pInterrupt : interrupts)
        if (pInterrupt == pSpellEntry)
            return 0;

    uint32 reserve = 0;

    for (SpellEntry const* pInterrupt : interrupts)
    {
        if (Powers(pInterrupt->powerType) != powerType)
            continue;

        if (!me->IsSpellReady(pInterrupt))
            continue;

        uint32 const cost = Spell::CalculatePowerCost(pInterrupt, me);
        if (!reserve || cost < reserve)
            reserve = cost;
    }

    return reserve;
}

bool PartyBotAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (!CombatBotBaseAI::CanTryToCastSpell(pTarget, pSpellEntry))
        return false;

    Powers const powerType = Powers(pSpellEntry->powerType);
    if (uint32 const reserved = GetPowerReservedForInterrupt(powerType, pSpellEntry))
    {
        uint32 const cost = Spell::CalculatePowerCost(pSpellEntry, me);
        if (cost && me->GetPower(powerType) < (cost + reserved))
            return false;
    }

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

    // Nothing that reaches past its target may touch something nobody is fighting. Every role,
    // including the tank: a tank standing where every movement rule approves of still wakes the
    // next pack with a Consecration, and it is the one bot whose mistake the group cannot walk
    // away from.
    //
    // Placed here, in front of the threat rule below rather than inside it, because the two ask
    // different questions and the older one cannot answer this. It walks the enemies around the
    // target comparing threat, and its loop begins `pEnemy->GetVictim() && ...` - so a creature
    // that is in no fight has no victim, fails that condition, and is skipped. The rule that was
    // supposed to stop area effects pulling was structurally incapable of seeing anything
    // unpulled, which is the only thing that can be pulled.
    if (!IsInDuel() && WouldSpellPullExtraEnemies(pTarget, pSpellEntry))
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

    // Last, because it is the most expensive test in the function and every cheaper one above can
    // reject a spell without paying for it.
    //
    // Do not offer a cast at something the bot cannot see. CheckCast performs this same test and
    // refuses the spell for it, so nothing is lost by asking first and a great deal is saved: the
    // rotation is a list, a refused spell falls through to the next one, and a bot behind a wall
    // walks the whole list several times a second for as long as the wall is there. Eight hundred
    // and twenty six casts in one Wailing Caverns run went nowhere for this reason, and every one
    // of them was logged as though it were a decision.
    //
    // Only offensive spells, and only against somebody else: a heal refused for line of sight is
    // still worth attempting, because the healer moves to fix that and the refusal is how it finds
    // out it needs to.
    if (pTarget && pTarget != me && !pSpellEntry->IsPositiveSpell() &&
        !me->IsWithinLOSInMap(pTarget))
        return false;

    return true;
}

bool PartyBotAI::CanUseCrowdControl(SpellEntry const* pSpellEntry, Unit* pTarget) const
{
    if (IsInDuel())
        return true;

    // Species and immunity are not checked here any more. Both live in CanTryToCastSpell, which
    // every one of these sites already calls alongside this one, and which now covers the whole
    // rotation rather than the four crowd control spells that happened to be guarded by hand.
    if (pSpellEntry->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) &&
        AreOthersOnSameTarget(pTarget->GetObjectGuid()))
        return false;

    if (pSpellEntry->HasSingleTargetAura())
    {
        auto const& singleAuras = me->GetSingleCastSpellTargets();
        if (singleAuras.find(pSpellEntry) != singleAuras.end())
            return false;
    }

    if (!IsWorthALongCooldownCC(pSpellEntry, pTarget))
        return false;

    return true;
}

// Whether this target is worth a crowd control the bot only gets once.
//
// Blind is a five minute cooldown. That is longer than the whole of Antu'sul, so the rogue gets
// exactly one, and every attempt so far has spent it in the first ten seconds on a Sul'lithuz
// Broodling out of the trigger pack -- trash the group kills in four swings -- leaving nothing for
// the Servant of Antu'sul that arrives thirty seconds later at seventy five percent and spends the
// rest of the fight eating the healer. The log reads the same way every time: one "cc" line
// against a Broodling, and none ever against the Servant.
//
// A short cooldown needs none of this. Gouge, Sap and Polymorph come back inside a pull, so
// spending one early costs nothing and the ordinary target selection is right about them.
//
// The judgement of what is worth saving for comes from the instance's tactics rather than from
// anything on the creature, because that is where it can be known. Both creatures here are
// summons of the same boss, both are elite, and the Servant is not distinguishable from the
// Broodling by level, health or family alone at the moment the choice has to be made. What
// separates them is when they arrive, and the table already records it: the summon is named
// against the boss that calls it, so requiring that boss to be alive and already fighting rules
// out the pre-pull hatchlings and admits the mid-fight adds, without naming either.
bool PartyBotAI::IsWorthALongCooldownCC(SpellEntry const* pSpellEntry, Unit const* pTarget) const
{
    if (pSpellEntry->GetRecoveryTime() < PB_CC_LONG_COOLDOWN_MS)
        return true;

    // Nothing written down for this map, so nothing to save it for. Every instance without an
    // entry behaves exactly as it did before this existed.
    if (!m_tactics)
        return true;

    Creature const* pCreature = pTarget ? pTarget->ToCreature() : nullptr;
    if (!pCreature)
        return true;

    // Not on something weaker than the bot casting it. This is what the summoner check alone did
    // not catch: Sul'lithuz Broodling and Servant of Antu'sul are both summons of the same boss and
    // both appear in his entry in the table, so requiring a named summon admitted the trigger pack
    // as readily as the real add -- and the tank has usually already engaged Antu'sul by the time
    // the last Broodling is up, so requiring the summoner to be fighting admitted it too. The log
    // has the rogue spending Blind on a level thirty nine Broodling at 22:27:52, three separate
    // attempts running, with the level forty eight Servant arriving half a minute later to nothing.
    //
    // Level is the discriminator that actually separates them and it needs no table: a creature
    // below the bot's own level is trash the group kills in a few swings, and nothing about it is
    // worth a cooldown measured in minutes.
    if (pTarget->GetLevel() < me->GetLevel())
        return false;

    float gate = 0.0f;
    uint32 const summonerEntry = m_tactics->GetBurnGateFor(pCreature->GetEntry(), gate);
    if (!summonerEntry)
        return false;

    // And the summoner has to be in the fight. The same Broodling entry is used by the trigger
    // pack that hatches before anybody has touched Antu'sul and by the adds he calls once he is
    // engaged, so the entry alone cannot tell them apart and the boss's own combat state can.
    Creature const* pSummoner = me->FindNearestCreature(summonerEntry, PB_CC_SUMMONER_SEARCH_RADIUS, true);
    return pSummoner && pSummoner->IsInCombat();
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

// The one thing the group is killing.
//
// Everything that damages reads this and nothing else, so the answer has to be stable: a focus that
// changes its mind every tick is four bots running between mobs and landing nothing. Hence the
// ordering below, which prefers the most deliberate signal available and only falls back to a
// judgement of its own when there is none.
//
// A crowd-controlled mob can never be the answer. IsValidHostileTarget already refuses anything
// carrying a break-on-damage aura, which is exactly the sapped, polymorphed, hibernated or shackled
// mob the group has deliberately taken out of the fight.
// What the whole party should be killing, and why.
//
// Tiers, compared against each other, so the gaps carry no meaning beyond the ordering. The point
// of tiers rather than one number is the stickiness below: a focus is only given up to something
// in a strictly higher tier, and inside a tier nobody moves. A single score would have the group
// drifting between two mobs whose health percentages cross over mid-fight, which is the shape of
// the complaint this exists to answer.
enum PbFocusTier : uint32
{
    PB_FOCUS_NONE = 0,
    // Anything in the fight at all.
    PB_FOCUS_ENGAGED = 1,
    // A mob whose template says it can heal or control. Worth killing before the melee around it
    // even while it is doing nothing, because it will.
    PB_FOCUS_SUPPORT = 2,
    // Nearly dead. Finishing it removes a whole mob's damage now, which beats a larger amount of
    // damage spread across mobs that all keep swinging.
    PB_FOCUS_EXECUTE = 3,
    // Casting a heal or a crowd control this second.
    PB_FOCUS_CASTING = 4,
    // Named by the instance's own tactics as the thing to kill first.
    PB_FOCUS_TACTICS = 5,
    // Said out loud by a player: a raid mark, or an explicit attack order.
    PB_FOCUS_ORDERED = 6,
};

// Below this share of health a mob is worth finishing ahead of a healthier one.
static constexpr float PB_FOCUS_EXECUTE_HEALTH = 25.0f;

// The party's current kill target, by group. Held rather than recomputed from scratch because the
// stickiness is the whole feature: every bot runs the same selection over the same candidates and
// so agrees without having to be told, but agreeing on a fresh answer every tick is precisely how
// five bots ended up working five mobs to half health. One of them writes this, the rest read it,
// and the write only ever happens on an empty focus, a dead one, or a strictly better tier - so it
// does not matter which bot gets there first.
struct PartyBotGroupFocus
{
    ObjectGuid target;
    uint32 tier = PB_FOCUS_NONE;
    time_t since = 0;

    // What a player last told the group to kill, which is a different thing from what the group
    // has drifted onto and outlives it.
    //
    // Held here, group wide, rather than on each bot, for two reasons. A bot that was out of range
    // when the command went out still joins in, and more importantly the instruction survives the
    // movement exemption it used to be carried by: Player::SetAttackOrders does double duty as
    // "you may walk into an unengaged mob's aggro radius", and that exemption is correctly spent
    // the moment the mob enters combat - which is one tick after the first bot reaches it. Reading
    // the instruction off the same field gave every order a lifetime of about a quarter second,
    // after which the group reverted to whatever it had been on and looked as though it had
    // ignored the player.
    ObjectGuid ordered;
};
static std::unordered_map<uint32 /*groupId*/, PartyBotGroupFocus> s_groupFocus;

// Point the whole group at something, on a player's say-so.
void PartyBotAI::SetGroupAttackOrder(ObjectGuid guid)
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;

    s_groupFocus[pGroup->GetId()].ordered = guid;
}

void PartyBotAI::ClearGroupAttackOrder()
{
    if (Group* pGroup = me->GetGroup())
        s_groupFocus[pGroup->GetId()].ordered.Clear();
}

uint32 PartyBotAI::ScoreFocusCandidate(Unit const* pEnemy) const
{
    if (!pEnemy)
        return PB_FOCUS_NONE;

    // An order or a mark is a player talking, and it wins from wherever the mob happens to be.
    if (Group* pGroup = me->GetGroup())
    {
        auto const itr = s_groupFocus.find(pGroup->GetId());
        if (itr != s_groupFocus.end() && itr->second.ordered == pEnemy->GetObjectGuid())
            return PB_FOCUS_ORDERED;
    }

    if (Group* pGroup = me->GetGroup())
    {
        for (auto markId : m_marksToFocus)
        {
            if (pGroup->GetTargetWithIcon(markId) == pEnemy->GetObjectGuid())
                return PB_FOCUS_ORDERED;
        }
    }

    if (m_tactics)
    {
        // Promoted only while turning for it is still worth the time. The burn gate was written for
        // exactly this and was wired into every path except the one that decides what the group
        // shoots at, so it never applied: Servant of Antu'sul is in focusFirst, focusFirst is tier
        // five, Antu'sul himself scores tier two, and so the whole party switched to the add the
        // moment it spawned and never came back. One attempt has the boss dropping to forty three
        // percent and then not being targeted again by anybody for the rest of the fight.
        //
        // Asked every tick rather than latched, so the answer tracks the boss's health: above the
        // gate the add is worth killing, below it the group stays on the boss, and crossing the
        // line turns the group round on its own.
        if (Creature const* pCreature = pEnemy->ToCreature())
            if (m_tactics->IsFocusFirst(pCreature->GetEntry()) &&
                IsSummonWorthLeavingBossFor(pEnemy, false) &&
                CanEngageFromHeldGround(pEnemy))
                return PB_FOCUS_TACTICS;
    }

    // Mid-cast right now. Reuses the interrupt classifier rather than a second opinion about which
    // spells matter, so a mob channelling a heal reads the same here as it does to a Kick.
    uint32 const casting = GetInterruptPriority(pEnemy);
    if (casting >= PB_INTERRUPT_HEAL)
        return PB_FOCUS_CASTING;

    if (pEnemy->GetHealthPercent() <= PB_FOCUS_EXECUTE_HEALTH)
        return PB_FOCUS_EXECUTE;

    // Not casting, but known to be able to. Cached per creature entry, so this costs a lookup.
    if (GetWorstKnownCastPriority(pEnemy) >= PB_INTERRUPT_HEAL)
        return PB_FOCUS_SUPPORT;

    return PB_FOCUS_ENGAGED;
}

Unit* PartyBotAI::SelectGroupFocusTarget() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup || IsInDuel())
        return nullptr;

    PartyBotGroupFocus& held = s_groupFocus[pGroup->GetId()];

    // A pull or an explicit attack order outranks everything, including whatever the group had
    // settled on, and unlike everything below it is about a mob that is not part of any fight yet -
    // so it is answered before the candidate scan, which only looks at mobs that are.
    //
    // The order is written into the focus rather than returned around it. Returning early used to
    // leave the group's held focus pointing at the previous mob, so the instruction was obeyed
    // without ever being agreed to: bots that had not received the command stayed where they were,
    // and the ones that had went back the moment the order lapsed. The group has to actually change
    // its mind, not merely be overruled for a tick.
    if (!held.ordered.IsEmpty())
    {
        Unit* pOrdered = me->GetMap()->GetUnit(held.ordered);

        // Spent once the thing is dead or is no longer something to hit. Deliberately not spent
        // when it enters combat, which is what the old movement exemption did and is the whole
        // bug: entering combat is the order working.
        if (!pOrdered || !pOrdered->IsAlive() || !IsValidHostileTarget(pOrdered))
        {
            held.ordered.Clear();
        }
        else
        {
            if (held.target != pOrdered->GetObjectGuid())
            {
                if (IsCombatLogged())
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] focus bot='%s' group=%u ordered onto '%s' (guid=%u hp=%.0f) "
                             "from '%s'",
                             me->GetName(), pGroup->GetId(), pOrdered->GetName(),
                             pOrdered->GetObjectGuid().GetCounter(), pOrdered->GetHealthPercent(),
                             held.target ? "a previous focus" : "nothing");

                held.target = pOrdered->GetObjectGuid();
                held.since = time(nullptr);
            }

            held.tier = PB_FOCUS_ORDERED;
            return pOrdered;
        }
    }

    Player* const pTank = GetGroupTank();
    Unit* const pTankVictim = pTank ? pTank->GetVictim() : nullptr;
    Player* const pLeader = GetPartyLeader();
    Unit* const pLeaderVictim = pLeader ? pLeader->GetVictim() : nullptr;

    // Score everything the group is actually fighting.
    //
    // Ties are broken, in order, by what the player is on, then by what the tank is on, then by
    // lowest health, then by guid. The leader and tank preferences are what keep this agreeing with
    // the old behaviour in the ordinary case - the group converges where the tank holds aggro, so
    // damage cannot pull the mob off it - and the guid is there so that two bots scoring an exact
    // tie still arrive at the same mob rather than one each.
    Unit* pBest = nullptr;
    uint32 bestTier = PB_FOCUS_NONE;

    // Where this creature comes in the instance's own kill order, or zero for one the instance
    // says nothing about. See DungeonTactics::focusFirst: the list is written in order and the
    // order is the tactic.
    auto const focusRank = [&](Unit const* pUnit) -> uint32
    {
        if (!m_tactics)
            return 0;

        Creature const* pCreature = pUnit->ToCreature();
        return pCreature ? m_tactics->GetFocusRank(pCreature->GetEntry()) : 0;
    };

    auto const better = [&](Unit* pCandidate, uint32 tier)
    {
        if (!pBest)
            return true;
        if (tier != bestTier)
            return tier > bestTier;

        // Ahead of the leader and the tank, because this is the instance answering a question
        // neither of them was asked. Two entries off the same list standing in front of the group
        // is the normal case rather than the exception -- Zul'Farrak's third pyramid wave arrives
        // as Nekrum and Sezz'ziz together, and its last fight is Bly with Oro and Murta beside him
        // -- and with the tiers equal the comparisons below settle it on health and then on guid,
        // which is to say the group picks whichever of the two happens to have been hit.
        //
        // Only between two ranked entries. A creature with no rank is not ordered against one that
        // has: tiers are equal here, so the pair are equally worth killing as far as everything
        // else in this function is concerned, and inventing a preference from the absence of a
        // table entry would make the list mean something it does not say.
        uint32 const candidateRank = focusRank(pCandidate);
        uint32 const heldRank = focusRank(pBest);
        if (candidateRank && heldRank && candidateRank != heldRank)
            return candidateRank < heldRank;

        if ((pCandidate == pLeaderVictim) != (pBest == pLeaderVictim))
            return pCandidate == pLeaderVictim;
        if ((pCandidate == pTankVictim) != (pBest == pTankVictim))
            return pCandidate == pTankVictim;
        if (pCandidate->GetHealthPercent() != pBest->GetHealthPercent())
            return pCandidate->GetHealthPercent() < pBest->GetHealthPercent();
        return pCandidate->GetObjectGuid() < pBest->GetObjectGuid();
    };

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember->GetMap() != me->GetMap())
            continue;

        for (const auto pAttacker : pMember->GetAttackers())
        {
            if (!IsValidHostileTarget(pAttacker) || !me->IsWithinDist(pAttacker, 50.0f))
                continue;

            // What the dungeon table says to leave alone is not a candidate for the group's
            // focus either. Filtering the peel alone was half a fix: the tank correctly stayed on
            // Antu'sul while the hunter and the healer opened on a Broodling anyway -- Shadow
            // Word: Pain and Smite, four hundred and seventy nine mana, thirteen percent of the
            // priest's bar, spent before the boss had been touched. An explicit attack order
            // still overrides this, above, because a player pointing at something outranks the
            // table.
            if (IsIgnoredByParty(pAttacker))
                continue;

            uint32 const tier = ScoreFocusCandidate(pAttacker);
            if (better(pAttacker, tier))
            {
                pBest = pAttacker;
                bestTier = tier;
            }
        }
    }

    // Whatever the group settled on last, if it is still a thing worth hitting.
    Unit* pHeld = held.target ? me->GetMap()->GetUnit(held.target) : nullptr;
    if (pHeld && (!IsValidHostileTarget(pHeld) || !IsEngagedWithGroup(pHeld)))
        pHeld = nullptr;

    if (pHeld)
    {
        // Re-scored rather than trusted, because the reason a mob was chosen expires: the one that
        // was casting a heal has finished casting it.
        uint32 const heldTier = ScoreFocusCandidate(pHeld);

        // An elite or a boss is not abandoned for an add. The group can only be moved off one by a
        // player saying so, which is the answer to "I have said to fight the boss, stay on it"
        // without needing the boss to be marked - though marking it is still the way to be sure,
        // since a mark scores above everything.
        bool const heavy = pHeld->ToCreature() &&
                           pHeld->ToCreature()->GetCreatureInfo()->rank != CREATURE_ELITE_NORMAL;

        // The one thing besides a player that moves the group off something heavy: the instance
        // naming a target that comes earlier in the same kill order.
        //
        // Without this the ordering above only decides the first pick of a fight, and whichever of
        // two simultaneous arrivals happened to swing first would keep the group for the rest of
        // it -- both of Zul'Farrak's ordered pairs are elites, so both would be heavy, and the
        // wave three pair arrive within a tick of each other. Which of them the group ends up on
        // would then be a race rather than a decision.
        //
        // Narrow on purpose. It needs both mobs to be on the same instance's focusFirst list, so
        // it can never drag a group off a boss for an add -- a boss with a tactics entry of its
        // own is not on that list, and an add with no entry has no rank to outrank anything with.
        // And it will not move the group down the order, only up it.
        uint32 const heldRank = focusRank(pHeld);
        uint32 const bestRank = pBest ? focusRank(pBest) : 0;
        bool const outrankedByTable = heldRank && bestRank && bestRank < heldRank &&
                                      bestTier >= heldTier;

        if (heavy && bestTier < PB_FOCUS_ORDERED && !outrankedByTable)
            return pHeld;

        // Otherwise: hold unless something is in a strictly higher tier. Equal tiers never move the
        // group, which is what stops it drifting between two mobs as their health crosses over.
        if (!outrankedByTable && (!pBest || bestTier <= heldTier))
        {
            held.tier = heldTier;
            return pHeld;
        }
    }

    if (!pBest)
    {
        s_groupFocus.erase(pGroup->GetId());
        return nullptr;
    }

    if (held.target != pBest->GetObjectGuid())
    {
        if (IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] focus bot='%s' group=%u now on '%s' (guid=%u tier=%u hp=%.0f) "
                     "from '%s' held %lds",
                     me->GetName(), pGroup->GetId(), pBest->GetName(),
                     pBest->GetObjectGuid().GetCounter(), bestTier, pBest->GetHealthPercent(),
                     pHeld ? pHeld->GetName() : "nothing",
                     held.since ? long(time(nullptr) - held.since) : 0L);
        }

        held.target = pBest->GetObjectGuid();
        held.since = time(nullptr);
    }

    held.tier = bestTier;
    return pBest;
}

// Take something out of the fight that nobody is killing yet.
//
// Only ever aimed away from the focus: crowd control on the mob the group is beating on is worse
// than nothing, because the first hit breaks it and the cast is wasted. Aimed at anything else in
// the pack it is a mob's whole damage output removed for the length of the fight, which at this
// level is the difference between a healer that copes and one that does not.
bool PartyBotAI::CrowdControlOffFocus()
{
    if (IsInDuel())
        return false;

    SpellEntry const* pSpellEntry = GetCrowdControlSpell();
    if (!pSpellEntry)
        return false;

    // Marks are handled separately and take precedence: a player who has marked something for
    // crowd control has said which mob, and this should not spend the cast on a different one.
    if (!m_marksToCC.empty())
        return false;

    Unit* pFocus = SelectGroupFocusTarget();

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    // The add the burn rule just told the group to walk away from is the best thing in the room to
    // control. It is alive, it is swinging, and by construction nobody is going to kill it:
    // Antu'sul sends two Servants at twenty five percent and each carries forty one percent of his
    // own health bar, so killing them is arithmetic nobody should attempt, and leaving them loose
    // on the healer is how the attempt ends instead.
    //
    // Run ahead of the ordinary scan rather than folded into it, because that scan takes the first
    // eligible attacker it finds and this is rarely the first.
    //
    // Whether any of it lands is still the spell's business. Against a Servant the answer is
    // narrow: it is immune to root and snare, and its creature type rules out Polymorph, Sap,
    // Shackle, Banish and Hibernate. Blind and Hammer of Justice are typeless and do work.
    if (m_tactics)
    {
        std::list<Unit*> suspended;
        me->GetEnemyListInRadiusAround(me, 40.0f, suspended);

        for (Unit* pAdd : suspended)
        {
            if (pAdd == pFocus || IsSummonWorthLeavingBossFor(pAdd, false))
                continue;

            if (pAdd->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
                continue;

            if (!IsValidHostileTarget(pAdd))
                continue;

            // Still not the one the tank has hold of. At twenty five percent two arrive, the tank
            // takes one, and this is how the other stops being everyone's problem.
            if (Player* pTank = GetGroupTank())
                if (pTank->GetVictim() == pAdd)
                    continue;

            if (AreOthersOnSameTarget(pAdd->GetObjectGuid()))
                continue;

            if (!CanUseCrowdControl(pSpellEntry, pAdd))
                continue;

            if (!CanTryToCastSpell(pAdd, pSpellEntry))
                continue;

            if (DoCastSpell(pAdd, pSpellEntry) == SPELL_CAST_OK)
            {
                me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);

                if (IsCombatLogged())
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] ccsummon bot='%s' role=%s controlled '%s' (lvl %u) with "
                             "'%s' rather than let it loose while the group burns the boss",
                             me->GetName(), GetRoleName(GetRole()), pAdd->GetName(),
                             pAdd->GetLevel(), pSpellEntry->SpellName[0].c_str());
                }

                return true;
            }
        }
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember->GetMap() != me->GetMap())
            continue;

        for (const auto pAttacker : pMember->GetAttackers())
        {
            if (pAttacker == pFocus)
                continue;

            // Already out of the fight, by this bot's hand or somebody else's.
            if (pAttacker->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
                continue;

            if (!IsValidHostileTarget(pAttacker))
                continue;

            // Not the mob the tank is holding, even when it is not the focus: taking the tank's
            // target out of its threat list is how a fight ends up with nobody tanking.
            if (Player* pTank = GetGroupTank())
                if (pTank->GetVictim() == pAttacker)
                    continue;

            // Somebody is already hitting it, so the control would break on the next swing.
            if (AreOthersOnSameTarget(pAttacker->GetObjectGuid()))
                continue;

            if (!CanUseCrowdControl(pSpellEntry, pAttacker))
                continue;

            if (!CanTryToCastSpell(pAttacker, pSpellEntry))
                continue;

            if (DoCastSpell(pAttacker, pSpellEntry) == SPELL_CAST_OK)
            {
                // The bot was probably swinging at the focus a moment ago, and a melee swing at
                // the thing just controlled would undo it.
                me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);

                if (IsCombatLogged())
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] cc bot='%s' role=%s lvl=%u controlled '%s' (lvl %u) with "
                             "'%s' while the group kills '%s'",
                             me->GetName(), GetRoleName(GetRole()), me->GetLevel(),
                             pAttacker->GetName(), pAttacker->GetLevel(),
                             pSpellEntry->SpellName[0].c_str(),
                             pFocus ? pFocus->GetName() : "nothing");
                }

                return true;
            }
        }
    }

    return false;
}

Unit* PartyBotAI::SelectAttackTarget(Player* pLeader) const
{
    // Nobody may be handed something the group is not already fighting. Applied here, at the one
    // place every role picks a target, rather than at each of the branches below, so a candidate
    // added later cannot quietly bypass it.
    //
    // This was the tank's rule alone, on the reasoning that the tank is the one that charges. It
    // is not: a mob is pulled by whoever touches it first, and the two branches this gate covers
    // are a raid mark and the leader's selection - both of which a player sets on the next pack
    // while the current one is still alive, which is the whole point of marking ahead. So the
    // damage dealers read the skull on the unpulled pack and opened on it while the tank, correctly
    // gated, stayed where it was. Every other way into this function is something already hitting
    // a group member, and those return without passing through here.
    //
    // IsTargetInCurrentFight carries the two exemptions that matter - an explicit attack order and
    // a pull in progress - so the deliberate pull is untouched and only the incidental one stops.
    auto const accept = [this](Unit* pCandidate) -> Unit*
    {
        if (!pCandidate)
            return nullptr;

        if (!IsTargetInCurrentFight(pCandidate))
            return nullptr;

        // And nothing the dungeon table says to leave alone, unless the player has pointed at it.
        //
        // Filtering the group's focus scan and the tank's peel collection was not enough, because
        // neither is how a bot picks a target when something walks up and hits it. The Antu'sul
        // pull that killed the priest has it cast Shadow Word: Pain three times on Sul'lithuz
        // Broodlings in the four seconds before the boss was engaged -- nine hundred mana, and
        // more importantly a threat lead it never lost: top='Rinval' at 1456 against the tank's
        // 1051, with the tank taunting the boss back off it five times in forty seconds and the
        // boss walking straight back each time.
        //
        // An explicit attack order still wins. "I will worry about them" means the player handles
        // these, and pointing the group at one is the player handling it.
        if (IsIgnoredByParty(pCandidate) &&
            !(me->HasAttackOrders() && me->GetAttackOrders() == pCandidate->GetObjectGuid()))
            return nullptr;

        return pCandidate;
    };

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
                                if (Unit* pAccepted = accept(pVictim))
                                    return pAccepted;
                }
            }
        }

        // Who is the leader attacking. Gated for a tank like everything else: a mob the leader has
        // merely selected is not a mob anybody is fighting, and charging it is how the tank starts
        // the second pull. Once the leader actually engages it, it passes.
        if (Unit* pVictim = pLeader->GetVictim())
        {
            if (IsValidHostileTarget(pVictim))
                if (Unit* pAccepted = accept(pVictim))
                    return pAccepted;
        }
    }

    // Who is attacking me. Never gated - something hitting the bot is in the fight by definition.
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

    // Last resort, and deliberately not put through accept(): that gate asks whether anybody is
    // currently fighting the candidate, and a controlled mob is by definition fighting nobody, so
    // the tank would be the one bot in the group that refused the last mob in the room. What the
    // gate is there to prevent - the tank charging something the group never pulled - is ruled out
    // more strictly below, by requiring the mob to still hold threat on this group.
    if (!IsInDuel())
    {
        if (Unit* pLeftover = SelectControlledLeftoverTarget())
            return pLeftover;
    }

    return nullptr;
}

// The fight is down to a mob somebody controlled, and nothing above will ever offer it.
//
// Every candidate in SelectAttackTarget is found by what a mob is doing: it carries a raid mark, it
// is what the leader is hitting, it is hitting this bot or a party member, or it is on the pet. A
// sheep does none of those things. It attacks nobody, so it is on no attacker list, and it is not
// the leader's victim unless the player happens to still have it selected. That is the whole reason
// the group stands around when the last mob left alive is the one that got polymorphed.
//
// Threat is what survives being controlled. Polymorph stops the mob attacking but leaves its threat
// list intact, which is precisely why it comes straight back to the same target the moment it
// breaks - so "was this mob part of our fight" is asked of its threat list rather than of who it is
// swinging at, and that is a stronger answer than the tank gate's, not a weaker one.
Unit* PartyBotAI::SelectControlledLeftoverTarget() const
{
    // Throttled because the caller runs every tick that this bot has no valid target, which
    // includes every idle tick out of combat, and this is a sixty yard cell visit. A second of
    // delay before the group turns on a leftover sheep is not noticeable; a cell visit per bot
    // per tick for the whole time a party stands in an inn is.
    time_t const now = time(nullptr);
    if (now - m_lastLeftoverScan < PB_LEFTOVER_SCAN_INTERVAL)
        return nullptr;

    m_lastLeftoverScan = now;

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, PB_LEFTOVER_SEARCH_RADIUS, enemies);

    for (Unit* pEnemy : enemies)
    {
        if (!pEnemy || !pEnemy->IsAlive())
            continue;

        if (!pEnemy->HasBreakableByDamageCrowdControlAura())
            continue;

        // IsValidHostileTarget already carries the "is there anything better to hit" question, so
        // a sheep that is still worth respecting is refused here without asking it twice.
        if (!IsValidHostileTarget(pEnemy))
            continue;

        if (!HasThreatOnGroup(pEnemy))
            continue;

        if (IsCombatLogged())
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] lastmob bot='%s' role=%s breaking control on '%s' at %.1fy, "
                     "nothing else left in the fight",
                     me->GetName(), GetRoleName(m_role), pEnemy->GetName(),
                     me->GetDistance(pEnemy));

        return pEnemy;
    }

    return nullptr;
}

// Whether this mob's threat list still holds anyone from the group.
//
// IsEngagedWithGroup reads victims and attacker lists, which is the right question about a mob that
// is fighting and the wrong one about a mob that has been controlled: the control stops it
// attacking, so it drops off every attacker list in the group while its threat list is left
// untouched.
bool PartyBotAI::HasThreatOnGroup(Unit const* pEnemy) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    // Read-only, but neither the threat lookup nor the container beneath it is marked const.
    ThreatManager& threat = const_cast<Unit*>(pEnemy)->GetThreatManager();

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsInWorld() || pMember->GetMap() != me->GetMap())
            continue;

        // The offline list is searched too: a target the creature cannot currently act against is
        // moved out of the live list, and being unable to act is the state control puts it in.
        if (threat.getThreat(pMember, true) > 0.0f)
            return true;

        if (Pet* pPet = pMember->GetPet())
            if (threat.getThreat(pPet, true) > 0.0f)
                return true;
    }

    return false;
}

// Whether turning to kill this summon still pays, given how far through its summoner is.
//
// Only ever says no about creatures a tactic names as a summon of something with a burn gate, so
// every add in every other fight, and every totem in this one, is unaffected. See
// DungeonCreatureTactic::burnBelowPercent for why Antu'sul needs it: above sixty percent he has no
// heal and the pause is free, below it he undoes whatever the pause cost.
// The nearest thing the instance says to kill that a pet can reasonably be sent at on its own.
Unit* PartyBotAI::FindFocusTotemForPet(float radius) const
{
    if (!m_tactics || m_tactics->focusFirst.empty())
        return nullptr;

    std::list<Unit*> nearby;
    me->GetEnemyListInRadiusAround(me, radius, nearby);

    for (Unit* pUnit : nearby)
    {
        Creature const* pCreature = pUnit->ToCreature();
        if (!pCreature)
            continue;

        if (!m_tactics->IsFocusFirst(pCreature->GetEntry()))
            continue;

        // Elites the group has to handle together, not things to post a pet at.
        if (m_tactics->IsSummonedAdd(pCreature->GetEntry()))
            continue;

        // Said explicitly rather than left to the filter below, because the whole of "the pet goes
        // back to the boss afterwards" rests on this returning nothing once the totems are down.
        if (!pUnit->IsAlive())
            continue;

        if (!IsValidHostileTarget(pUnit))
            continue;

        return pUnit;
    }

    return nullptr;
}

// The nearest add the burn rule has told the group to leave alone, or null when there is none.
Unit* PartyBotAI::FindSuspendedSummonNearby(float radius) const
{
    if (!m_tactics)
        return nullptr;

    std::list<Unit*> nearby;
    me->GetEnemyListInRadiusAround(me, radius, nearby);

    for (Unit* pUnit : nearby)
    {
        if (IsSummonWorthLeavingBossFor(pUnit, false))
            continue;

        if (!IsValidHostileTarget(pUnit))
            continue;

        if (pUnit->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
            continue;

        return pUnit;
    }

    return nullptr;
}

// Whether the dungeon table says to leave this creature alone entirely.
//
// Distinct from the burn gate next to it, which asks "is this add worth leaving the boss for right
// now" and answers from the boss's health. This asks nothing about timing: the answer is the same
// at the pull and at five percent, because the reason is that the creature is not the group's
// problem at all.
// Whether this interrupt can actually take a cast away from this target, or only looks like it.
//
// The whole of three nights' worth of missed Healing Waves of Antu'sul is in this one question,
// and the log could not ask it. Antu'sul carries mechanic_immune_mask 646659935, whose bit 25 is
// mechanic 26, MECHANIC_INTERRUPT. On the 5086 build of the spell table Kick's interrupt sits in
// effect two with EffectMechanic 26, and Shield Bash's in effect one with the same, so
// Unit::IsImmuneToSpellEffect drops the interrupt effect and leaves the rest of the ability
// alone: the Kick lands, deals its damage, reads as a successful cast, and does nothing to the
// heal. Counterspell carries mechanic 26 at the spell level and is refused outright.
//
// The measured result is four interrupts against two Healing Waves in one fight -- Kick at 900ms
// and Shield Bash at 299ms on the first, Kick at 799ms and Shield Bash at 298ms on the second --
// and both heals landing in full. Read as timing it looks like the group was a fraction too slow
// twice. It was never timing. Nothing this group owns can interrupt him.
//
// Checked per effect rather than per spell because that is where the immunity bites, and a spell
// whose interrupt is not an effect at all -- Gouge, which incapacitates -- is left alone.
bool PartyBotAI::CanInterruptWith(SpellEntry const* pSpellEntry, Unit const* pTarget) const
{
    if (!pSpellEntry || !pTarget)
        return false;

    if (pTarget->IsImmuneToSpell(pSpellEntry, false))
        return false;

    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (pSpellEntry->Effect[i] != SPELL_EFFECT_INTERRUPT_CAST)
            continue;

        return !pTarget->IsImmuneToSpellEffect(pSpellEntry, SpellEffectIndex(i), false);
    }

    return true;
}

bool PartyBotAI::IsIgnoredByParty(Unit const* pEnemy) const
{
    if (!m_tactics || !pEnemy)
        return false;

    Creature const* pCreature = pEnemy->ToCreature();
    if (!pCreature)
        return false;

    return m_tactics->IsIgnoredByParty(pCreature->GetEntry());
}

bool PartyBotAI::IsSummonWorthLeavingBossFor(Unit const* pAdd, bool logIt) const
{
    if (!m_tactics || !pAdd)
        return true;

    Creature const* pCreature = pAdd->ToCreature();
    if (!pCreature)
        return true;

    float belowPercent = 0.0f;
    uint32 const gateEntry = m_tactics->GetBurnGateFor(pCreature->GetEntry(), belowPercent);
    if (!gateEntry)
        return true;

    Creature* pGate = me->FindNearestCreature(gateEntry, 100.0f, true);
    if (!pGate || !pGate->IsAlive())
        return true;

    if (pGate->GetHealthPercent() > belowPercent)
        return true;

    if (logIt && IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] burnon bot='%s' role=%s stayed on '%s' (%.0f%%) rather than turn for "
                 "'%s', because leaving now costs more than the add does",
                 me->GetName(), GetRoleName(GetRole()), pGate->GetName(),
                 pGate->GetHealthPercent(), pCreature->GetName());
    }

    return false;
}

// The suspended summon close enough to walk onto a trap laid here.
//
// Deliberately a short radius. A trap is laid at the hunter's own feet and only springs when
// something walks over it, so an add on the far side of the room is not a candidate however badly
// it needs controlling -- it will never reach this patch of floor before the trap times out.
Unit* PartyBotAI::FindSummonWorthTrapping(float radius) const
{
    if (!m_tactics)
        return nullptr;

    std::list<Unit*> nearby;
    me->GetEnemyListInRadiusAround(me, radius, nearby);

    Player* pTank = GetGroupTank();

    for (Unit* pAdd : nearby)
    {
        if (!pAdd || !pAdd->IsAlive() || !IsValidHostileTarget(pAdd))
            continue;

        // Only the ones the burn rule has already told the group to walk away from. Anything the
        // group is still killing does not want trapping, and the trap breaks on damage anyway.
        if (IsSummonWorthLeavingBossFor(pAdd, false))
            continue;

        // Not the one the tank has hold of. At twenty five percent two Servants arrive, the tank
        // takes one, and the other is what this is for.
        if (pTank && pTank->GetVictim() == pAdd)
            continue;

        if (pAdd->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
            continue;

        if (pAdd->HasBreakableByDamageCrowdControlAura())
            continue;

        return pAdd;
    }

    return nullptr;
}

// Freezing Trap, in the two steps the game insists on.
//
// The trap is the only control that works on a Servant of Antu'sul. Its creature type is
// NOT_SPECIFIED, which rules out Polymorph, Sap, Shackle, Banish and Hibernate outright -- their
// target masks do not carry that bit -- and its immunity mask rules out root and snare. What is
// left is the typeless handful: Blind, on a five minute cooldown the rogue gets once a fight;
// Gouge, four seconds; and Freezing Trap, twenty. Against six thousand one hundred and eighty six
// health of add that the group cannot afford to kill -- forty seven percent of the boss's own bar,
// thirty three seconds of damage, two Healing Waves handed back -- twenty seconds of control is
// the only number here that changes the fight.
//
// Feign Death is not decoration on this and not an attempt to use it as control. The trap carries
// SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL: the server refuses it outright while the hunter is in
// combat, so there is no version of this that does not drop combat first. That is the whole of
// why it is here.
//
// The previous build of this livelocked -- the hunter lay down for sixty nine seconds and never
// laid the trap -- and the cause was a hold that kept it feigned while it waited for conditions.
// There is no hold here and no waiting state. Every path either casts something this tick and
// returns true, or returns false immediately and lets the rotation have the tick. A deadline
// bounds the whole attempt, and missing it stands the sequence down rather than retrying.
// End a trap attempt and get the hunter back on its feet.
//
// The single thing both previous builds of this were missing, and the whole of the livelock. A
// feigned hunter cannot act, and "let the rotation have the tick" therefore does nothing at all:
// the rotation reaches for a shot, the feign refuses it, and the bot sits there. Feign Death has
// no duration worth waiting out -- six minutes -- so unless something takes the aura off
// deliberately the hunter stays down for the rest of the fight. One capture has it silent from
// 23:45:43 to 23:46:46, sixty three seconds, while Antu'sul healed from fifteen percent to
// thirty six and the kill was lost.
//
// Removing the aura here rather than relying on the next action is what makes the deadline real.
void PartyBotAI::EndTrapAttempt(uint32 now)
{
    m_trapAttemptStart = 0;
    m_trapStandDownUntil = now + PB_TRAP_RETRY_MS;

    if (m_spells.hunter.pFeignDeath && me->HasAura(m_spells.hunter.pFeignDeath->Id))
        me->RemoveAurasDueToSpell(m_spells.hunter.pFeignDeath->Id);

    HoldPet(false);
}

bool PartyBotAI::TryFreezingTrapSequence()
{
    if (me->GetClass() != CLASS_HUNTER || IsInDuel() || m_holdPosition)
        return false;

    if (!m_spells.hunter.pFreezingTrap || !m_spells.hunter.pFeignDeath)
        return false;

    uint32 const now = WorldTimer::getMSTime();

    // Stood down after a failed attempt. Nothing is retried for a while, so a sequence that cannot
    // complete costs one attempt rather than the fight.
    if (m_trapStandDownUntil && WorldTimer::getMSTimeDiff(now, m_trapStandDownUntil) > 0 &&
        m_trapStandDownUntil > now)
        return false;

    bool const feigned = me->HasAura(m_spells.hunter.pFeignDeath->Id);

    // Step two: feigned already, so the trap is castable. This is the only reason the hunter is
    // lying down, so it happens now or the attempt is over.
    if (feigned)
    {
        if (CanTryToCastSpell(me, m_spells.hunter.pFreezingTrap) &&
            DoCastSpell(me, m_spells.hunter.pFreezingTrap) == SPELL_CAST_OK)
        {
            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] trap bot='%s' laid '%s' while feigned, %ums into the attempt",
                         me->GetName(), m_spells.hunter.pFreezingTrap->SpellName[0].c_str(),
                         WorldTimer::getMSTimeDiff(m_trapAttemptStart, now));
            }

            EndTrapAttempt(now);
            return true;
        }

        // Could not lay it. The deadline is what ends this: past it the hunter stands up by
        // getting on with the rotation, which breaks the feign on its own.
        if (m_trapAttemptStart &&
            WorldTimer::getMSTimeDiff(m_trapAttemptStart, now) >= PB_TRAP_ATTEMPT_BUDGET_MS)
        {
            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] trapabort bot='%s' gave up laying the trap after %ums and "
                         "went back to fighting",
                         me->GetName(), WorldTimer::getMSTimeDiff(m_trapAttemptStart, now));
            }

            EndTrapAttempt(now);
        }

        return false;
    }

    // Step one: worth starting at all. Checked before Feign Death rather than after, because
    // dropping combat for a trap that is on cooldown is the hunter taking itself out of the fight
    // for nothing -- which is exactly what the removed version did.
    if (m_trapAttemptStart)
    {
        // Was feigned, is not any more, and never laid it. Resistance, or the feign broke.
        EndTrapAttempt(now);
        return false;
    }

    if (!me->IsSpellReady(m_spells.hunter.pFreezingTrap) ||
        !me->IsSpellReady(m_spells.hunter.pFeignDeath))
        return false;

    Unit* pAdd = FindSummonWorthTrapping(PB_TRAP_SUMMON_RADIUS);
    if (!pAdd)
        return false;

    // Call the pet off first, and this is the whole reason the sequence has never once worked.
    //
    // Feign Death does drop combat -- SetFeignDeath calls CombatStop and deletes every hostile
    // reference -- and then, four lines later, puts the hunter straight back into it:
    //
    //     if (Pet* pPet = GetPet())
    //         if (pPet->IsInCombat() && pPet->GetVictim())
    //             SetInCombatWithVictim(pPet->GetVictim(), false, 6000);
    //
    // Six seconds, re-applied from the pet's target, which for a hunter bot is always something.
    // Freezing Trap is flagged non-combat and CheckCast refuses it outright while IsInCombat is
    // true, so the trap was being rejected with SPELL_FAILED_AFFECTING_COMBAT every single time,
    // deterministically, for longer than the attempt's own deadline. Not a timing race, which is
    // what the two failed builds were written to fix: the log shows "feigned but the trap was
    // refused: ok" -- every precondition passing and the server saying no anyway.
    //
    // Stopping the pet's attack clears its victim, so that branch does not fire and the hunter
    // stays out of combat long enough to plant the trap. UpdatePetCombat sends it back in on the
    // next tick, which is also when the feign ends.
    HoldPet(true);

    if (!CanTryToCastSpell(me, m_spells.hunter.pFeignDeath) ||
        DoCastSpell(me, m_spells.hunter.pFeignDeath) != SPELL_CAST_OK)
    {
        HoldPet(false);
        return false;
    }

    m_trapAttemptStart = now;

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] trapsetup bot='%s' feigned to lay a trap for '%s' (lvl %u) at %.1fy",
                 me->GetName(), pAdd->GetName(), pAdd->GetLevel(), me->GetDistance(pAdd));
    }


    // Deliberately no trap attempt in this tick. DoCastSpell returning OK means the cast started,
    // not that the aura is on, so Feign Death has not dropped combat yet and the trap is refused
    // with SPELL_FAILED_AFFECTING_COMBAT every time -- which is exactly what the log shows,
    // "feigned but the trap was refused: ok (incombat=1)", three builds running. The feigned
    // branch at the top of this function takes the next tick, and the deadline bounds it.
    return true;
}

Unit* PartyBotAI::SelectPartyAttackTarget() const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    // The first thing found that is hitting somebody, and the first thing found that the instance
    // says is worth killing before the rest of its pack. Both are collected in one pass because
    // the second only ever breaks a tie between candidates the first would have accepted: it never
    // reaches past the group for a target, and it never overrules a mark or the leader's choice,
    // both of which are settled before this is called.
    Unit* pAnyAttacker = nullptr;
    Unit* pPreferred = nullptr;
    Unit* pMarked = nullptr;

    auto const consider = [&](Unit* pAttacker)
    {
        if (!IsValidHostileTarget(pAttacker) || !me->IsWithinDist(pAttacker, 50.0f))
            return;

        if (IsIgnoredByParty(pAttacker))
            return;

        if (!pAnyAttacker)
            pAnyAttacker = pAttacker;

        // A focus mark outranks everything else here. This scan is what the tank's defend rule
        // reads, and it had no notion of marks at all, so the one command that exists to say
        // "kill this" was the one thing it could not see.
        if (!pMarked)
        {
            for (auto markId : m_marksToFocus)
            {
                if (pGroup->GetTargetWithIcon(markId) == pAttacker->GetObjectGuid())
                {
                    pMarked = pAttacker;
                    break;
                }
            }
        }

        if (!pPreferred && m_tactics)
            if (Creature const* pCreature = pAttacker->ToCreature())
                if (m_tactics->IsFocusFirst(pCreature->GetEntry()) &&
                    IsSummonWorthLeavingBossFor(pAttacker) &&
                    CanEngageFromHeldGround(pAttacker))
                    pPreferred = pAttacker;
    };

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // We already checked self.
            if (pMember == me)
                continue;

            for (const auto pAttacker : pMember->GetAttackers())
                consider(pAttacker);
        }
    }

    if (pMarked)
        return pMarked;

    // Nothing in the group is being hit by a focus target, which is not the same as there being
    // none. The scan above walks GetAttackers, so it can only ever see creatures that are hitting
    // somebody -- and the whole reason the worst of these entries are on the list is that they do
    // not hit anybody. A Greater Healing Ward heals, an Earthgrab Totem roots, a Ward of Zum'rah
    // raises skeletons; not one of them appears in any player's attacker list, ever. So the
    // instance's kill-on-sight list silently covered only the half of itself that fights back, and
    // Antu'sul's ward healed him through three wipes without a bot ever selecting it.
    //
    // Only while the group is already fighting, and only within the radius the scan above uses, so
    // this stays a rule about the fight in progress rather than a licence to wander off and pull
    // the next room's totem.
    // Damage dealers only. The first cut of this let any role take the job and the combat log
    // came back with five lines of the healer walking off to hit a totem, which is a worse outcome
    // than the totem living: the ward heals the boss, but the healer not healing kills the group.
    // The tank is excluded for the same reason in reverse -- whatever it is holding goes with it.
    bool const canLeaveItsPostForThis = (GetRole() != ROLE_HEALER && GetRole() != ROLE_TANK);

    if (!pPreferred && canLeaveItsPostForThis && m_tactics && !m_tactics->focusFirst.empty() &&
        me->IsInCombat())
    {
        std::list<Unit*> nearby;
        me->GetEnemyListInRadiusAround(me, 50.0f, nearby);

        for (Unit* pUnit : nearby)
        {
            Creature const* pCreature = pUnit->ToCreature();
            if (!pCreature || !m_tactics->IsFocusFirst(pCreature->GetEntry()))
                continue;

            if (!IsValidHostileTarget(pUnit) || !IsSummonWorthLeavingBossFor(pUnit))
                continue;

            // The measured failure: this scan is fifty yards and the courtyard floor is forty
            // seven to fifty from the middle of the landing, so it reached past the hold line for
            // Acolytes nobody could touch.
            if (!CanEngageFromHeldGround(pUnit))
                continue;

            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] idlefocus bot='%s' role=%s picked '%s' (lvl %u, %.1fy) "
                         "which is hitting nobody but the instance says kill it first",
                         me->GetName(), GetRoleName(GetRole()), pCreature->GetName(),
                         pCreature->GetLevel(), me->GetDistance(pUnit));
            }

            pPreferred = pUnit;
            break;
        }
    }

    if (pPreferred)
        return pPreferred;

    if (pAnyAttacker)
        return pAnyAttacker;

    // Nothing is on the group, which in an escort is exactly the moment the escort is being eaten.
    // Checked last so that a party member under attack always outranks an NPC under attack.
    return SelectEscortAttackTarget();
}

void PartyBotAI::RefreshDungeonTactics()
{
    // Looked up on a change of map rather than every tick, because the answer cannot change while
    // the bot stands still and the lookup walks a table. A bot is not reinitialised when it is
    // summoned into an instance, so the map it was last asked about is what says whether to ask
    // again.
    if (m_tacticsMapId == me->GetMapId())
        return;

    m_tacticsMapId = me->GetMapId();
    m_tactics = GetDungeonTactics(m_tacticsMapId);
}

bool PartyBotAI::GetFightAnchor(Unit const* pVictim, float& x, float& y, float& z,
                                float& radius) const
{
    if (!m_tactics || !pVictim)
        return false;

    Creature const* pCreature = pVictim->ToCreature();
    if (!pCreature)
        return false;

    return m_tactics->GetFightAnchor(pCreature->GetEntry(), x, y, z, radius);
}

// Whether stepping to a spot would take the bot off ground it is holding.
//
// Asked of where the bot is now as well as of where it is going, so the rule only binds a bot that
// is already on the ground in question. One that has not reached it yet is not held back from
// walking to it, and one the leader has walked away is not dragged back to it.
bool PartyBotAI::WouldLeaveHeldGround(float x, float y, float z) const
{
    if (!m_tactics || m_tactics->holdLines.empty())
        return false;

    DungeonHoldLine const* pHere = m_tactics->GetHoldLineAt(me->GetPositionX(), me->GetPositionY(),
                                                            me->GetPositionZ());
    if (!pHere)
        return false;

    return m_tactics->GetHoldLineAt(x, y, z) != pHere;
}

// Walk back onto ground the instance says to hold, having been pushed off it.
//
// The refusal above is only half of a hold line and this is the half Zul'Farrak needs. A bot that
// is standing on the landing will not follow a troll down the stairs; a bot that has been *thrown*
// down them by Shadowpriest Sezz'ziz's Psychic Scream is outside every zone, so by construction
// nothing holds it any more and it fights out the rest of the event at the bottom, among the
// trolls that have not been released yet. Which is the same losing position the refusal exists to
// prevent, arrived at from the other direction.
//
// Only in combat, and only from inside the recovery radius. Out of combat the follow already puts
// the bot wherever the leader is, and dragging it back to a landing the group has deliberately
// left would strand it there -- the Bly fight is at the foot of these very stairs, so this must
// not be a rule that says "always be at the top".
bool PartyBotAI::ReturnToHeldGround()
{
    if (!m_tactics || m_tactics->holdLines.empty() || IsInDuel())
        return false;

    if (!me->IsInCombat() || m_holdPosition || IsPulling() || me->HasAttackOrders())
        return false;

    // Nothing can be walked anywhere in these states, and a feared bot is still being moved by the
    // fear -- ordering a walk under it is a fight between two movement generators that the fear
    // wins. The recovery happens on the tick after it wears off, which is soon enough.
    if (me->IsMounted() || me->IsNonMeleeSpellCasted())
        return false;

    if (me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING))
        return false;

    if (!CanIssueCombatMovement())
        return false;

    DungeonHoldLine const* pLine = m_tactics->GetHoldLineToRecover(
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
    if (!pLine)
        return false;

    // Only back to ground the party leader is still holding.
    //
    // Distance alone cannot tell "thrown off the landing" from "the group has moved on", and in
    // Zul'Farrak the two are barely thirty yards apart: Bly's crew are moved to the foot of the
    // stairs for wave three, about fifty three yards from the middle of the landing, and the third
    // wave itself is spawned down there for the group to come to. A radius wide enough to bring a
    // feared bot back up the stairway also reaches the fight at the bottom of it.
    //
    // The leader and nobody else, which is the correction to the first version of this rule. That
    // one asked whether *any* party member was still in the zone, and it deadlocked exactly where
    // it mattered: one bot left on the landing made every other bot recover to the landing, which
    // kept a bot on the landing. The measured run has the party walked down for wave three and
    // then dragged back up in forty nine yard legs, over and over, while the healer stood at the
    // top refusing to follow Nekrum down -- a stable oscillation that cost the whole event with
    // nobody dying.
    //
    // A leader is the right authority anyway. It carries the intent these bots already follow
    // everywhere else, it cannot be dragged anywhere by this rule because it is not subject to it,
    // and so a player standing at the choke means hold while a player walking down means go.
    Player* const pLeader = GetPartyLeader();
    if (!pLeader || !pLeader->IsInWorld() || pLeader->GetMap() != me->GetMap())
        return false;

    if (m_tactics->GetHoldLineAt(pLeader->GetPositionX(), pLeader->GetPositionY(),
                                 pLeader->GetPositionZ()) != pLine)
        return false;

    // Straight to the middle of it rather than to the nearest edge. The edge of a hold line is the
    // top step of a staircase, and a bot that stops there is one knockback from being back where
    // it started.
    if (!SafeMoveTo(pLine->x, pLine->y, pLine->z))
        return false;

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] holdline bot='%s' role=%s walked %.1fy back onto the ground it is "
                 "holding at (%.1f %.1f %.1f), from %.1f %.1f %.1f",
                 me->GetName(), GetRoleName(GetRole()),
                 me->GetDistance(pLine->x, pLine->y, pLine->z),
                 pLine->x, pLine->y, pLine->z,
                 me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
    }

    NoteCombatMovement();
    return true;
}

// Step into sight of somebody the healer can reach but cannot see.
//
// A healer that is in range of a dying party member and blocked only by a corner is the most
// expensive kind of doing nothing in this file, and it is what ended the first Zul'Farrak run:
// the priest logged wreason=no_los against a hunter at seventeen point eight yards -- well inside
// its forty yard reach -- on two consecutive ticks, at thirty seven percent health and then at
// eighteen, and the hunter died between them.
//
// The step already existed. It was unreachable. It sat under `if (!pVictim)` in the movement
// block, meaning a healer only went looking for line of sight when it had no attack target at
// all -- and a healer in combat is nearly always wanding something, so the branch never ran in
// the one situation it was written for. Hoisted out here it is asked on its own terms, every
// tick, the way RecoverLineOfSight already is for the bot's own target.
//
// Deliberately not subject to the hold, for the reason given above RecoverLineOfSight: holding a
// choke means not closing on the mob, and has never meant standing behind a rock while the group
// dies. SafeMoveTo still refuses anything that would leave held ground, so a healer on a landing
// looks for sight on the landing.
bool PartyBotAI::RecoverHealLineOfSight()
{
    if (m_role != ROLE_HEALER || !me->IsInCombat() || IsInDuel())
        return false;

    if (me->IsMoving() || me->IsMounted() || me->IsNonMeleeSpellCasted())
        return false;

    if (me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING))
        return false;

    if (!CanIssueCombatMovement())
        return false;

    Unit* const pBlind = SelectHealTargetOutOfReach();
    if (!pBlind)
        return false;

    // Only the sight case. Out of range is the follow's job and is answered below in the tick;
    // stepping ten yards at something forty yards away would be neither.
    float const reach = GetMaxHealSpellRange();
    if (!me->IsWithinDist(pBlind, reach) || me->IsWithinLOSInMap(pBlind))
        return false;

    float x, y, z;
    if (!FindFiringPosition(pBlind, 0.0f, reach, PB_HEAL_SIGHT_STEP_TRAVEL, x, y, z))
        return false;

    if (!SafeMoveTo(x, y, z))
        return false;

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] healsight bot='%s' stepped to (%.1f %.1f) to see '%s' at %.1fy on "
                 "%.0f%% health, which it could reach and not see",
                 me->GetName(), x, y, pBlind->GetName(), me->GetDistance(pBlind),
                 pBlind->GetHealthPercent());
    }

    NoteCombatMovement();
    return true;
}

// Whether this bot could actually fight this target without leaving the ground it is holding.
//
// The other half of the hold line, and the half the first Zul'Farrak run was missing. Refusing the
// chase stops a bot walking down the stairs; it does nothing about the bot *choosing* something at
// the bottom of them, and the two rules met badly. The focus scan reaches fifty yards, the
// courtyard floor is forty seven to fifty from the middle of the landing, and Sandfury Acolytes
// are on the instance's kill-first list for their Mana Burn -- so the party repeatedly settled on
// unreleased Acolytes standing at the foot of the stairs, measured at 49.1, 49.4 and 49.6 yards.
// A hundred and thirteen of the hundred and thirty chase refusals in that run were Acolytes. The
// bot then stands there holding a target it may not approach and, at that range, cannot shoot
// either.
//
// A target off the held ground is still allowed when the bot can hit it from where it stands,
// which is the whole of what a ranged bot on a choke is for. Melee cannot, and so declines it.
bool PartyBotAI::CanEngageFromHeldGround(Unit const* pTarget) const
{
    if (!pTarget)
        return false;

    if (!WouldLeaveHeldGround(pTarget->GetPositionX(), pTarget->GetPositionY(),
                              pTarget->GetPositionZ()))
        return true;

    // Off the ground, so the only question left is whether it can be reached with something that
    // is not a walk. Melee range is deliberately not counted: a melee bot already within swing
    // range of something off the line will not stay there, because the target moves and the chase
    // after it is exactly what this exists to refuse.
    if (!IsRangedDamageClass(me->GetClass()) ||
        IsAttackSpeedOverridenForm(me->GetShapeshiftForm()))
        return false;

    // Thirty yards, which is what GetMaxHealSpellRange falls back to for the same reason: there is
    // no damage-spell list to measure, and thirty is the reach of the long end of a vanilla
    // spellbook. Being approximate is affordable here because the distances this arbitrates are
    // not close -- the landing is thirty three yards above the courtyard and forty seven from it,
    // so nothing the rule refuses was within twenty yards of being shootable.
    constexpr float RANGED_REACH = 30.0f;

    return me->IsWithinDist(pTarget, RANGED_REACH) && me->IsWithinLOSInMap(pTarget);
}

// The instance's kill-on-sight list doubles as the list of things worth waking.
//
// These two rules were written apart and met badly. Focus-first says a Greater Healing Ward has
// to die or the boss does not; pull avoidance says do not walk within twenty six yards of an
// unengaged level forty eight creature. A ward is unengaged by construction -- it has no melee, it
// never chases, it never enters combat on its own -- so avoidance refused every approach to it
// permanently, and focus-first silently picked a target no bot would ever walk to. The Zul'Farrak
// log is unambiguous: eleven refusals to close on wards and totems, and not one bot ever swinging
// at one.
//
// Saying it once here rather than exempting totems by creature type, because the type does not
// separate them: Earthgrab Totem is CREATURE_TYPE_TOTEM and Greater Healing Ward is
// CREATURE_TYPE_NOT_SPECIFIED, and both matter for the same reason.
std::vector<uint32> const* PartyBotAI::GetApproachAnywayEntries() const
{
    if (!m_tactics || m_tactics->focusFirst.empty())
        return nullptr;

    return &m_tactics->focusFirst;
}

// Whether this creature is one the instance's tactics say to treat as part of the fight even
// though nothing has engaged it -- the boss's own totems and wards, which are summoned into a
// fight already in progress and can never be the start of a second one.
bool PartyBotAI::IsApproachAnywayTarget(Unit const* pTarget) const
{
    std::vector<uint32> const* pEntries = GetApproachAnywayEntries();
    if (!pEntries || !pTarget)
        return false;

    Creature const* pCreature = pTarget->ToCreature();
    if (!pCreature)
        return false;

    return std::find(pEntries->begin(), pEntries->end(), pCreature->GetEntry()) != pEntries->end();
}

float PartyBotAI::GetTacticalStandoff(Unit const* pTarget) const
{
    if (!m_tactics || !pTarget)
        return 0.0f;

    Creature const* pCreature = pTarget->ToCreature();
    if (!pCreature)
        return 0.0f;

    return m_tactics->GetRangedStandoff(pCreature->GetEntry());
}

// Whether this enemy is part of the fight the group is already having, as opposed to something
// standing in the room minding its own business.
//
// Every tactic below spends something on the answer - an interrupt, a taunt, a bot's attention -
// and spending any of it on a creature the group has not engaged is how a pull becomes two pulls.
bool PartyBotAI::IsEngagedWithGroup(Unit const* pEnemy) const
{
    if (!pEnemy || !pEnemy->IsInCombat())
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsInWorld() || pMember->GetMap() != me->GetMap())
            continue;

        if (pEnemy->GetVictim() == pMember)
            return true;

        for (const auto pAttacker : pMember->GetAttackers())
            if (pAttacker == pEnemy)
                return true;

        // And the other direction, which this was missing entirely: a mob the group is attacking
        // is the group's fight whether or not it is hitting anybody back yet.
        //
        // Both tests above ask "is this mob on us". In an escort fight the answer is routinely no
        // while the group is killing it, because it is busy with the escort -- and in Zul'Farrak
        // that is the entire premise of the event, since the trolls come up the stairs into Bly
        // and his four. One measured run: two hundred and fourteen of two hundred and fifteen
        // group focus changes logged "from nothing", two hundred and one of them inside forty
        // seconds, which is about five target switches a second across the party. The held focus
        // was being discarded every tick because the mob the group had settled on was swinging at
        // Bly, so nothing was ever killed, everyone took damage for the full duration, and the
        // healer emptied its bar holding up a fight that was making no progress.
        if (pMember->GetVictim() == pEnemy)
            return true;

        // A pet holding the mob counts. A hunter's pet is often the only thing on an add for the
        // first few seconds, and those are the seconds an interrupt is wanted in.
        if (Pet* pPet = pMember->GetPet())
        {
            if (pEnemy->GetVictim() == pPet)
                return true;

            for (const auto pAttacker : pPet->GetAttackers())
                if (pAttacker == pEnemy)
                    return true;
        }
    }

    // Anything fighting an ally this instance asked the group to keep alive.
    //
    // The escort is not a group member, so none of the tests above can see it, and in a fight
    // built around one the mobs spend most of their time on it rather than on the party. Left out,
    // the group treats every such mob as somebody else's business the instant it turns to face the
    // NPC it was summoned to kill -- which is the moment the group most needs to stay on it.
    std::vector<Creature*> escorts;
    FindGuardedEscorts(escorts);
    for (Creature const* pEscort : escorts)
    {
        if (pEnemy->GetVictim() == pEscort)
            return true;

        for (const auto pAttacker : pEscort->GetAttackers())
            if (pAttacker == pEnemy)
                return true;
    }

    return false;
}

// The one ability this bot can stop a cast with, or nothing.
//
// Deliberately a single answer rather than a list. A bot gets one attempt per tick and the choice
// between two interrupts is never interesting: what matters is that the bot has one at all, and
// most of them do not.
// Whether this target belongs to the fight the group is actually having.
//
// The rule a tank has to obey and did not: never bring in something nobody has pulled. A tank that
// picks its target from anything it can see turns one fight into two, and it does so at the worst
// moment, because the reason it is looking for a new target at all is that the current one is
// keeping it busy.
//
// Two exemptions, both of them explicit instructions rather than choices the bot made: an attack
// order from .partybot attackstart, and a pull in progress. Refusing those would be refusing the
// command, and starting a fight is precisely what they are for.
bool PartyBotAI::IsTargetInCurrentFight(Unit const* pTarget) const
{
    if (!pTarget)
        return false;

    if (IsInDuel())
        return true;

    if (me->HasAttackOrders() && me->GetAttackOrders() == pTarget->GetObjectGuid())
        return true;

    if (IsPulling())
        return true;

    return IsEngagedWithGroup(pTarget);
}

// Every interrupt this bot owns, best first.
//
// A list rather than a single choice, because returning one meant a warrior whose Shield Bash was
// on cooldown had no interrupt at all: the caller asked once, got a spell it could not cast, and
// gave up without ever trying Pummel. Real interrupts lead, stuns follow, since a stun is refused
// outright by anything immune to it and generally costs a much longer cooldown.
void PartyBotAI::GetInterruptSpells(std::vector<SpellEntry const*>& out) const
{
    out.clear();

    auto add = [&out](SpellEntry const* pSpellEntry)
    {
        if (pSpellEntry)
            out.push_back(pSpellEntry);
    };

    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
            // Shield Bash ahead of Pummel because it is the one a tank can use without leaving
            // Defensive Stance - but only with a shield actually equipped. CanTryToCastSpell does
            // not check the weapon class, which the first live run made obvious: twenty Shield
            // Bashes refused with SPELL_FAILED_EQUIPPED_ITEM_CLASS, every one of them an interrupt
            // the group did not get.
            if (IsWearingShield(me))
                add(m_spells.warrior.pShieldBash);
            add(m_spells.warrior.pPummel);
            add(m_spells.warrior.pConcussionBlow);
            break;
        case CLASS_ROGUE:
            add(m_spells.rogue.pKick);
            // Kidney Shot only with something to spend. It costs combo points, and offering it
            // without any meant the driver kept reaching for it and the server kept refusing:
            // one rogue attempted it seventy six times in a run and landed six, the other seventy
            // rejected with SPELL_FAILED_NO_COMBO_POINTS. Kick is the interrupt that costs no bar,
            // so it stays first and this remains the fallback for when Kick is down.
            if (me->GetComboPoints())
                add(m_spells.rogue.pKidneyShot);
            break;
        case CLASS_SHAMAN:
            add(m_spells.shaman.pEarthShock);
            break;
        case CLASS_DRUID:
            add(m_spells.druid.pBash);
            break;
        case CLASS_MAGE:
            add(m_spells.mage.pCounterspell);
            break;
        case CLASS_PALADIN:
            add(m_spells.paladin.pHammerOfJustice);
            break;
    }

    // War Stomp, which every tauren is given at character creation and no bot has ever used. It is
    // a two second stun on everything nearby, off a racial cooldown that competes with nothing
    // else the bot wants, and it is the only interrupt a tauren hunter has at any level and the
    // only one a warrior has below thirty eight without a shield, Pummel being trained that late.
    // Last in the list because it is a stun and because it hits more than the intended target.
    if (me->HasSpell(PB_SPELL_WAR_STOMP))
        add(sSpellMgr.GetSpellEntry(PB_SPELL_WAR_STOMP));
}

// Whether what this creature is casting is worth an interrupt.
//
// Two kinds of cast, and the group cannot answer either by out-damaging it.
//
// A heal undoes work already paid for: the alternative to stopping it is dealing the same damage
// twice. Crowd control aimed at the group takes a player out of the fight altogether, and at low
// level losing the healer for six seconds is the fight. Everything else a mob casts is damage,
// which is what a healer is for, and choosing among damage spells needs encounter knowledge a boss
// script has and a bot does not.
//
// Not gated on the instance, deliberately. Wailing Caverns is where the absence was measured -
// every Fanglord and every Druid of the Fang casts both Healing Touch and a sleep, a group at that
// level does not out-damage the healing, and a slept healer is a wipe - but nothing about the
// reasoning is particular to that dungeon, and gating it there would leave the same hole
// everywhere else.
// What a hostile cast is worth taking away, rather than merely whether it is worth anything.
//
// This was a yes or no question and the answer was no for every spell that was only damage, which
// left an interrupt sitting on cooldown while a nuke landed. Every named druid in Wailing Caverns
// casts Lightning Bolt, it has a real cast time, and nothing ever stopped one: the group would
// hold Kick for a heal that might not come while taking the bolt that certainly did.
//
// Ranked rather than listed, so a bot with one interrupt spends it on the worst thing in range and
// a bot with a spare one still uses it on something.
// What one spell is worth taking away, judged from the spell alone.
//
// Separated from the live-cast version so the same ranking can be applied to a spell a creature
// merely knows, which is what lets a bot decide in advance what to save its interrupt for.
static uint32 ScoreSpellForInterrupt(SpellEntry const* pSpellEntry, Unit const* pCaster = nullptr)
{
    if (!pSpellEntry)
        return PB_INTERRUPT_NONE;

    // Crowd control first, ahead of healing. A landed sleep on the healer loses the fight; a
    // landed heal only makes it longer. Asked of the mechanic rather than of a list of spell ids,
    // so it covers Sleep, Druid's Slumber and Naralex's Nightmare without naming any of them, and
    // covers whatever the next instance uses without being told. Effect mechanics are checked too,
    // because plenty of spells carry theirs on the effect and leave the spell's own field at zero.
    auto isControlMechanic = [](uint32 mechanic)
    {
        switch (mechanic)
        {
            case MECHANIC_CHARM:
            case MECHANIC_FEAR:
            case MECHANIC_SLEEP:
            case MECHANIC_STUN:
            case MECHANIC_POLYMORPH:
            case MECHANIC_BANISH:
            case MECHANIC_SHACKLE:
            case MECHANIC_HORROR:
            case MECHANIC_KNOCKOUT:
            case MECHANIC_SILENCE:
            case MECHANIC_ROOT:
                return true;
        }
        return false;
    };

    if (isControlMechanic(pSpellEntry->Mechanic))
        return PB_INTERRUPT_CONTROL;

    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (isControlMechanic(pSpellEntry->EffectMechanic[i]))
            return PB_INTERRUPT_CONTROL;

    if (pSpellEntry->IsHealSpell())
    {
        // How much of its own bar the caster gets back. Without a caster to measure against there
        // is no scale to judge it on, so the heal keeps the higher tier: an unknown heal is
        // treated as worth stopping, which is the safe direction to be wrong in.
        if (!pCaster || !pCaster->GetMaxHealth())
            return PB_INTERRUPT_HEAL;

        int32 healed = 0;
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (pSpellEntry->Effect[i] != SPELL_EFFECT_HEAL &&
                pSpellEntry->Effect[i] != SPELL_EFFECT_HEAL_MAX_HEALTH)
                continue;

            // A heal to full is the whole bar by definition, whatever its base points say.
            if (pSpellEntry->Effect[i] == SPELL_EFFECT_HEAL_MAX_HEALTH)
                return PB_INTERRUPT_HEAL;

            healed += pSpellEntry->CalculateSimpleValue(SpellEffectIndex(i)) +
                      int32(pSpellEntry->EffectDieSides[i]) / 2;
        }

        float const share = float(healed) / float(pCaster->GetMaxHealth());
        return share >= PB_INTERRUPT_BIG_HEAL_SHARE ? PB_INTERRUPT_HEAL : PB_INTERRUPT_MINOR_HEAL;
    }

    // Anything else with a cast time long enough to take away. Summons rank with damage: both are
    // work the group has to undo afterwards.
    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        switch (pSpellEntry->Effect[i])
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_SUMMON:
            case SPELL_EFFECT_WEAPON_DAMAGE:
                return PB_INTERRUPT_DAMAGE;
        }
    }

    // A hostile creature spending a cast on itself. Recognised by who it is aimed at rather than
    // by what it does, and that is the point: the classification above is a list of aura types
    // and effects, and the buffs that matter most do not appear in one. Shadowfang Moonwalker's
    // Anti-Magic Shield is a two second cast whose whole effect is a script hook hanging off a
    // dummy aura, so it carries no mechanic, heals nobody and deals no damage -- it scored zero
    // and the bots let it through every time, and it makes the mob immune to the entire caster
    // half of the group with no way to remove it afterwards, since it is not dispellable either.
    //
    // Reading the target instead covers that spell, every other absorb or immunity, and every
    // enrage and haste buff, without naming any of them. The reasoning is simple enough to trust:
    // a hostile spending two seconds casting at itself is doing something it wants and the group
    // does not, so taking it away is never wrong. It ranks lowest because it is also never
    // urgent, and the hold below already keeps an interrupt back when the mob has worse to come.
    bool selfCast = false;
    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (!pSpellEntry->Effect[i])
            continue;

        // Every effect that names a target has to name the caster. A spell with one effect on
        // itself and another on the group is not a self buff, it is that other thing.
        if (pSpellEntry->EffectImplicitTargetA[i] != TARGET_UNIT_CASTER)
            return PB_INTERRUPT_NONE;

        if (pSpellEntry->EffectImplicitTargetB[i] &&
            pSpellEntry->EffectImplicitTargetB[i] != TARGET_UNIT_CASTER)
            return PB_INTERRUPT_NONE;

        selfCast = true;
    }

    return selfCast ? PB_INTERRUPT_BUFF : PB_INTERRUPT_NONE;
}

// Whether a spell takes long enough to cast that there is anything to interrupt.
//
// An instant spell can never be caught, so it is not something to hold an ability for: Pythas's
// Thunderclap is an AoE stun and would otherwise be the worst thing in his book, which would have
// a rogue saving Kick all fight for a cast that never exists.
static bool IsInterruptibleCast(SpellEntry const* pSpellEntry)
{
    if (!pSpellEntry)
        return false;

    if (SpellCastTimesEntry const* pCastTime = sSpellCastTimesStore.LookupEntry(pSpellEntry->CastingTimeIndex))
        if (pCastTime->CastTime > 0)
            return true;

    // A channel is interruptible even where the initial cast is instant.
    return pSpellEntry->IsChanneledSpell();
}

// The worst thing this creature is known to be able to cast, whether or not it is casting now.
//
// Read out of the creature's own spell list and its EventAI scripts, which is where the answer has
// always been: the bot was reacting to whatever cast happened to be in progress with no idea
// whether something worse was coming from the same mob. A Druid of the Fang casts Lightning Bolt
// constantly and Druid's Slumber occasionally, so a rogue that spends Kick on the first bolt it
// sees has nothing left for the sleep, and the sleep is the entire reason to bring an interrupt.
//
// Cached by creature entry, since the answer is a property of the creature template and does not
// change while the server is up.
uint32 PartyBotAI::GetWorstKnownCastPriority(Unit const* pEnemy) const
{
    Creature const* pCreature = pEnemy->ToCreature();
    if (!pCreature)
        return PB_INTERRUPT_NONE;

    uint32 const entry = pCreature->GetEntry();

    static std::unordered_map<uint32, uint32> s_worstKnownCast;

    auto cached = s_worstKnownCast.find(entry);
    if (cached != s_worstKnownCast.end())
        return cached->second;

    uint32 worst = PB_INTERRUPT_NONE;

    auto consider = [&worst, pEnemy](uint32 spellId)
    {
        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spellId);
        if (!IsInterruptibleCast(pSpellEntry))
            return;

        worst = std::max(worst, ScoreSpellForInterrupt(pSpellEntry, pEnemy));
    };

    // The assigned spell list, which is how most casters are given their abilities.
    if (uint32 const spellListId = pCreature->GetCreatureInfo()->spell_list_id)
        if (CreatureSpellsList const* pList = sObjectMgr.GetCreatureSpellsList(spellListId))
            for (CreatureSpellsEntry const& spell : *pList)
                consider(spell.spellId);

    // And the EventAI scripts, which is how the rest are. The spell is buried in a generic script
    // action rather than named on the event, so the actions have to be walked.
    CreatureEventAI_Event_Map const& eventMap = sEventAIMgr.GetCreatureEventAIMap();
    auto events = eventMap.find(entry);
    if (events != eventMap.end())
    {
        for (CreatureEventAI_Event const& event : events->second)
        {
            for (uint32 i = 0; i < MAX_ACTIONS; ++i)
            {
                if (!event.action[i])
                    continue;

                for (auto const& script : *event.action[i])
                    if (script.second.command == SCRIPT_COMMAND_CAST_SPELL)
                        consider(script.second.castSpell.spellId);
            }
        }
    }

    s_worstKnownCast[entry] = worst;
    return worst;
}

uint32 PartyBotAI::GetInterruptPriority(Unit const* pCaster) const
{
    // A cast in progress, or a channel already running. Only the first was ever checked, so
    // anything channelled was invisible to this: no Wailing Caverns mob channels, but plenty
    // elsewhere do, and a channel is exactly the case where interrupting pays most.
    Spell const* pSpell = pCaster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!pSpell || pSpell->getState() != SPELL_STATE_PREPARING)
    {
        pSpell = pCaster->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!pSpell || pSpell->getState() != SPELL_STATE_CASTING)
            return PB_INTERRUPT_NONE;
    }

    SpellEntry const* pSpellEntry = pSpell->m_spellInfo;
    if (!pSpellEntry)
        return PB_INTERRUPT_NONE;

    return ScoreSpellForInterrupt(pSpellEntry, pCaster);
}

// Whether this spell takes a cast away outright, rather than by happening to stun.
//
// The distinction matters because a stun is refused by anything stun immune, which is most of what
// matters in a raid, while an interrupt effect lands regardless. Silence immunity does not enter
// into it: Creature::LockOutSpells is what silence immunity blocks, and that is the school lockout
// afterwards, not the interruption itself.
static bool IsHardInterrupt(SpellEntry const* pSpellEntry)
{
    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (pSpellEntry->Effect[i] == SPELL_EFFECT_INTERRUPT_CAST)
            return true;

    return false;
}

Unit* PartyBotAI::SelectInterruptTarget(SpellEntry const* pInterruptSpell, uint32 minPriority,
                                        bool mayPreempt) const
{
    if (!pInterruptSpell)
        return nullptr;

    // A stun cannot interrupt something immune to being stunned, so a stun-only ability has to
    // check before it is spent. Nothing did, and a Bash into a stun immune boss read in the log
    // exactly like a successful interrupt.
    bool const stunOnly = !IsHardInterrupt(pInterruptSpell);

    // Look no further than the ability reaches. Earth Shock is twenty yards and Kick is melee, so
    // the same search serves both and neither wastes a scan on ground it cannot act on.
    float range = 5.0f;
    if (SpellRangeEntry const* pRange = sSpellRangeStore.LookupEntry(pInterruptSpell->rangeIndex))
        range = pRange->maxRange;

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, range, enemies);

    // A stun-immune target is not worth considering for a stun.
    auto canBeStopped = [&](Unit const* pEnemy)
    {
        if (!stunOnly)
            return true;

        return !pEnemy->IsImmuneToMechanic(MECHANIC_STUN);
    };

    // Worst cast in range wins, so a single interrupt is spent on the sleep rather than on
    // whichever nuke happened to be found first. The bot's own target breaks ties, because
    // interrupting the mob already being hit costs nothing beyond the ability, where interrupting
    // anything else adds the bot to a second creature's threat list.
    Unit* pBest = nullptr;
    uint32 bestPriority = PB_INTERRUPT_NONE;

    auto consider = [&](Unit* pEnemy, bool isOwnVictim)
    {
        if (!pEnemy || !IsValidHostileTarget(pEnemy) || !me->IsWithinLOSInMap(pEnemy))
            return;

        if (!me->IsWithinDist(pEnemy, range) || !canBeStopped(pEnemy))
            return;

        if (WasRecentlyInterrupted(pEnemy->GetObjectGuid()))
            return;

        uint32 const priority = GetInterruptPriority(pEnemy);
        if (priority < minPriority)
            return;

        // Hold, when this mob is known to have something worse in its book than the damage it is
        // casting now. This is the whole of the pre-decision: a Druid of the Fang casts Lightning
        // Bolt constantly and Druid's Slumber occasionally, so spending Kick on the first bolt
        // leaves nothing for the sleep, and the sleep is the entire reason to carry an interrupt.
        //
        // A heal that is worth the cooldown is never held. Healing Touch returns most of a Druid
        // of the Fang's health bar, an interrupt is back inside ten seconds, and holding one
        // indefinitely against a sleep that may never be cast while a full heal lands in front of
        // you is worse play than not holding at all.
        //
        // A minor heal is held, and that is the one thing above damage that is. It is the same
        // reasoning as the damage case, only applied to a mob whose book holds two heals rather
        // than a bolt and a sleep: spending the cooldown on the small one means it is down when
        // the large one comes, and the large one is the reason the group carries an interrupt.
        if (!mayPreempt &&
            priority <= PB_INTERRUPT_MINOR_HEAL &&
            priority < GetWorstKnownCastPriority(pEnemy))
            return;

        if (priority > bestPriority || (priority == bestPriority && isOwnVictim))
        {
            bestPriority = priority;
            pBest = pEnemy;
        }
    };

    consider(me->GetVictim(), true);

    for (Unit* pEnemy : enemies)
    {
        if (pEnemy == me->GetVictim())
            continue;

        if (!IsEngagedWithGroup(pEnemy))
            continue;

        consider(pEnemy, false);
    }

    return pBest;
}

bool PartyBotAI::InterruptHostileCasters()
{
    if (IsInDuel())
        return false;

    std::vector<SpellEntry const*> candidates;
    GetInterruptSpells(candidates);
    if (candidates.empty())
        return false;

    // Four passes, in decreasing order of how sure the spend is.
    //
    //   Control is taken immediately and always: nothing a mob owns is worse than a sleep on the
    //   healer, so there is never a reason to hold for something better.
    //
    //   Then anything that is the worst thing its caster is known to be able to do. A mob whose
    //   whole book is Lightning Bolt gets its Lightning Bolt interrupted, because waiting for
    //   something worse from that mob means waiting forever.
    //
    //   Then, using only a second or later ability, anything at all. This is what keeps the
    //   holding from becoming hoarding: a warrior owning both Shield Bash and War Stomp saves the
    //   first for what it is worth saving for and spends the second on whatever is being cast,
    //   where a rogue owning only Kick keeps it. Ownership rather than readiness, since a bot with
    //   two abilities has one to spare often enough for the distinction not to earn its keep.
    //
    //   And last, a self buff with the only ability there is, but only from a mob that has nothing
    //   worse. The pass above needs a spare ability and so does not exist for a party of four that
    //   owns one interrupt each, which is most five mans at level twenty.
    struct InterruptPass { uint32 minPriority; bool mayPreempt; uint32 firstAbility; };

    static constexpr InterruptPass passes[] =
    {
        { PB_INTERRUPT_CONTROL, true,  0 },
        { PB_INTERRUPT_DAMAGE,  false, 0 },
        // Down to a self buff on the last pass, and only with an ability to spare. A mob shield
        // is worth taking away and is never worth keeping a lone Kick for, so it belongs exactly
        // here: reachable, and reachable late. Left at DAMAGE this tier could never be selected
        // at all, which would have made recognising it pointless.
        { PB_INTERRUPT_BUFF,    true,  1 },
        // And a self buff with the only ability there is, when it is the worst thing that mob is
        // known to cast. The pass above reads "never keep a lone Kick for a shield", which is the
        // right rule when there is something worse to keep it for and wrong when there is not: a
        // party of four owning one interrupt each never reached that pass at all, so every
        // Shadowfang Moonwalker got its Anti-Magic Shield up, unremovable and undispellable,
        // against a group that had recognised the cast and declined it four times a second.
        //
        // Not a preempt, so the hold in SelectInterruptTarget still applies and still refuses a
        // buff from a mob with worse in its book -- Kick is kept for the sleep, as before. This is
        // the same reasoning the DAMAGE pass already runs on: waiting for something worse from a
        // mob whose worst is this means waiting forever.
        { PB_INTERRUPT_BUFF,    false, 0 },
    };

    for (InterruptPass const& pass : passes)
    {
        for (uint32 i = pass.firstAbility; i < candidates.size(); ++i)
        {
            SpellEntry const* pInterruptSpell = candidates[i];

            Unit* pCaster = SelectInterruptTarget(pInterruptSpell, pass.minPriority, pass.mayPreempt);
            if (!pCaster)
                continue;

            if (!CanTryToCastSpell(pCaster, pInterruptSpell))
                continue;

            // And whether it would do anything if it landed. Spending a ten second Kick on a mob
            // immune to the effect is worse than not having the ability: it reads in the log as
            // an interrupt, so the failure looks like timing.
            if (!CanInterruptWith(pInterruptSpell, pCaster))
                continue;

            // Read before the cast, because interrupting it is what makes it unreadable
            // afterwards. The channel is checked as well as the cast, since either can be the one
            // being taken away.
            SpellEntry const* pInterrupted = nullptr;
            if (Spell const* pTheirSpell = pCaster->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                pInterrupted = pTheirSpell->m_spellInfo;
            else if (Spell const* pTheirChannel = pCaster->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                pInterrupted = pTheirChannel->m_spellInfo;

            uint32 const priority = GetInterruptPriority(pCaster);

            // How much of their cast was still to run when we reached for this. The whole question
            // the log could not answer: three Antu'sul interrupts all reported stopping a Healing
            // Wave and the boss gained a full Wave's health one second after each of them, which
            // is what interrupting the last fraction of a cast looks like from outside. A logged
            // interrupt only ever meant our own ability went off.
            int32 remainingMs = -1;
            if (Spell const* pTheirSpell = pCaster->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                remainingMs = pTheirSpell->GetCastedTime();

            if (DoCastSpell(pCaster, pInterruptSpell) != SPELL_CAST_OK)
                continue;

            NoteGroupInterrupt(pCaster->GetObjectGuid());

            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] interrupt bot='%s' role=%s lvl=%u stopped '%s' (lvl %u) "
                         "casting '%s' with '%s' priority=%u castleft=%dms stillcasting=%u",
                         me->GetName(), GetRoleName(GetRole()), me->GetLevel(),
                         pCaster->GetName(), pCaster->GetLevel(),
                         pInterrupted ? pInterrupted->SpellName[0].c_str() : "something",
                         pInterruptSpell->SpellName[0].c_str(), priority, remainingMs,
                         pCaster->IsNonMeleeSpellCasted(false, false, true) ? 1 : 0);
            }

            return true;
        }
    }

    // Last resort, and only against crowd control.
    //
    // Gouge was removed from the interrupt list on the evidence: sixty six of them in one run, every
    // one incapacitating the mob the whole group was killing, because it was being spent on ordinary
    // damage casts. That reasoning does not reach the case it is kept for here. A sleep landing on
    // the healer costs more than a few seconds of one mob's uptime, and by the time this is reached
    // the real interrupt is on cooldown or unaffordable - the choice is not Gouge against Kick, it
    // is Gouge against the sleep going off. Ninety four of those were missed in one clear.
    if (me->GetClass() == CLASS_ROGUE && m_spells.rogue.pGouge)
    {
        if (Unit* pCaster = SelectInterruptTarget(m_spells.rogue.pGouge, PB_INTERRUPT_CONTROL, true))
        {
            if (CanTryToCastSpell(pCaster, m_spells.rogue.pGouge))
            {
                SpellEntry const* pInterrupted = nullptr;
                if (Spell const* pTheirSpell = pCaster->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                    pInterrupted = pTheirSpell->m_spellInfo;
                else if (Spell const* pTheirChannel = pCaster->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                    pInterrupted = pTheirChannel->m_spellInfo;

                if (DoCastSpell(pCaster, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                {
                    NoteGroupInterrupt(pCaster->GetObjectGuid());

                    if (IsCombatLogged())
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                 "[BotCombat] interrupt bot='%s' role=%s lvl=%u stopped '%s' (lvl %u) "
                                 "casting '%s' with 'Gouge' priority=%u (last resort)",
                                 me->GetName(), GetRoleName(GetRole()), me->GetLevel(),
                                 pCaster->GetName(), pCaster->GetLevel(),
                                 pInterrupted ? pInterrupted->SpellName[0].c_str() : "something",
                                 PB_INTERRUPT_CONTROL);
                    }

                    return true;
                }
            }
        }
    }

    // Nothing was stopped. Worth a line when something was being cast at the group and this bot
    // owned an interrupt for it, because supply was the only half of this that could be measured
    // before: the log recorded interrupts taken and nothing at all about the ones wanted.
    // Two reasons it can end up here and they need telling apart: the ability was unavailable, or
    // it was available and deliberately kept back. The second is a decision and reads as a bug
    // without a line saying so.
    if (IsCombatLogged())
    {
        if (Unit* pWanted = SelectInterruptTarget(candidates.front(), PB_INTERRUPT_BUFF, true))
        {
            uint32 const priority = GetInterruptPriority(pWanted);
            uint32 const worstKnown = GetWorstKnownCastPriority(pWanted);

            std::string detail;

            // The same threshold SelectInterruptTarget holds on, and it has to stay the same. When
            // the minor heal tier went in, the rule moved and this copy did not, so every held
            // Flash Heal was reported as "Kick:refused, Kidney Shot:refused" -- a per-ability
            // reason computed for a cast the bot had deliberately declined and never attempted.
            // One run read as thirty two failed interrupts and was thirty two correct decisions.
            if (priority <= PB_INTERRUPT_MINOR_HEAL && priority < worstKnown)
            {
                detail = "held for something worse";
            }
            // No branch for a lone interrupt declining a self buff any more. It used to say so,
            // and it was the truth for as long as the last pass needed a spare ability, but a
            // buff from a mob with nothing worse is now taken with whatever the bot has. Reaching
            // here with one means something else stopped it, and the per-ability detail below is
            // the answer -- the old line named a policy that no longer applies and hid a cooldown.
            else
            {
                // Which ability, and what stopped it. "None were castable" was true and useless:
                // an interrupt lost to a cooldown is a coverage problem and one lost to an empty
                // rage bar is a rotation problem, and the two want opposite fixes.
                for (SpellEntry const* pCandidate : candidates)
                {
                    if (!detail.empty())
                        detail += ", ";

                    detail += pCandidate->SpellName[0];
                    detail += ":";

                    // First, because it outranks every other reason and is the only one that is
                    // permanent. Everything below describes an interrupt that would have worked
                    // if it had been available; this one describes an interrupt that would not.
                    if (!CanInterruptWith(pCandidate, pWanted))
                        detail += "immune";
                    else if (!me->IsSpellReady(pCandidate))
                        detail += "cooldown";
                    else if (me->GetPower(Powers(pCandidate->powerType)) <
                             Spell::CalculatePowerCost(pCandidate, me))
                        detail += "power";
                    else if (pCandidate->rangeIndex == SPELL_RANGE_IDX_COMBAT
                                 ? !me->CanReachWithMeleeAutoAttack(pWanted)
                                 : !me->IsWithinDist(pWanted, Spells::GetSpellMaxRange(
                                       sSpellRangeStore.LookupEntry(pCandidate->rangeIndex))))
                        detail += "range";
                    // The global cooldown, which "refused" was hiding and which wants a completely
                    // different fix from the rest: an interrupt lost to a cooldown is a coverage
                    // problem, one lost to range is a positioning problem, and one lost to the
                    // global is the bot's own rotation having spent the tick on filler with a heal
                    // already in progress.
                    else if (!me->IsSpellReady(pCandidate, nullptr) || me->HasGCD(pCandidate))
                        detail += "gcd";
                    else if (!me->IsWithinLOSInMap(pWanted))
                        detail += "los";
                    else
                        detail += "refused";
                }
            }

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] nointerrupt bot='%s' role=%s left '%s' casting priority=%u "
                     "(worst it knows is %u) with %u abilities: %s",
                     me->GetName(), GetRoleName(GetRole()), pWanted->GetName(),
                     priority, worstKnown, uint32(candidates.size()), detail.c_str());
        }
    }

    return false;
}

// A creature beating on somebody who is not the tank.
//
// This is the other half of no longer switching targets to chase loose adds. A tank that keeps the
// mob it is holding still has to answer for the one that walked past it, and a taunt is how: it
// costs a cooldown rather than the current target, which is the whole point.
//
// The healer first, because the healer dying ends the fight and everyone else dying merely costs a
// corpse run; then whoever is worst off. The capture this was written from ends with the healer at
// four attackers, thirty two percent health and seventeen percent mana, four seconds before the
// group fell over, with the tank's own log line reading normally throughout.
Unit* PartyBotAI::SelectPeelTarget() const
{
    if (m_role != ROLE_TANK)
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    Player* pHealer = FindGroupHealer();

    Unit* pBest = nullptr;
    bool bestIsOnHealer = false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;

        if (pMember->GetMap() != me->GetMap())
            continue;

        // A second tank is not somebody to be rescued, and taunting off one is how two tanks spend
        // an encounter trading a boss between them.
        if (GetEffectiveRole(pMember) == ROLE_TANK)
            continue;

        bool const isHealer = (pMember == pHealer);

        for (const auto pAttacker : pMember->GetAttackers())
        {
            // Already ours. Taunting it again would spend the cooldown on a mob that is on the
            // tank and standing next to somebody only because they walked to it.
            if (pAttacker->GetVictim() == me)
                continue;

            if (!IsValidHostileTarget(pAttacker) || !me->IsWithinDist(pAttacker, 30.0f))
                continue;

            // Not for things that cannot hurt anybody. One capture has the tank spending a Taunt
            // on a level one Biletoad because it happened to be attacking the healer, which is the
            // cooldown gone for the ten seconds something real might need it.
            if (Creature const* pCreature = pAttacker->ToCreature())
                if (pCreature->GetCreatureType() == CREATURE_TYPE_CRITTER)
                    continue;

            if ((pAttacker->GetLevel() + PB_PEEL_LEVEL_FLOOR) < me->GetLevel())
                continue;

            // Nor for what the group has been told to leave alone. A Broodling was still worth a
            // Taunt to this path at 00:42:02 -- the cooldown gone, and the tank turned away from
            // the boss for it -- because the peel collection filter sits in GatherLooseEnemies
            // and this is a different road to the same mistake.
            if (IsIgnoredByParty(pAttacker))
                continue;

            // One peel per target per cooldown. A mob taunted a moment ago is already running at
            // the tank and has not arrived, so its victim is still whoever it was hitting and it
            // reads as needing the taunt again: one capture taunts a single Deviate Adder five
            // times in forty seconds.
            if (pAttacker->GetObjectGuid() == m_lastPeelGuid &&
                (time(nullptr) - m_lastPeelTime) < PB_PEEL_REPEAT_INTERVAL)
                continue;

            // The healer outranks everyone; within a tier, the biggest thing is the one worth the
            // cooldown, and health is the only proxy for that without a combat log.
            if (!pBest ||
                (isHealer && !bestIsOnHealer) ||
                (isHealer == bestIsOnHealer && pAttacker->GetMaxHealth() > pBest->GetMaxHealth()))
            {
                pBest = pAttacker;
                bestIsOnHealer = isHealer;
            }
        }
    }

    return pBest;
}

// Whether a taunt could be put on this target right now.
//
// Asked so that the tank's two ways of peeling stop competing. They were tried in the wrong order
// and by the wrong measure: the body peel ran first, and it decided whether to walk without ever
// asking whether the cheap tool was available. The same CanTryToCastSpell the taunt itself uses,
// so the answer cannot disagree with what happens a moment later.
bool PartyBotAI::HasTauntReadyFor(Unit const* pTarget) const
{
    if (!pTarget)
        return false;

    for (const auto& pSpellEntry : m_spellListTaunt)
        if (CanTryToCastSpell(pTarget, pSpellEntry))
            return true;

    return false;
}

bool PartyBotAI::PeelForTheHealer()
{
    Unit* pPeelTarget = SelectPeelTarget();
    if (!pPeelTarget)
        return false;

    for (const auto& pSpellEntry : m_spellListTaunt)
    {
        if (!CanTryToCastSpell(pPeelTarget, pSpellEntry))
            continue;

        if (DoCastSpell(pPeelTarget, pSpellEntry) != SPELL_CAST_OK)
            continue;

        m_lastPeelGuid = pPeelTarget->GetObjectGuid();
        m_lastPeelTime = time(nullptr);

        if (IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] peel bot='%s' lvl=%u pulled '%s' (lvl %u) off the group with '%s'",
                     me->GetName(), me->GetLevel(), pPeelTarget->GetName(),
                     pPeelTarget->GetLevel(), pSpellEntry->SpellName[0].c_str());
        }

        return true;
    }

    return false;
}

// The escort this instance asks the group to keep alive, if one is standing near enough to be the
// group's problem.
// Every allied NPC of this instance's that is alive and near enough to be the group's problem.
//
// Plural because Zul'Farrak's are: Bly frees four others with him and all five fight beside the
// group for the pyramid event. Belnistrasz was one, which is why this started as one, and a list
// of one behaves exactly as the single entry did.
void PartyBotAI::FindGuardedEscorts(std::vector<Creature*>& out) const
{
    if (!m_tactics || m_tactics->escortNpcEntries.empty())
        return;

    for (uint32 entry : m_tactics->escortNpcEntries)
    {
        if (Creature* pEscort = me->FindNearestCreature(entry, m_tactics->escortGuardRadius))
        {
            if (!pEscort->IsAlive())
                continue;

            // An ally only for as long as it is one, which in Zul'Farrak is not for ever.
            //
            // Bly's crew are the group's escort for the whole pyramid event and then the group's
            // next fight: when the last troll dies the gossip turns all five hostile, and the
            // table cannot express "these five, until they turn" because the moment is a script
            // event rather than a fact about the creatures. Asked of the faction instead, which is
            // exactly what the script changes.
            //
            // Without this the healer keeps topping up Bly while he kills it, and the escort
            // defence at the bottom of SelectPartyAttackTarget goes looking for whatever is
            // attacking him -- which is the party.
            if (me->IsHostileTo(pEscort))
                continue;

            out.push_back(pEscort);
        }
    }
}

Creature* PartyBotAI::FindGuardedEscort() const
{
    std::vector<Creature*> escorts;
    FindGuardedEscorts(escorts);
    return escorts.empty() ? nullptr : escorts.front();
}

// The heal selectors in CombatBotBaseAI ask for this by name; see the declaration there for why it
// is a hook rather than a direct reach into the tactics table.
//
// The worst off of them, not the nearest. With one escort the two were the same question; with
// five they are not, and answering "nearest" would have a healer topping up Bly at ninety percent
// while Weegli -- who is the only thing that opens the end door -- dies behind him.
Unit* PartyBotAI::GetGuardedEscort() const
{
    std::vector<Creature*> escorts;
    FindGuardedEscorts(escorts);

    // Worst off, with the table's ordering worth a few points of health.
    //
    // Straight "lowest health wins" is what this did, and on a five NPC escort it concentrates
    // everything on whoever happens to be tanking. One measured Zul'Farrak run: forty six escort
    // heals, twenty nine of them on Oro Eyegouge, two on Weegli Blastfuse -- and those two were
    // Greater Heals at fourteen and nineteen percent, because the selector had not looked at him
    // until he was nearly dead. All five died, and Weegli is the only one that opens the end
    // door, so the instance could not be finished either.
    //
    // The bonus is small on purpose. It breaks a near-tie towards the escort the encounter cannot
    // do without; it does not let a scratched Weegli outrank an Oro who is actually dying, which
    // would be the same failure with a different name.
    Creature* pWorst = nullptr;
    float bestScore = 0.0f;
    for (Creature* pEscort : escorts)
    {
        if (pEscort->GetHealth() >= pEscort->GetMaxHealth())
            continue;

        float score = pEscort->GetHealthPercent();
        if (m_tactics)
        {
            uint32 const rank = m_tactics->GetEscortRank(pEscort->GetEntry());
            if (rank == 1)
                score -= CB_ESCORT_FIRST_RANK_BONUS;
            else if (rank == 2)
                score -= CB_ESCORT_SECOND_RANK_BONUS;
        }

        if (!pWorst || score < bestScore)
        {
            bestScore = score;
            pWorst = pEscort;
        }
    }

    // Nothing hurt, so hand back whichever is there: the caller still has its own ceiling to
    // apply, and a full health escort fails it the same way a full health party member does.
    return pWorst ? pWorst : (escorts.empty() ? nullptr : escorts.front());
}

Unit* PartyBotAI::SelectEscortAttackTarget() const
{
    std::vector<Creature*> escorts;
    FindGuardedEscorts(escorts);

    for (Creature* pEscort : escorts)
    {
        if (!pEscort->IsInCombat())
            continue;

        // Only what is actually on him. A bot's threat rules are built entirely around the group,
        // and an escort NPC is not a group member, so without this the party stands and watches
        // the thing it came to protect get eaten by adds that never touched a player.
        for (const auto pAttacker : pEscort->GetAttackers())
            if (IsValidHostileTarget(pAttacker) && me->IsWithinDist(pAttacker, 50.0f))
                return pAttacker;

        if (Unit* pVictim = pEscort->GetVictim())
            if (IsValidHostileTarget(pVictim) && me->IsWithinDist(pVictim, 50.0f))
                return pVictim;
    }

    return nullptr;
}

// Step somewhere the target can actually be seen from.
//
// Cave instances are the whole reason this exists. Of about eleven hundred refused damage casts in
// one Wailing Caverns run, eight hundred and twenty six were SPELL_FAILED_LINE_OF_SIGHT: one
// warlock, holding for a pull twenty nine yards from a Druid of the Fang with a wall between them,
// spent an entire fight firing eight failed casts a second and contributed nothing whatsoever. The
// rotation had no idea anything was wrong, because a refused cast simply falls through to the next
// spell in the list and then round again.
//
// Asked about the bot's own target once a tick rather than inferred from refused casts, which is
// how this started and could not work: CanTryToCastSpell now declines a cast at something it
// cannot see, so the refusals that would have been counted never happen. One line of sight test a
// tick is also cheaper than the several the rotation used to buy inside CheckCast for nothing.
// How long a mob is going to be unable to come after us, in milliseconds. Zero when it can.
//
// Root, stun and the incapacitating effects all end the same way as far as this decision goes: the
// mob is standing still and cannot follow. Fear is deliberately absent - a feared mob is running
// away on its own and there is nothing to walk out of.
static uint32 GetHeldInPlaceDurationMs(Unit const* pEnemy)
{
    static AuraType const holdingAuraTypes[] =
    {
        SPELL_AURA_MOD_ROOT,
        SPELL_AURA_MOD_STUN,
        SPELL_AURA_MOD_CONFUSE,
    };

    uint32 longest = 0;

    for (AuraType type : holdingAuraTypes)
    {
        for (Aura* pAura : pEnemy->GetAurasByType(type))
        {
            if (!pAura)
                continue;

            int32 const duration = pAura->GetAuraDuration();

            // A permanent hold is the strongest case there is, not the weakest.
            if (duration < 0)
                return PB_HELD_STEP_MIN_REMAINING_MS * 10;

            longest = std::max(longest, uint32(duration));
        }
    }

    return longest;
}

// Walk out of the reach of something that has been rooted or stunned while beating on us.
//
// The gap this fills is the one that looks stupidest from outside. A player sees the priest being
// chewed on, freezes the mob in place to save them, and the priest carries on standing inside its
// swing radius taking every hit - because nothing in the rotation ever asked whether the thing
// hitting it could still follow. A rooted mob adjacent to a caster is free damage to walk away
// from, and the only reason not to is that the walk itself might wake something else, which
// RunAwayFromTarget already refuses to do.
//
// Melee roles are excluded: being inside that radius is their job, and a rooted target is the best
// thing that can happen to them.
// Go there if the way is clear, go most of the way round if it is not, and only give up if there
// is no way round at all.
//
// The refusals this replaces were all of the form "the route is not safe, so stay put", which is
// half a decision: correct about the route and useless about the goal. In a corridor it is also
// permanent, because standing still does not change what the direct line crosses, which is the
// freezing that looks like the bot has stopped working.
bool PartyBotAI::SafeMoveTo(float x, float y, float z)
{
    // Before reachability, because a destination off the held ground is refused whether or not
    // there is a path to it, and asking the pathfinder first would be paying for a mesh query to
    // answer a question already settled.
    if (WouldLeaveHeldGround(x, y, z))
        return false;

    // Reachable before safe, because an unreachable destination is not made acceptable by having
    // nothing hostile near it. MovePoint answers an off-mesh point with a straight line rather
    // than a refusal, so without this the bot walks into whatever is between it and the spot and
    // the caller re-issues the same destination every tick.
    if (!CanWalkTo(x, y, z))
    {
        float detourX, detourY, detourZ;
        if (!FindSafeDetour(x, y, z, detourX, detourY, detourZ))
            return false;

        me->GetMotionMaster()->MovePoint(0, detourX, detourY, detourZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }

    if (!WouldPathPullExtraEnemies(x, y, z))
    {
        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
        return true;
    }

    float detourX, detourY, detourZ;
    if (!FindSafeDetour(x, y, z, detourX, detourY, detourZ))
        return false;

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] detour bot='%s' role=%s went wide to (%.1f %.1f) rather than "
                 "straight at (%.1f %.1f)",
                 me->GetName(), GetRoleName(GetRole()), detourX, detourY, x, y);
    }

    me->GetMotionMaster()->MovePoint(0, detourX, detourY, detourZ, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return true;
}

// Back a fight away from whatever is standing next to it.
//
// The tank's job includes where the fight happens, and nothing here ever treated it as a decision.
// A mob pulled from the edge of a camp stays at the edge of that camp for the whole fight, so every
// bot that has to reposition during it - a rogue going for the rear, a healer stepping into range,
// a caster backing out of melee - is doing so in a space bounded by a pack that is one stray step
// from joining in. Moving the tank a few yards back towards the group moves all of that at once,
// because everything on the tank follows the tank.
//
// Only the tank, only with aggro, and only when there is somewhere better: dragging a mob is how a
// tank loses it if the drag goes further than the leash.
// Get back behind the target once it is safe to be there.
//
// Standing in front is a fallback, taken only when the spot behind the target sits inside
// something else's aggro radius, and it is an expensive one: it gives up Backstab, both stealth
// openers, and the rule that nothing parries or blocks what it cannot see. It has to end when the
// reason for it does.
//
// It would not have. BeginChasing is called only when the movement generator has gone idle, and a
// chase does not go idle while its target lives, so the angle chosen in the first second of a
// fight was the angle for all of it - a rogue driven in front by a camp stayed in front for the
// rest of the fight even after the tank had dragged the mob well clear of it.
void PartyBotAI::ReconsiderMeleeChaseAngle()
{
    if (m_role != ROLE_MELEE_DPS || !me->IsInCombat() || m_holdPosition)
        return;

    Unit* pVictim = me->GetVictim();
    if (!pVictim)
        return;

    // Only while actually chasing. A point move or a hold is somebody else's decision.
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
        return;

    time_t const now = time(nullptr);
    if (now - m_lastFacingCheck < PB_MELEE_FACING_INTERVAL)
        return;

    m_lastFacingCheck = now;

    float x, y, z;
    pVictim->GetNearPoint(me, x, y, z, 0, pVictim->GetObjectBoundingRadius() + 1.0f,
                          pVictim->GetOrientation() + M_PI_F);

    // Asymmetric on purpose. Leaving the rear is judged at the ordinary margin, because the cost of
    // staying is a second pack. Returning to it asks for more clearance than that, so a spot that
    // only just qualified as unsafe does not immediately qualify as safe again.
    bool const rearUnsafe = m_chasingInFront
        ? WouldPositionPullExtraEnemies(x, y, z, PB_MELEE_REAR_RETURN_MARGIN)
        : WouldPositionPullExtraEnemies(x, y, z);

    // Already where it ought to be.
    if (rearUnsafe == m_chasingInFront)
    {
        m_facingChangeStreak = 0;
        return;
    }

    // And it has to still want the move next time it is asked.
    //
    // The rear point is taken from the target's live orientation, and a tanked mob turns - towards
    // a taunt, towards whoever peeled it, or just as it is dragged. So each check is asking about a
    // different piece of ground, and near the edge of a camp the answer alternated. One capture has
    // the rogue flipping front to rear and back ten times, every one of them three or six seconds
    // apart, which is this check running twice: on screen a rogue that walks out of the fight,
    // turns round, and walks back into it.
    m_facingChangeStreak = m_facingChangeStreak + 1;
    if (m_facingChangeStreak < PB_MELEE_FACING_CONFIRMATIONS)
        return;

    // Through the same gate as every other mid-fight reposition. Refacing did not use it, so it
    // could re-issue a chase over a move that had not finished - the contention this gate exists
    // to prevent, in the one system that was left outside it.
    if (!CanIssueCombatMovement())
        return;

    m_facingChangeStreak = 0;

    BeginChasing(pVictim);
    NoteCombatMovement();

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] reface bot='%s' moved to the %s of '%s': the rear is now %s",
                 me->GetName(), rearUnsafe ? "front" : "rear", pVictim->GetName(),
                 rearUnsafe ? "unsafe" : "clear");
    }
}

// Whether anything is allowed to reposition this bot right now.
//
// Three systems can move a warrior during a fight: the chase generator following its victim,
// backing the fight off a neighbouring camp, and collecting loose adds. Each was sensible alone
// and they had nothing in common, so the failure mode when they disagreed was a bot walking two
// yards, re-deciding, and walking back - which from outside is a tank stuttering instead of
// fighting. One clock and one rule: a point move already under way is left to finish, and nothing
// else issues one until it has.
// Where this bot trails the leader, and how far off to the side.
//
// Everyone used to roll urand(3,6) yards at a random angle inside a narrow rear cone on every call,
// which has two faults: two bots can roll the same spot, and re-rolling on every reposition means
// the group never holds a shape. A slot derived from the bot's own guid is stable for the life of
// the bot and different from its neighbours' without anybody having to agree on anything.
//
// The rear arc is kept. Its reason has not changed: a bot abreast of or ahead of whoever is
// steering is a second aggro radius dragged along the wall, and in a corridor that pulls exactly
// what the leader was walking around. Spreading out happens across the rear, not around it.
void PartyBotAI::GetFormationSlot(float& distance, float& angle) const
{
    switch (m_role)
    {
        case ROLE_HEALER:
            distance = PB_FORMATION_HEALER_DIST;
            break;
        case ROLE_RANGE_DPS:
            distance = PB_FORMATION_RANGED_DIST;
            break;
        default:
            distance = PB_FORMATION_MELEE_DIST;
            break;
    }

    if (me->IsInCombat())
        distance = distance + PB_FORMATION_COMBAT_BONUS;

    // Two bits of the guid give four lanes across the rear, and a third staggers the depth so two
    // bots sharing a lane are not standing on each other either.
    uint32 const seed = me->GetGUIDLow();
    uint32 const lane = seed % 4;
    bool const deeper = ((seed / 4) % 2) == 1;

    // Casters and healers take the outer lanes, melee the inner ones, so the back line is not
    // standing in the lane the melee walk down to reach the fight.
    bool const outer = (m_role == ROLE_HEALER || m_role == ROLE_RANGE_DPS);
    float const lanes[4] = { 0.35f, -0.35f, PB_FORMATION_MAX_OFFSET, -PB_FORMATION_MAX_OFFSET };
    float offset = outer ? lanes[(lane % 2) + 2] : lanes[lane % 2];

    // The two remaining lanes for whoever is left over, so a party of four melee still fans out.
    if (!outer && lane >= 2)
        offset = offset * 2.0f;

    if (offset > PB_FORMATION_MAX_OFFSET)
        offset = PB_FORMATION_MAX_OFFSET;
    if (offset < -PB_FORMATION_MAX_OFFSET)
        offset = -PB_FORMATION_MAX_OFFSET;

    if (deeper)
        distance = distance + 2.0f;

    angle = M_PI_F + offset;
}

// The same slot, tightened until the ground it names is ground nothing objects to.
//
// Spreading out is only free while it does not wake anything, which is the one condition attached
// to it. A slot that would put a bot inside an unengaged creature's aggro radius is pulled in
// towards directly-behind and shortened, and if three tries do not find room it settles for the
// old tucked-in position rather than insisting.
//
// The candidate position is worked out with trigonometry from the leader's coordinates rather than
// by asking the leader for a point near itself. GetNearPoint runs a grid visit on the object it is
// called on, and calling it on somebody else means touching their map: with thirty nine bots each
// asking up to four times per decision, one of them did it while the leader was mid-teleport and
// the server took a SIGSEGV. It was also four grid searches per bot per reposition to answer a
// question about a position, which trigonometry answers for nothing. The leader is checked for
// being somewhere askable at all before any of it.
void PartyBotAI::GetSafeFormationSlot(Player const* pLeader, float& distance, float& angle) const
{
    GetFormationSlot(distance, angle);

    if (!pLeader || !pLeader->IsInWorld() || pLeader->IsBeingTeleported() ||
        pLeader->GetMap() != me->GetMap())
    {
        angle = M_PI_F;
        distance = PB_MIN_FOLLOW_DIST;
        return;
    }

    float const wanted = angle - M_PI_F;
    float const leaderX = pLeader->GetPositionX();
    float const leaderY = pLeader->GetPositionY();
    float const leaderZ = pLeader->GetPositionZ();

    for (uint32 attempt = 0; attempt <= PB_FORMATION_TIGHTEN_TRIES; ++attempt)
    {
        float const scale = 1.0f - (PB_FORMATION_TIGHTEN_STEP * attempt);
        float const tryAngle = M_PI_F + (wanted * scale);
        float const tryDistance = distance * scale;
        float const absAngle = pLeader->GetOrientation() + tryAngle;

        float const x = leaderX + tryDistance * cos(absAngle);
        float const y = leaderY + tryDistance * sin(absAngle);

        // The leader's own height. An aggro radius is a distance and a few feet of slope does not
        // change the answer, so this does not need the ground fixed up under it.
        if (!WouldPositionPullExtraEnemies(x, y, leaderZ))
        {
            angle = tryAngle;
            distance = tryDistance;
            return;
        }
    }

    // Nowhere spread out is safe, so stand where the group always used to.
    angle = M_PI_F;
    distance = PB_MIN_FOLLOW_DIST;
}

bool PartyBotAI::CanIssueCombatMovement() const
{
    if (me->IsMoving() &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return false;

    return (time(nullptr) - m_lastCombatMove) >= PB_COMBAT_MOVE_INTERVAL;
}

void PartyBotAI::NoteCombatMovement()
{
    m_lastCombatMove = time(nullptr);
}

// Everything currently hitting somebody who cannot take a hit.
//
// A loose enemy is one whose victim is a group member built to stand at range: a healer or a
// caster. Whatever is on the tank is by definition not loose, and whatever is on another melee is
// a fair trade rather than an emergency, so neither counts.
void PartyBotAI::CollectLooseEnemies(std::vector<Unit*>& out) const
{
    out.clear();

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;

        if (pMember->GetMap() != me->GetMap())
            continue;

        CombatBotRoles const role = GetEffectiveRole(pMember);
        if (role != ROLE_HEALER && role != ROLE_RANGE_DPS)
            continue;

        for (Unit* pAttacker : pMember->GetAttackers())
        {
            if (!pAttacker || !pAttacker->IsAlive() || pAttacker->GetVictim() == me)
                continue;

            if (!IsValidHostileTarget(pAttacker))
                continue;

            if (!me->IsWithinDist(pAttacker, PB_GATHER_SEARCH_RANGE))
                continue;

            // Not worth crossing a room for, and not worth a global cooldown either.
            if (Creature const* pCreature = pAttacker->ToCreature())
                if (pCreature->GetCreatureType() == CREATURE_TYPE_CRITTER)
                    continue;

            // And not what the dungeon table says to leave alone. This is the chokepoint for the
            // whole peel: what is not collected here is not fetched, not shouted at and not
            // switched to, so one filter covers all three.
            if (IsIgnoredByParty(pAttacker))
                continue;

            if ((pAttacker->GetLevel() + PB_PEEL_LEVEL_FLOOR) < me->GetLevel())
                continue;

            if (std::find(out.begin(), out.end(), pAttacker) == out.end())
                out.push_back(pAttacker);
        }
    }
}

// Where the collected adds are meant to end up.
//
// Read live from the group every tick rather than stamped once. The first version took the
// warrior's own position at the moment it entered combat and held it for the rest of the fight,
// on the reasoning that this is where the group engaged. In a dungeon that reasoning is wrong in
// the one way that matters: combat does not drop between pulls. A group chain-pulling through
// Wailing Caverns stays in combat for minutes at a time, so the anchor stayed pinned to the first
// pull of the chain while the group walked on, and the warrior ran further and further backwards
// to reach it - 16y, then 41y, then 53y in one capture, growing exactly as fast as the group
// advanced. The same stale point was also measuring which loose adds were worth fetching, so by
// then every real add read as too far from the anchor and got skipped. The warrior stopped
// collecting anything and only kept running the wrong way.
//
// The tank is the anchor for a damage warrior, because the tank is the pile: an add dragged
// anywhere else is an add the tank then has to go and fetch. A tank has no such anchor of its own
// - it is standing on the pile already - so it falls through to the healer, which bounds how far
// it will chase a loose add without ever asking it to walk backwards. Both are real standable
// points rather than a computed average that can land inside a wall.
bool PartyBotAI::GetGatherAnchor(float& x, float& y, float& z) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    if (m_role != ROLE_TANK)
    {
        if (Player* pTank = GetGroupTank())
        {
            if (me->GetDistance(pTank) <= PB_GATHER_ANCHOR_MAX_RANGE)
            {
                x = pTank->GetPositionX();
                y = pTank->GetPositionY();
                z = pTank->GetPositionZ();
                return true;
            }
        }
    }

    Player* pAnchor = nullptr;
    float bestDistance = 0.0f;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;

        if (pMember->GetMap() != me->GetMap())
            continue;

        CombatBotRoles const role = GetEffectiveRole(pMember);
        if (role != ROLE_HEALER && role != ROLE_RANGE_DPS)
            continue;

        // Far enough away to be a straggler or a body in another room rather than the group's
        // back line, and dragging a pack of adds to one is worse than not collecting at all.
        float const distance = me->GetDistance(pMember);
        if (distance > PB_GATHER_ANCHOR_MAX_RANGE)
            continue;

        // The healer outright, otherwise the nearest of the casters.
        bool const preferred = (role == ROLE_HEALER);
        bool const currentIsHealer = pAnchor && GetEffectiveRole(pAnchor) == ROLE_HEALER;

        if (!pAnchor || (preferred && !currentIsHealer) ||
            (preferred == currentIsHealer && distance < bestDistance))
        {
            pAnchor = pMember;
            bestDistance = distance;
        }
    }

    if (!pAnchor)
        return false;

    x = pAnchor->GetPositionX();
    y = pAnchor->GetPositionY();
    z = pAnchor->GetPositionZ();
    return true;
}

// Collect what is loose and bring it back to where the group is standing.
//
// This is what a warrior in a competent group spends a fight doing and what none of these bots did
// at all: everything loose stayed on whoever it had picked until a taunt happened to come off
// cooldown, and taunt is one target every ten seconds. Watching a hardcore group clear this
// instance, both warriors spend the Mutanus fight running the adds into a pile for the mage,
// which is a behaviour made of three ordinary parts and no encounter knowledge.
//
// Any warrior, not only the tank. A damage warrior cannot taunt - taunt is Defensive Stance and it
// is in Battle - but it can hit the thing, and threat from hitting it is enough to take a Deviate
// off a priest. Demoralizing Shout is the piece that makes it a group behaviour rather than a
// single peel: it is on no cooldown, works in any stance, reaches ten yards, and lands threat on
// everything in that radius at once.
// One warrior goes, not all of them.
//
// Nothing stopped two from picking the same add, and in a group with a tank and a damage warrior
// both routinely did: one capture has both walking to the same Deviate Creeper in the same second,
// then both to the same Deviate Coiler three seconds later. Two warriors on one add is one
// warrior's worth of peel and two warriors' worth of damage given up for it.
//
// Nearest wins, which needs no agreement between them - each asks the same question about the same
// positions and only one can be the answer. A warrior already peeling something else is not a
// candidate for this one, and neither is a tank unless the add is on the healer, because a tank
// declines everything else on its own: counting it as the nearest would leave the add on the caster
// with nobody coming for it.
bool PartyBotAI::ShouldThisWarriorPeel(Unit* pAdd, bool onHealer) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return true;

    ObjectGuid const addGuid = pAdd->GetObjectGuid();
    float const myDistance = me->GetDistance(pAdd);

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || !pMember->IsAlive())
            continue;

        if (pMember->GetMap() != me->GetMap() || pMember->GetClass() != CLASS_WARRIOR)
            continue;

        PlayerBotEntry const* pEntry = pMember->GetSession() ? pMember->GetSession()->GetBot() : nullptr;
        PartyBotAI const* pAI = pEntry ? dynamic_cast<PartyBotAI const*>(pEntry->ai.get()) : nullptr;
        if (!pAI)
            continue;

        // Already gone for it.
        if (pAI->m_gatherPeelTarget == addGuid)
            return false;

        if (!pAI->m_gatherPeelTarget.IsEmpty())
            continue;

        if (pAI->m_role == ROLE_TANK && !onHealer)
            continue;

        if (pMember->GetDistance(pAdd) < myDistance)
            return false;
    }

    return true;
}

bool PartyBotAI::GatherLooseEnemies()
{
    if (me->GetClass() != CLASS_WARRIOR)
        return false;

    if (!me->IsInCombat())
    {
        m_gatherPeelTarget.Clear();
        m_gatherReturnTarget.Clear();
        return false;
    }

    // Part-way through a peel, so finish it before considering anything else.
    //
    // Peeling is a detour, not a change of plan. Without this the switch was permanent: the warrior
    // took the add and simply never went back, which for a damage warrior means it stops focusing
    // whatever the group is killing, and for a tank in a boss fight means the boss is now loose
    // with the tank across the room holding an add.
    //
    // Aggro secured is the thing being waited for, not a fixed number of seconds, because that is
    // the actual job: one swing is usually enough and there is no reason to stand there for longer,
    // while an add that needs three should get three. The timeout below only covers the cases where
    // it never happens at all.
    if (!m_gatherPeelTarget.IsEmpty())
    {
        Unit* pAdd = me->GetMap()->GetUnit(m_gatherPeelTarget);
        bool const secured = !pAdd || !pAdd->IsAlive() || !IsValidHostileTarget(pAdd) ||
                             pAdd->GetVictim() == me;
        bool const expired = (time(nullptr) - m_gatherSwitchTime) >= PB_GATHER_PEEL_MAX_SECONDS;

        if (!secured && !expired)
            return false;

        Unit* pReturn = me->GetMap()->GetUnit(m_gatherReturnTarget);

        // An abandoned peel is remembered, a successful one is not. See
        // PB_GATHER_PEEL_RETRY_INTERVAL: the whole cost of this behaviour is the walk there and
        // the walk back, and paying it twice for the same add inside a second is the failure.
        if (!secured)
        {
            m_gatherGaveUpGuid = m_gatherPeelTarget;
            m_gatherGaveUpTime = time(nullptr);
        }

        m_gatherPeelTarget.Clear();
        m_gatherReturnTarget.Clear();

        if (pReturn && pReturn->IsAlive() && IsValidHostileTarget(pReturn) &&
            me->GetVictim() != pReturn)
        {
            AttackStart(pReturn);

            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] herd bot='%s' role=%s went back to '%s' after the peel (%s)",
                         me->GetName(), GetRoleName(m_role), pReturn->GetName(),
                         secured ? "aggro landed" : "gave up waiting");
            }

            return false;
        }

        // Nothing to go back to, so the ordinary target selection takes it from here.
        if (IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] herd bot='%s' role=%s finished the peel with nothing to go back to",
                     me->GetName(), GetRoleName(m_role));
        }
    }

    if (m_holdPosition || IsInDuel() || IsPulling() || me->IsNonMeleeSpellCasted())
        return false;

    if (me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING))
        return false;

    float anchorX, anchorY, anchorZ;
    if (!GetGatherAnchor(anchorX, anchorY, anchorZ))
        return false;

    std::vector<Unit*> loose;
    CollectLooseEnemies(loose);

    if (loose.empty())
    {
        // Nothing left to fetch. Walk whatever is already following back to the anchor so it piles
        // up there rather than wherever the last add happened to be standing, which is the half of
        // this that turns a peel into a pull. Only worth doing while something is actually in tow.
        //
        // And the mob the group is already fighting is not in tow. It used to count, because the
        // question asked was only whether anything at all was hitting this bot, and the answer is
        // yes for every melee in every fight. So a warrior stood in the boss, found itself more
        // than twelve yards from the anchor, and walked back -- with the boss, which the tank
        // already had, and which followed the tank rather than the warrior. Measured against
        // Arugal, who teleports every thirteen seconds and so puts the whole group out of position
        // on a timer: two warriors did this two hundred and six times in one session, median walk
        // fourteen yards, and spent over half the fight beyond ten yards of the boss they were
        // supposed to be hitting. The rogue in the same party never moved, which is what made it
        // look like a movement bug rather than this.
        //
        // And with no tank there is nowhere to walk to. GetGatherAnchor falls back to a healer or
        // a ranged member when nobody is tanking, so without this the rule ferried every melee's
        // attacker into the back line -- the exact opposite of what herding is for, and worse than
        // doing nothing. Measured on a tankless party of three melee and a healer: five walks in
        // sixty four seconds, every one of them dragging a mob onto the healer.
        //
        // Note GetGroupTank only recognises a bot in the tank role, so a human tanking the group
        // reads as no tank at all. That is the conservative way round: the walk is a convenience
        // and standing still is never the thing that loses a fight.
        Player* pTank = GetGroupTank();
        if (!pTank)
            return false;

        Unit const* pMainTarget = pTank->GetVictim();

        bool inTow = false;
        for (Unit* pAttacker : me->GetAttackers())
        {
            if (pAttacker != pMainTarget)
            {
                inTow = true;
                break;
            }
        }

        if (!inTow)
            return false;

        // Not the tank. The tank is where the pile belongs, so walking it towards the group's back
        // line drags the whole fight into the casters - the exact thing the rest of this file
        // spends its time preventing. Where a tank stands is DragFightAwayFromNeighbours' call.
        if (m_role == ROLE_TANK)
            return false;

        float const strayed = me->GetDistance(anchorX, anchorY, anchorZ);
        if (strayed < PB_GATHER_RETURN_DISTANCE)
            return false;

        if (!CanIssueCombatMovement())
            return false;

        if (!SafeMoveTo(anchorX, anchorY, anchorZ))
            return false;

        NoteCombatMovement();

        if (IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] herd bot='%s' walked %.1fy back to the anchor with %u in tow",
                     me->GetName(), strayed, uint32(me->GetAttackers().size()));
        }

        // Walking is not what this bot does instead of fighting. Reported as not having consumed
        // the tick so the rotation underneath still runs: a swing and a step happen at once in
        // this game, and returning true here cost a warrior its whole rotation on every tick it
        // repositioned.
        return false;
    }

    // Threat on everything within reach in one global cooldown. Ahead of taunting or chasing any
    // individual add because it is the only thing here that scales with the number of them, and
    // because on no cooldown there is nothing to save it for.
    if (m_spells.warrior.pDemoralizingShout)
    {
        // Everything in the radius, not just the loose ones: the shout lands on all of them and
        // the threat on each is worth having, so a fight with one loose add and three already on
        // this warrior is still worth shouting.
        //
        // And everything means everything. An area effect does not ask whether a creature is part
        // of the fight before hitting it, so a shout with an unengaged mob standing inside it is a
        // pull, not a peel - the one way this behaviour could turn into the exact disaster the
        // rest of the awareness work exists to prevent. One unengaged creature in radius and the
        // shout is off.
        // Scanned wider than the shout reaches. The radius is read from a DBC index rather than
        // written down here, so treating ten yards as exact is a guess, and the direction to be
        // wrong in is obvious: counting one enemy too few costs a shout, catching one unengaged
        // mob costs the group a second pack.
        std::list<Unit*> nearby;
        me->GetEnemyListInRadiusAround(me, PB_GATHER_SHOUT_SAFETY_RADIUS, nearby);

        uint32 inRadius = 0;
        bool wouldPull = false;

        for (Unit* pEnemy : nearby)
        {
            if (!pEnemy || !pEnemy->IsAlive() || !IsValidHostileTarget(pEnemy))
                continue;

            // Not yet in the fight, and inside the blast. That is a pull, not a peel.
            //
            // Except for what the fight itself put there. A totem never enters combat, so
            // IsEngagedWithGroup is permanently false for one and a boss that drops them at the
            // tank's feet switches this shout off for the whole encounter: Antu'sul plants an
            // Earthgrab Totem on an eleven second timer, and one attempt has the tank logging
            // "held its shout" roughly forty times across eighty seconds while landing two. There
            // is no second pack to be pulled by hitting a totem whose summoner is already
            // swinging at us, so the tactics table's exemption list -- the same one that lets a
            // bot walk up to one of these at all -- applies here too.
            if (!IsEngagedWithGroup(pEnemy) && !IsApproachAnywayTarget(pEnemy))
            {
                wouldPull = true;
                break;
            }

            if (!me->IsWithinDist(pEnemy, PB_GATHER_SHOUT_RADIUS))
                continue;

            // Only the ones it would actually be telling something new.
            //
            // The rotation's own use of this shout checks the debuff before recasting; this path
            // never did, so a tank holding a pack that was already shouted at re-shouted it every
            // couple of seconds for as long as the pack lived. Logged three times in four seconds
            // against the same targets -- thirty rage, which at ten rage a Sunder Armor is two
            // Sunders of threat the tank then did not have, in a fight it was losing on threat.
            //
            // The debuff lasts thirty seconds and the rage is the tank's whole threat budget, so
            // an enemy already carrying it is not worth counting towards the decision to spend it.
            if (m_spells.warrior.pDemoralizingShout &&
                pEnemy->HasAura(m_spells.warrior.pDemoralizingShout->Id))
                continue;

            ++inRadius;
        }

        if (wouldPull && IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] herd bot='%s' held its shout: something not in the fight is "
                     "standing inside the radius", me->GetName());
        }

        if (!wouldPull &&
            inRadius >= PB_GATHER_SHOUT_MIN_TARGETS &&
            CanTryToCastSpell(me, m_spells.warrior.pDemoralizingShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pDemoralizingShout) == SPELL_CAST_OK)
            {
                if (IsCombatLogged())
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] herd bot='%s' shouted %u loose enemies onto itself",
                             me->GetName(), inRadius);
                }

                return true;
            }
        }
    }

    // Otherwise go and get the nearest one. Bounded by how far it is from the anchor rather than
    // from the warrior, because the thing being protected against is a warrior that walks off
    // after an add and takes the fight with it.
    Unit* pNearest = nullptr;
    float bestDistance = 0.0f;

    time_t const now = time(nullptr);

    for (Unit* pLoose : loose)
    {
        if (pLoose->GetDistance(anchorX, anchorY, anchorZ) > PB_GATHER_MAX_STRAY)
            continue;

        // Not the one the last peel was given up on. See PB_GATHER_PEEL_RETRY_INTERVAL.
        if (pLoose->GetObjectGuid() == m_gatherGaveUpGuid &&
            (now - m_gatherGaveUpTime) < PB_GATHER_PEEL_RETRY_INTERVAL)
            continue;

        float const distance = me->GetDistance(pLoose);
        if (!pNearest || distance < bestDistance)
        {
            pNearest = pLoose;
            bestDistance = distance;
        }
    }

    if (!pNearest)
        return false;

    if (me->GetVictim() == pNearest)
        return false;

    // Only fetch what this warrior is actually going to hit.
    //
    // Walking and attacking used to be separate decisions here, and the pairing was wrong in both
    // directions. The walk was unconditional while the switch only ever happened for something on
    // the healer, so an add on a caster had the warrior walk the whole way over and then stand
    // there swinging at nothing, because its victim was still the mob it had left behind. Worse,
    // leaving the victim set meant the chase generator was still pulling it back towards that mob
    // while this was pushing it towards the add, and the two took turns: one capture has a warrior
    // at 1.0y from its target, then 4.0y, then 11.6y, then 2.5y, then 10.6y, dealing no damage
    // through any of it. That oscillation is what reads on screen as a warrior turning and running
    // away mid-fight.
    //
    // So the walk is gone. Deciding to fetch something now means attacking it, and the chase that
    // already exists does the walking - one movement system with one opinion about where to stand.
    Player* pHealer = FindGroupHealer();
    bool const onHealer = pHealer && pNearest->GetVictim() == pHealer;

    if (!ShouldThisWarriorPeel(pNearest, onHealer))
        return false;

    if (Unit* pVictim = me->GetVictim())
    {
        // A tank taking an add by switching to it drops the Sunder stack it has been building and
        // gains nothing a taunt would not have given it. Taunt is what a tank peels with, and the
        // peel logic spends it; the only thing worth breaking that rule for is the healer.
        //
        // And even for the healer, only when there is no taunt to spend. A taunt lands from where
        // the tank is standing; a body peel is a walk out and a walk back, and it cannot work at
        // all on an add that never closes -- which is precisely the add that ends up on a healer,
        // because the ones that close are already on the tank. So the body peel waited out its
        // full timeout on a Sandfury Shadowcaster shooting the healer from range, walked the tank
        // back, and was re-taken on arrival: the tank covered twenty three yards in eight seconds
        // and neither mob changed target. PeelForTheHealer runs ahead of this now, so reaching
        // here with a taunt ready means the taunt was refused for the target rather than missing.
        if (m_role == ROLE_TANK && (!onHealer || HasTauntReadyFor(pNearest)))
            return false;

        // Nearly dead. Finishing it is a few more swings and one less mob in the fight, and the
        // add will still be there.
        if (pVictim->GetHealthPercent() < PB_GATHER_FINISH_PERCENT && !onHealer)
            return false;

        // Something on the healer is worth interrupting anything for. Everything else waits its
        // turn, so that collecting cannot become a warrior that changes target every tick.
        if (!onHealer && (now - m_lastGatherSwitch) < PB_GATHER_SWITCH_INTERVAL)
            return false;
    }

    Unit* const pLeaving = me->GetVictim();

    if (!AttackStart(pNearest))
        return false;

    m_lastGatherSwitch = time(nullptr);
    m_gatherSwitchTime = m_lastGatherSwitch;
    m_gatherPeelTarget = pNearest->GetObjectGuid();
    m_gatherReturnTarget = pLeaving ? pLeaving->GetObjectGuid() : ObjectGuid();

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] herd bot='%s' role=%s took '%s' off %s from %.1fy away",
                 me->GetName(), GetRoleName(m_role), pNearest->GetName(),
                 onHealer ? "the healer" : "a caster", bestDistance);
    }

    return true;
}

// Get out of the way of something nobody has pulled.
//
// Every other aggro rule in this file is a refusal, and a refusal only ever answers the bot moving
// towards trouble. It has nothing at all to say about trouble arriving: a patrol walks up to a
// group eating between pulls, a leader parks the party in a doorway, a pack respawns around bots
// that killed it. In each case the bot is inside an aggro radius, no rule objects because nothing
// asked one, and whether the group gets a second fight is now up to the creature's own timer.
//
// So this is the one rule here that moves a bot for a reason outside the fight it is in.
//
// Melee in combat are deliberately excluded, and the exclusion is the important part. A melee bot
// cannot be both out of a seventeen yard radius and inside its target's melee reach, so the only
// available step is one that abandons the fight -- and the chase generator would immediately walk
// it back in, which is a bot turning and running from its target for no visible reason. Where a
// melee fight happens is the tank's business and DragFightAwayFromNeighbours is how the tank says
// so: it moves the mob as well, which is the only version of this that works in melee.
bool PartyBotAI::AvoidUnpulledNeighbours()
{
    if (IsInDuel() || m_holdPosition || IsPulling() || me->HasAttackOrders())
        return false;

    // Mounted means travelling, which is the follow generator's business and already subject to
    // the route rule; and a cast in progress is worth more than the step.
    if (me->IsMounted() || me->IsNonMeleeSpellCasted())
        return false;

    if (me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING))
        return false;

    bool const inCombat = me->IsInCombat();
    if (inCombat && GetRole() != ROLE_RANGE_DPS)
        return false;

    time_t const now = time(nullptr);
    if (now - m_lastNeighbourStep < PB_NEIGHBOUR_STEP_INTERVAL)
        return false;

    if (!CanIssueCombatMovement())
        return false;

    Creature* const pNeighbour = me->FindUnengagedCreatureAggroedByPosition(
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
        PB_NEIGHBOUR_STEP_MARGIN, GetApproachAnywayEntries());
    if (!pNeighbour)
        return false;

    float x, y, z;
    bool found = false;

    if (inCombat)
    {
        // A step that gives up the fight is not a step worth taking, so in combat this asks for a
        // firing position rather than for open ground: in range of what the bot is shooting, with
        // a view of it, and - because FindFiringPosition already refuses anything inside an
        // unengaged aggro radius - out of the band that prompted the search.
        //
        // Capped at the range the bot is already at, so this is sideways and never a retreat out
        // of range. Same reasoning as the blind-corner step, and the same effect when nothing
        // sideways is clear: the bot stays where it is rather than walking out of the fight.
        Unit* const pVictim = me->GetVictim();
        if (!pVictim)
            return false;

        float const distance = me->GetDistance(pVictim);
        float const standoff = std::max(GetTacticalStandoff(pVictim), PB_BLIND_STEP_MIN_DISTANCE);
        if (distance <= standoff)
            return false;

        found = FindFiringPosition(pVictim, standoff, distance, PB_NEIGHBOUR_STEP_MAX_TRAVEL,
                                   x, y, z);
    }
    else
    {
        found = FindSpotClearOfUnengaged(pNeighbour, PB_NEIGHBOUR_STEP_MAX_TRAVEL, x, y, z);
    }

    if (!found)
        return false;

    if (!me->IsStopped())
        me->StopMoving();

    me->GetMotionMaster()->Clear(false, true);
    // Idle before the point order, for the reason every other reposition here does it: a movement
    // stack emptied and then left empty is what killed the world thread once already.
    me->GetMotionMaster()->MoveIdle();
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);

    m_lastNeighbourStep = now;
    NoteCombatMovement();

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] neighbour bot='%s' role=%s stepped %.1fy to (%.1f %.1f) out of '%s' "
                 "(lvl %u, aggro %.1fy) which it was standing %.1fy from and nobody has pulled",
                 me->GetName(), GetRoleName(GetRole()), me->GetDistance(x, y, z), x, y,
                 pNeighbour->GetName(), pNeighbour->GetLevel(),
                 pNeighbour->GetAttackDistance(me), me->GetDistance(pNeighbour));
    }

    return true;
}

bool PartyBotAI::DragFightAwayFromNeighbours()
{
    if (m_role != ROLE_TANK || !me->IsInCombat() || m_holdPosition || IsInDuel())
        return false;

    if (me->IsNonMeleeSpellCasted() || IsPulling())
        return false;

    if (!CanIssueCombatMovement())
        return false;

    Unit* pVictim = me->GetVictim();
    if (!pVictim || !me->CanReachWithMeleeAutoAttack(pVictim))
        return false;

    // Nothing to get away from.
    Creature* const pNeighbour = me->FindUnengagedCreatureAggroedByPosition(
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), PB_DRAG_BACK_MARGIN);
    if (!pNeighbour)
        return false;

    Player* pLeader = GetPartyLeader();
    if (!pLeader || pLeader->GetMapId() != me->GetMapId())
        return false;

    // Back towards the group, not away from the camp: away from one camp is towards whatever else
    // is out there, and the group's own position is the one place known to be clear, because the
    // group is standing in it.
    float const distanceToLeader = me->GetDistance(pLeader);
    if (distanceToLeader < PB_DRAG_BACK_MIN_GAP || distanceToLeader > PB_DRAG_BACK_MAX_GAP)
        return false;

    float x, y, z;
    me->GetNearPoint(me, x, y, z, 0, PB_DRAG_BACK_STEP, me->GetAngle(pLeader));

    if (WouldPathPullExtraEnemies(x, y, z))
        return false;

    // And it has to actually be an improvement.
    if (me->FindUnengagedCreatureAggroedByPosition(x, y, z, PB_DRAG_BACK_MARGIN))
        return false;

    NoteCombatMovement();
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);

    if (IsCombatLogged())
    {
        // A drag validates its destination as clear before it moves, so one step ought to settle
        // it. Captures show the same mob dragged five times in half a minute, which means the
        // situation is being recreated between drags. The field that answers why is driftback:
        // how far the tank now stands from where the previous drag was supposed to leave it. Near
        // zero means the step did not buy enough clearance and the neighbour is simply still in
        // range; near the step length means something walked the tank back again, and the only
        // candidate is the chase generator following the victim to its old ground.
        float driftBack = -1.0f;
        if (m_lastDragTime)
            driftBack = me->GetDistance(m_lastDragX, m_lastDragY, m_lastDragZ);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] dragback bot='%s' pulled '%s' %.1fy back towards the group to clear "
                 "'%s' (lvl %u, aggro %.1fy, %.1fy away) from %.1f %.1f %.1f to %.1f %.1f %.1f, "
                 "vdist=%.1f driftback=%.1f since=%ld",
                 me->GetName(), pVictim->GetName(), PB_DRAG_BACK_STEP,
                 pNeighbour->GetName(), pNeighbour->GetLevel(),
                 pNeighbour->GetAttackDistance(me), me->GetDistance(pNeighbour),
                 me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), x, y, z,
                 me->GetDistance(pVictim), driftBack,
                 m_lastDragTime ? long(time(nullptr) - m_lastDragTime) : -1L);
    }

    m_lastDragX = x;
    m_lastDragY = y;
    m_lastDragZ = z;
    m_lastDragTime = time(nullptr);

    return true;
}

// Back a caster or healer out of a melee range it never chose to be in.
//
// The standoff machinery in BeginChasing cannot do this, and its own comment claiming otherwise
// was wrong. It works by handing a distance to the chase generator, and the generator applies it
// through PathInfo::UpdateForCaster, which walks forward along the path and truncates it at the
// first point inside cast range. That only ever stops a caster short on its way in. A caster
// already inside the range is answered by the function's first branch, which clears the path
// entirely and reports success: stay exactly where you are.
//
// Which is fine in a five man, because the mob walks to the tank and the caster is behind the
// tank. In a forty man the mob walks through the raid to reach the tank, and every caster and
// healer it passes is left standing in its melee arc for the rest of the fight. The capture that
// prompted this has a warlock at vdist=1.9 with melee=1 casting Shadow Bolt into a level sixty
// two giant, and fourteen of the raid's nineteen casters and rogues dead by the end of one trash
// pull that nobody should have died on.
//
// Deliberately not conditional on being attacked. StepAwayFromHeldAttacker already covers a bot
// that is being hit and rooted, and waiting to be hit is waiting too long: what kills a clothed
// character at this range is the cleave that is not aimed at it.
Unit* PartyBotAI::FindCastToBreakSightFrom() const
{
    if (!m_tactics)
        return nullptr;

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, PB_BREAK_SIGHT_SCAN, enemies);

    for (Unit* pEnemy : enemies)
    {
        if (!pEnemy || !pEnemy->IsAlive() || !pEnemy->IsCreature())
            continue;

        uint32 const spellId = m_tactics->GetBreakSightSpell(pEnemy->GetEntry());
        if (!spellId)
            continue;

        Spell* pSpell = pEnemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!pSpell || pSpell->getState() != SPELL_STATE_PREPARING)
            continue;

        if (!pSpell->m_spellInfo || pSpell->m_spellInfo->Id != spellId)
            continue;

        // Aimed at this bot and not at whoever is stood next to it. One bot walking away cannot
        // take the spell off anybody else, so a bot that is not the target has a position to lose
        // here and nothing to win.
        if (pSpell->m_targets.getUnitTargetGuid() != me->GetObjectGuid())
            continue;

        // Already behind something. The cast is going to fail where the bot stands, and moving now
        // could only walk it back into sight.
        if (!pEnemy->IsWithinLOSInMap(me))
            continue;

        if (pSpell->GetCastedTime() < PB_BREAK_SIGHT_MIN_WINDOW_MS)
            continue;

        return pEnemy;
    }

    return nullptr;
}

// The patch of hostile ground this bot is standing in.
//
// A persistent area aura is the one hazard in a dungeon that is invisible to everything else in
// this file. It is a DynamicObject sitting on the floor with a radius and a periodic effect, and
// from the bot's side there is no attacker, no threat entry, no cast to interrupt and nothing for
// a positioning rule written against a creature to hold on to. Health simply goes down.
//
// Maraudon is what made that matter. Noxious Cloud is five yards across and deals a hundred and
// fifty nature damage a second for twenty seconds -- three thousand health against a level forty
// seven caster's sixteen hundred -- and Noxious Slime casts it as it dies, which is to say
// directly underneath whoever just killed it.
//
// Named by the instance table rather than inferred, because the shape alone does not tell the two
// cases apart: a damaging area aura on the floor is also the party's own Blizzard, every
// Consecration and every totem pulse, and a bot that walks out of its own mage's ground effect is
// worse than one that stands in a cloud.
DynamicObject* PartyBotAI::FindGroundHazardUnderfoot() const
{
    if (!m_tactics || m_tactics->groundHazardSpellIds.empty())
        return nullptr;

    std::list<WorldObject*> found;
    MaNGOS::AllWorldObjectsInRange check(me, PB_GROUND_HAZARD_SCAN);
    MaNGOS::WorldObjectListSearcher<MaNGOS::AllWorldObjectsInRange> searcher(found, check);
    Cell::VisitAllObjects(me, searcher, PB_GROUND_HAZARD_SCAN);

    DungeonTactics const* pTactics = m_tactics;

    DynamicObject* pWorst = nullptr;
    float worstDepth = 0.0f;

    for (WorldObject* pObject : found)
    {
        if (!pObject || pObject->GetTypeId() != TYPEID_DYNAMICOBJECT)
            continue;

        DynamicObject* pDynObj = static_cast<DynamicObject*>(pObject);
        if (!pTactics->IsGroundHazard(pDynObj->GetSpellId()))
            continue;

        // Only what would actually hurt this bot. The same spell id can belong to a patch laid
        // down by something friendly in principle, and the object knows which it is.
        if (!pDynObj->IsHostileTo(me))
            continue;

        float const radius = pDynObj->GetRadius();
        if (radius <= 0.0f)
            continue;

        float const dx = me->GetPositionX() - pDynObj->GetPositionX();
        float const dy = me->GetPositionY() - pDynObj->GetPositionY();
        float const distance = sqrt(dx * dx + dy * dy);
        if (distance > radius)
            continue;

        // Deepest first when there are two, which is the case that happens in a Creeping Sludge
        // pack: the one the bot is furthest inside is the one the shortest step is measured
        // against, and stepping out of the shallow one would usually leave it in the other.
        float const depth = radius - distance;
        if (!pWorst || depth > worstDepth)
        {
            pWorst = pDynObj;
            worstDepth = depth;
        }
    }

    return pWorst;
}

// Step out of it.
//
// Deliberately unconditional on role, which is the opposite of what TakeCoverFromCast decides a
// few lines below and for a reason that does not carry over. A dodge trades melee uptime for a
// cast avoided, and for a melee bot that trade is a loss. This trades a couple of yards of
// walking for a hundred and fifty damage a second that nothing in the group can out-heal, and
// there is no role for which standing in it is the better answer.
//
// Reports true while the walk is under way so the rest of the tick is left alone, and ages the
// walk out rather than trusting it: a bot shoved or rooted halfway out has to be released back to
// fighting rather than left holding a point move that will never arrive.
bool PartyBotAI::StepOutOfGroundHazard()
{
    if (!m_tactics || m_tactics->groundHazardSpellIds.empty())
        return false;

    uint32 const now = WorldTimer::getMSTime();

    if (m_groundHazardSince &&
        WorldTimer::getMSTimeDiff(m_groundHazardSince, now) < PB_GROUND_HAZARD_HOLD_MS &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return true;

    m_groundHazardSince = 0;

    DynamicObject* pHazard = FindGroundHazardUnderfoot();
    if (!pHazard)
        return false;

    // Rooted, stunned or otherwise unable to walk, which in this instance is a real possibility
    // rather than a formality: Constrictor Vine's Entangling Roots and Barbed Lasher's Thorn
    // Volley are both in the packs the clouds are dropped in. Nothing to do about it, and saying
    // so rather than issuing a move that is silently discarded keeps the tick for the rotation.
    if (!me->IsAlive() || me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) ||
        me->IsTaxiFlying() || me->HasUnitState(UNIT_STATE_ROOT))
        return false;

    float const clear = pHazard->GetRadius() + PB_GROUND_HAZARD_MARGIN;

    float x, y, z;
    if (!FindSpotClearOfPoint(pHazard->GetPositionX(), pHazard->GetPositionY(), clear,
                              PB_GROUND_HAZARD_MAX_TRAVEL, x, y, z))
    {
        if (IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] cloudstuck bot='%s' is standing in spell %u and found nowhere "
                     "to step (radius %.1fy)",
                     me->GetName(), pHazard->GetSpellId(), pHazard->GetRadius());
        }
        return false;
    }

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] cloudstep bot='%s' role=%s stepped %.1fy out of spell %u",
                 me->GetName(), GetRoleName(m_role), me->GetDistance(x, y, z),
                 pHazard->GetSpellId());
    }

    me->InterruptNonMeleeSpells(false);
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    m_groundHazardSince = now;
    return true;
}

// Walk out of sight of a cast rather than eat it.
//
// This is what a player does to Archmage Arugal, and the reason it works is that the sight check on
// a spell is not made once when the cast starts. Spell::cast re-runs CheckCast when the cast
// completes, and the unit target branch of it refuses SPELL_FAILED_LINE_OF_SIGHT for any spell
// without SPELL_ATTR_EX2_IGNORE_LINE_OF_SIGHT. So a three second cast that started with the bot in
// the open fails outright if the bot is behind a wall three seconds later.
//
// Deliberately an individual dodge and not the party-wide version of the tactic. Only the bot the
// spell names moves, which keeps this out of the business of coordinating forty positions, and
// means at most one member of the group is ever out of position for it. The party-wide version --
// everybody stacked behind one wall while the tank holds the boss at the corner -- needs a place to
// stand that is a fact about the room, and nothing here knows the room.
//
// Ordered after InterruptHostileCasters in the tick on purpose. Taking the cast away is strictly
// better than dodging it, since it costs no position and no uptime, so a bot that can interrupt has
// already tried by the time this is reached.
bool PartyBotAI::TakeCoverFromCast()
{
    if (!m_tactics)
        return false;

    uint32 const now = WorldTimer::getMSTime();

    // A walk already under way is the whole of the tick: the point of it is to be somewhere else,
    // and standing still halfway to cover to cast something is how a bot ends up eating the spell
    // it set out to dodge.
    if (m_breakSightSince &&
        WorldTimer::getMSTimeDiff(m_breakSightSince, now) < PB_BREAK_SIGHT_HOLD_MS &&
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
        return true;

    m_breakSightSince = 0;

    // Ranged and healers only, which is the same line BackOutOfMeleeRange draws and for a harder
    // reason. Measured: a five man that let melee dodge wiped Arugal at a hundred percent health,
    // with every one of the four dodges taken by a melee bot. Arugal carries Void Bolt twice, on a
    // five to seven second timer while somebody is in melee and a one second timer while nobody is,
    // so a melee bot stepping out of melee to dodge one cast buys five more, each of which sends
    // somebody else out of melee. Nobody attacks and the group loses to a boss it out-damages.
    //
    // A bot that fights from thirty yards has no such trade to make: the dodge moves it within its
    // own range and costs it a cast, not its uptime.
    if (m_role != ROLE_RANGE_DPS && m_role != ROLE_HEALER)
        return false;

    if (m_holdPosition || IsInDuel())
        return false;

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) || me->IsTaxiFlying())
        return false;

    Unit* pCaster = FindCastToBreakSightFrom();
    if (!pCaster)
        return false;

    // A heal already in the air outranks the dodge. Going out of sight now loses the heal as well,
    // which leaves the bot worse off than if it had stood still and taken the hit.
    if (GetIncomingHeals(me) > 0)
        return false;

    // And below the floor the bot stops dodging altogether and stands where it can be healed. A
    // dodge costs nothing but position right up to the point where position is what the next heal
    // needs.
    if (me->GetHealthPercent() < PB_BREAK_SIGHT_MIN_HEALTH)
        return false;

    // A heal of its own is worth more than the hit. Anything else is not: moving cancels the cast,
    // and a nuke given up is cheaper than two hundred and fifty damage taken.
    if (Spell const* pOwnCast = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (pOwnCast->m_spellInfo && pOwnCast->m_spellInfo->IsHealSpell())
            return false;
    }

    Spell* pSpell = pCaster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!pSpell)
        return false;

    uint32 const remaining = pSpell->GetCastedTime();
    float const budget = float(remaining) / 1000.0f * PB_BREAK_SIGHT_TIME_BUDGET;
    float const reach = me->GetSpeed(MOVE_RUN) * budget;

    // Not enough cast left to get anywhere. Arriving as the spell lands would mean giving up the
    // position and taking the hit as well.
    if (reach < PB_BREAK_SIGHT_MIN_MOVE)
        return false;

    float x, y, z;
    if (!FindBreakSightSpot(pCaster, reach, x, y, z))
        return false;

    if (!me->IsStopped())
        me->StopMoving();

    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    NoteCombatMovement();

    // Zero is the "no walk" value, so a tick counter that happens to land on it borrows the next
    // millisecond rather than releasing the bot on the following tick.
    m_breakSightSince = now ? now : 1;

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] breaksight bot='%s' role=%s hp=%.0f%% took cover from '%s' casting "
                 "'%s' with %ums left, %.1fy to (%.1f %.1f)",
                 me->GetName(), GetRoleName(GetRole()), me->GetHealthPercent(),
                 pCaster->GetName(),
                 pSpell->m_spellInfo ? pSpell->m_spellInfo->SpellName[0].c_str() : "something",
                 remaining, me->GetDistance2d(x, y), x, y);
    }

    return true;
}

// Whether walking away from this enemy would actually get the bot away from it.
//
// For a priest with a mob on it the answer is almost always no, and that is the point. A creature
// chasing a player runs at least as fast as the player does, so a healer that turns and runs takes
// the same swings it was already taking, from behind, with nothing going out of its hands - and
// drags the mob across whatever else is in the room on the way, which is how backing out turns into
// the next pull. Standing still and casting while the tank peels is the better trade every time the
// gap cannot actually be opened.
//
// Not being attacked counts as escapable, and is the case this used to get wrong most often: a mob
// that merely stands close enough to reach the healer is not chasing anybody, so stepping out of its
// arc costs nothing and leaves it where the tank has it.
static constexpr float PB_ESCAPE_SPEED_MARGIN = 1.15f;

// Whether walking away from this enemy would actually open any distance. Only asked about an enemy
// already known to be attacking this bot, so there is no "it is not on me, so it is easy to escape"
// case here: that question is answered by the caller, and answered the other way.
static bool CanEscapeOnFoot(Unit const* pBot, Unit const* pEnemy)
{
    // Rooted, stunned or confused for long enough to be worth the walk. Same threshold the held
    // step uses, so the two paths cannot disagree about what counts as pinned.
    if (GetHeldInPlaceDurationMs(pEnemy) >= PB_HELD_STEP_MIN_REMAINING_MS)
        return true;

    // Running the other way on its own account.
    if (pEnemy->HasUnitState(UNIT_STATE_FLEEING))
        return true;

    // Or slowed enough that the distance will genuinely open. A margin rather than a plain
    // comparison because a rounding-error advantage buys a yard and costs a cast.
    return pBot->GetSpeed(MOVE_RUN) > pEnemy->GetSpeed(MOVE_RUN) * PB_ESCAPE_SPEED_MARGIN;
}

// Hold the distance the instance table asks for, rather than merely chasing to it.
//
// The standoff was inert before this and the measurement says so plainly. Princess Theradras is
// written down at twenty five yards, which is the radius of Dust Field plus a margin; across one
// measured fight her party's ranged and healer logged seventy four ticks at twelve to nineteen
// yards and eleven outside twenty. They spent the fight inside the thing the number exists to
// clear, and the group lost at sixty nine percent.
//
// The reason is the same one BackOutOfMeleeRange was written for, one band further out.
// BeginChasing applies the standoff by handing a distance to the chase generator, and the
// generator applies it through PathInfo::UpdateForCaster, which truncates a path on the way in
// and does nothing at all to a bot that is already closer than that. So the number bounds an
// approach and never a position. A bot that arrived inside the band -- knocked back into it,
// chased something into it, or simply pulled from there -- stays inside it for the whole fight.
//
// BackOutOfMeleeRange cannot cover this because it asks a different question: it looks for
// something that can reach this bot with a swing, and Princess Theradras standing fifteen yards
// away cannot. The danger at fifteen yards is not her reach, it is her radius, and only the table
// knows she has one.
//
// Deliberately narrow. Only creatures the group is actually fighting, only creatures the table
// names a standoff for, and only ranged bots and healers -- the same line BackOutOfMeleeRange
// draws, for the same reason: a melee bot outside the radius is a melee bot not attacking.
bool PartyBotAI::HoldTacticalStandoff()
{
    if (m_role != ROLE_RANGE_DPS && m_role != ROLE_HEALER)
        return false;

    if (m_holdPosition || IsInDuel() || !m_tactics)
        return false;

    if (!IsRangedDamageClass(me->GetClass()) ||
        IsAttackSpeedOverridenForm(me->GetShapeshiftForm()))
        return false;

    // Same exemption as the melee backout: a caster reduced to its wand is a melee character for
    // the rest of the fight, and walking it out of range of its only attack helps nobody.
    if (me->GetPowerPercent(POWER_MANA) <= 10.0f &&
       !me->GetWeaponForAttack(RANGED_ATTACK, true, true))
        return false;

    time_t const now = time(nullptr);
    if (now - m_lastStandoffWalk < PB_STANDOFF_INTERVAL)
        return false;

    // Whichever the bot is furthest inside, which is the one a single walk is least likely to
    // leave it still standing in.
    Unit* pTight = nullptr;
    float want = 0.0f;
    float deepest = 0.0f;

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, PB_STANDOFF_SCAN, enemies);

    for (Unit* pEnemy : enemies)
    {
        if (!pEnemy || !pEnemy->IsAlive() || !pEnemy->IsCreature())
            continue;

        float const standoff = GetTacticalStandoff(pEnemy);
        if (standoff <= 0.0f)
            continue;

        // Only what the group is already fighting. Walking away from something nobody has
        // pulled is how a retreat becomes the next pull.
        if (!IsEngagedWithGroup(pEnemy))
            continue;

        float const distance = me->GetDistance(pEnemy);
        if (distance >= standoff)
            continue;

        float const depth = standoff - distance;
        if (depth < PB_STANDOFF_DEADBAND)
            continue;

        if (!pTight || depth > deepest)
        {
            pTight = pEnemy;
            want = standoff;
            deepest = depth;
        }
    }

    if (!pTight)
        return false;

    float x, y, z;
    if (!FindSpotClearOfPoint(pTight->GetPositionX(), pTight->GetPositionY(),
                              want + PB_STANDOFF_MARGIN, PB_STANDOFF_MAX_TRAVEL,
                              x, y, z, PB_STANDOFF_CEILING))
    {
        if (IsCombatLogged())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] standofffail bot='%s' role=%s is %.1fy inside the %.0fy band "
                     "for '%s' and found nowhere to stand",
                     me->GetName(), GetRoleName(m_role), deepest, want, pTight->GetName());
        }
        return false;
    }

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] standoff bot='%s' role=%s walked out to %.1fy from '%s' "
                 "(wanted %.0fy, was %.1fy)",
                 me->GetName(), GetRoleName(m_role), pTight->GetDistance(x, y, z),
                 pTight->GetName(), want, me->GetDistance(pTight));
    }

    m_lastStandoffWalk = now;
    me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
    return true;
}

bool PartyBotAI::BackOutOfMeleeRange()
{
    if (m_role != ROLE_RANGE_DPS && m_role != ROLE_HEALER)
        return false;

    if (m_holdPosition || IsInDuel())
        return false;

    // A caster with no mana and a wand is a melee character for the rest of the fight, and
    // walking it out of range of the only attack it has left helps nobody. Matches the condition
    // BeginChasing uses to decide whether this bot keeps a standoff at all.
    if (!IsRangedDamageClass(me->GetClass()) ||
        IsAttackSpeedOverridenForm(me->GetShapeshiftForm()))
        return false;

    if (me->GetPowerPercent(POWER_MANA) <= 10.0f &&
       !me->GetWeaponForAttack(RANGED_ATTACK, true, true))
        return false;

    // Whatever is closest and can actually swing at this bot. Asked of the enemies in the fight
    // rather than of this bot's own target, because the mob that has wandered into a healer is by
    // definition not the one the healer is looking at.
    Unit* pCrowder = nullptr;
    Unit* pStuckOn = nullptr;
    float closest = 0.0f;

    std::list<Unit*> enemies;
    me->GetEnemyListInRadiusAround(me, PB_CASTER_MELEE_FLOOR, enemies);

    for (Unit* pEnemy : enemies)
    {
        if (!pEnemy || !pEnemy->IsAlive() || !pEnemy->IsCreature())
            continue;

        // Only what is in a position to hit. Something rooted or stuck across a gap is already
        // harmless, and giving up a casting position for it is the trade this is trying to win.
        if (!pEnemy->CanReachWithMeleeAutoAttack(me))
            continue;

        // And only what is actually hitting this bot. A mob the tank is holding, which happens to
        // be standing in the healer's square, does it no damage at all, so stepping out of that
        // square buys nothing and costs a cast -- and the step can walk the healer into a pack
        // nobody has pulled yet. Fifteen of thirty eight backouts in a Scarlet Monastery run were
        // this: a caster retreating from somebody else's mob.
        //
        // This reading used to be inside CanEscapeOnFoot, where "it is not attacking me" returned
        // true and so made the mob a crowder to run from, which is the opposite of what the answer
        // means. An enemy that is not on this bot is a reason to stand still.
        //
        // With one exception, and it is the one that has been killing the healer: a healer inside
        // the swing radius of something the group is fighting. The rule above is right that a mob
        // busy with the tank does the healer no damage right now, and wrong that this makes the
        // square safe -- it is one taunt miss, one cleave or one add spawning behind the tank from
        // being the most dangerous patch of floor in the room. Every Antu'sul attempt has the
        // priest logging its first standfast at two to four yards from the boss and dying inside
        // twenty seconds of the Servant arriving.
        //
        // Healers only, because a ranged damage dealer already keeps its distance through the
        // caster chase and a melee bot belongs in there. And still only against something in
        // genuine melee reach of the healer, which is what the check above established, so this
        // does not reintroduce the retreat-from-anything behaviour that rule was written for.
        bool const healerInSwingRadius =
            m_role == ROLE_HEALER && IsEngagedWithGroup(pEnemy);

        if (pEnemy->GetVictim() != me && !healerInSwingRadius)
            continue;

        // And only what the bot can actually get away from. Anything else is stood up to.
        if (!CanEscapeOnFoot(me, pEnemy))
        {
            pStuckOn = pEnemy;
            continue;
        }

        float const distance = me->GetDistance(pEnemy);
        if (!pCrowder || distance < closest)
        {
            pCrowder = pEnemy;
            closest = distance;
        }
    }

    if (!pCrowder)
    {
        // Nothing to run from, but possibly somewhere to run to.
        //
        // Standing its ground is the right answer to "I cannot outrun this", and it is the wrong
        // answer to the situation a healer is actually in. Nothing in the group outruns Antu'sul
        // -- seven against eight -- so CanEscapeOnFoot is permanently false against him and the
        // healer plants for the rest of the fight, whatever else is true. Three separate Antu'sul
        // wipes have the priest logging standfast every few seconds from two to four yards and
        // dying there.
        //
        // But running to the tank is not running away. The healer does not have to be faster than
        // the boss to win that move, because the boss follows it into the tank's lap, which is
        // where the taunt sticks and where the healer wanted the boss anyway. The tank taunted
        // Antu'sul off this priest five times in forty seconds and it walked straight back every
        // time, because the priest it was chasing was standing ten to twenty yards away.
        //
        // Healers only. A ranged damage dealer has a standoff it is entitled to keep, and a melee
        // bot is already where this would send it.
        if (pStuckOn && m_role == ROLE_HEALER && !me->IsNonMeleeSpellCasted() &&
            CanIssueCombatMovement())
        {
            if (Player* pTank = GetGroupMainTank())
            {
                float const tankDistance = me->GetDistance(pTank);

                if (pTank->IsAlive() && pTank != me &&
                    tankDistance > PB_HEALER_TANK_HUDDLE_RANGE &&
                    me->IsWithinLOSInMap(pTank))
                {
                    time_t const now = time(nullptr);
                    if (now - m_lastBackout >= PB_BACKOUT_INTERVAL)
                    {
                        if (!me->IsStopped())
                            me->StopMoving();

                        float x, y, z;
                        pTank->GetPosition(x, y, z);
                        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING);

                        m_lastBackout = now;
                        NoteCombatMovement();

                        if (IsCombatLogged())
                        {
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                     "[BotCombat] huddle bot='%s' role=healer could not outrun "
                                     "'%s', walked %.1fy to the tank '%s' to hand it over",
                                     me->GetName(), pStuckOn->GetName(), tankDistance,
                                     pTank->GetName());
                        }

                        return true;
                    }
                }
            }
        }

        // Say so, once in a while, so that a healer being eaten reads as a decision rather than as
        // an AI that has stopped noticing.
        if (pStuckOn && IsCombatLogged())
        {
            time_t const now = time(nullptr);
            if (now - m_lastStandLog >= PB_STAND_LOG_INTERVAL)
            {
                m_lastStandLog = now;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] standfast bot='%s' role=%s hp=%.0f%% stayed put with '%s' on "
                         "it at %.1fy, cannot outrun it (myspeed=%.1f itsspeed=%.1f held=%ums)",
                         me->GetName(), GetRoleName(m_role), me->GetHealthPercent(),
                         pStuckOn->GetName(), me->GetDistance(pStuckOn),
                         me->GetSpeed(MOVE_RUN), pStuckOn->GetSpeed(MOVE_RUN),
                         GetHeldInPlaceDurationMs(pStuckOn));
            }
        }

        return false;
    }

    // A cast in flight is worth more than the two yards. Interrupting it to shuffle would mean a
    // crowded caster never finishes anything, which is a worse outcome than being crowded: the
    // step happens on the next tick, between casts, and there is always a next tick.
    if (me->IsNonMeleeSpellCasted())
        return false;

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        return true;

    if (!CanIssueCombatMovement())
        return false;

    // One step per interval, the same way the held step is rationed. A mob that follows the bot
    // puts it straight back inside melee range, and without a floor between attempts the bot
    // re-decides on the next tick and spends the fight walking rather than casting. Above the
    // movement call rather than below it so a refused step costs nothing.
    time_t const now = time(nullptr);
    if (now - m_lastBackout < PB_BACKOUT_INTERVAL)
        return false;

    if (!me->IsStopped())
        me->StopMoving();

    if (!RunAwayFromTarget(pCrowder))
        return false;

    m_lastBackout = now;
    NoteCombatMovement();

    if (IsCombatLogged())
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] backout bot='%s' role=%s stepped out of '%s' melee at %.1fy",
                 me->GetName(), GetRoleName(m_role), pCrowder->GetName(), closest);

    return true;
}

bool PartyBotAI::StepAwayFromHeldAttacker()
{
    if (m_role == ROLE_TANK || m_role == ROLE_MELEE_DPS)
        return false;

    if (!me->IsInCombat() || IsInDuel() || m_holdPosition)
        return false;

    // Not mid-cast. Interrupting a heal to dodge damage the healer can out-heal is the wrong trade,
    // and the cast will be over within a tick or two anyway.
    if (me->IsNonMeleeSpellCasted())
        return false;

    // Nothing to walk with, or already walking.
    if (me->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING))
        return false;

    // One step per root, not one per tick for as long as it lasts.
    time_t const now = time(nullptr);
    if (now - m_lastHeldStep < PB_HELD_STEP_INTERVAL)
        return false;

    for (Unit* pAttacker : me->GetAttackers())
    {
        if (!pAttacker || !pAttacker->IsAlive())
            continue;

        // Only what is actually in a position to hit us. Something rooted across the room is
        // already harmless and is not a reason to give up a casting position.
        if (!pAttacker->CanReachWithMeleeAutoAttack(me))
            continue;

        // Long enough left on it to be worth the walk. Stepping out of a root with half a second
        // to run costs the position and saves one swing at most.
        if (GetHeldInPlaceDurationMs(pAttacker) < PB_HELD_STEP_MIN_REMAINING_MS)
            continue;

        if (!RunAwayFromTarget(pAttacker))
            continue;

        m_lastHeldStep = now;

        if (IsCombatLogged())
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] heldstep bot='%s' role=%s walked out of '%s' at %.1fy, held for %ums",
                     me->GetName(), GetRoleName(m_role), pAttacker->GetName(),
                     me->GetDistance(pAttacker), GetHeldInPlaceDurationMs(pAttacker));

        return true;
    }

    return false;
}

bool PartyBotAI::RecoverLineOfSight()
{
    // Ranged damage only, which is the one role this was ever about. The first live run had it
    // walking the wrong people: a tank told to step in to fifteen yards from eighty five, when
    // being in melee is its whole job and the chase already takes it there, and a healer marched
    // towards a mob it merely could not see - a healer needs line of sight to the people it heals,
    // and closing on the enemy to get it is exactly backwards.
    if (GetRole() != ROLE_RANGE_DPS)
    {
        m_blindTargetGuid.Clear();
        m_blindTicks = 0;
        return false;
    }

    Unit* pTarget = me->GetVictim();
    if (!pTarget || !pTarget->IsAlive() || pTarget->GetMap() != me->GetMap())
    {
        m_blindTargetGuid.Clear();
        m_blindTicks = 0;
        return false;
    }

    // A new target starts a new count. Otherwise a bot that spent a fight blind arrives at the
    // next one already convinced it needs to move.
    if (pTarget->GetObjectGuid() != m_blindTargetGuid)
    {
        m_blindTargetGuid = pTarget->GetObjectGuid();
        m_blindTicks = 0;
    }

    // Whatever was in the way is no longer, either because the bot drifted or because the mob
    // walked into the open. Nothing to do but forget it.
    if (me->IsWithinLOSInMap(pTarget))
    {
        m_blindTicks = 0;
        return false;
    }

    if (++m_blindTicks < PB_BLIND_TICKS_BEFORE_MOVING)
        return false;

    // A step takes time to finish and the count keeps climbing while it does, so without this the
    // bot re-launches the same walk every tick and never arrives anywhere.
    time_t const now = time(nullptr);
    if (m_lastBlindStep && (now - m_lastBlindStep) < PB_BLIND_STEP_INTERVAL)
        return false;

    // Straight down the line towards the target, stopping at whatever standoff this bot would
    // have taken anyway. A corner is cleared by getting closer to it far more reliably than by
    // sidestepping, and stopping at the standoff is what keeps this from being a charge: a
    // ranged bot ends up where a ranged bot belongs, and a melee bot was never blind for long
    // enough to reach here.
    float const distance = me->GetDistance(pTarget);

    // Not from across the dungeon. Without this the rule reads "get within fifteen yards of the
    // thing you cannot see" and acts on it at any range whatever: one capture has steps ordered
    // from a hundred and thirty six yards, which is a bot walking the length of the instance and
    // pulling everything on the way. A target that far away is not behind a corner, it is a stale
    // victim, or one that fled, or one another group is fighting, and none of those is a walk.
    if (distance > PB_BLIND_MAX_TARGET_DISTANCE)
    {
        m_blindTargetGuid.Clear();
        m_blindTicks = 0;
        return false;
    }

    float const standoff = std::max(GetTacticalStandoff(pTarget), PB_BLIND_STEP_MIN_DISTANCE);

    // Look for a spot with a view before looking for a spot that is nearer.
    //
    // Walking down the line is the crude answer and it is the one that was here: it clears a
    // corner reliably, and it clears it by spending the bot's standoff, which is the only thing
    // keeping a clothie out of the fight. Most of the time the obstruction is a pillar or a
    // doorframe and two steps sideways clears it at no cost at all, so that is worth asking
    // first. The step-in below is what happens when the answer is no.
    //
    // Capped at the range the bot is already shooting from, so this is a sideways move and never
    // a retreat out of range.
    {
        float x, y, z;
        if (FindFiringPosition(pTarget, standoff, distance, PB_BLIND_STEP_MAX_TRAVEL, x, y, z))
        {
            m_lastBlindStep = now;

            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear(false, true);
            // Idle before anything conditional: see the identical guard below, and the dead
            // world thread that taught it.
            me->GetMotionMaster()->MoveIdle();
            me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);

            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] sightstep bot='%s' role=%s could not see '%s' for %u casts, "
                         "stepping aside to (%.1f %.1f) at %.1fy rather than closing",
                         me->GetName(), GetRoleName(GetRole()), pTarget->GetName(), m_blindTicks,
                         x, y, me->GetDistance(pTarget));
            }

            m_blindTicks = 0;
            return true;
        }
    }

    if (distance <= standoff)
        return false;

    // A step, not a journey. Closing the whole gap in one order is what made this dangerous: the
    // destination is aggro-checked and every yard of the route to it is not, so the longer the
    // order the less the check is worth. Moving a little at a time means each leg is tested before
    // it is walked, and a corner that needs two legs simply takes two.
    float const stepTo = std::max(standoff, distance - PB_BLIND_STEP_MAX_TRAVEL);

    float x, y, z;
    pTarget->GetNearPoint(me, x, y, z, 0, stepTo, pTarget->GetAngle(me));

    m_lastBlindStep = now;

    if (!me->IsStopped())
        me->StopMoving();
    me->GetMotionMaster()->Clear(false, true);
    // Idle before anything conditional, because the clear above takes the default generator with
    // it and the SafeMoveTo below has a failure path. Leaving on that path used to leave the bot
    // with no movement generator at all, which the next MotionMaster::UpdateMotion answered with an
    // assertion, an uncaught throw, and a dead world thread. Every other clear in this file pushes
    // idle on the next line for the same reason; this was the one that did not.
    me->GetMotionMaster()->MoveIdle();

    // The same rule the rest of the movement obeys, with the same escape: a firing line that wakes
    // the next room is not a firing line, but going a few degrees wide of the thing in the way is
    // usually enough to get one, and standing blind is only better than the two of those failing.
    if (!SafeMoveTo(x, y, z))
        return false;

    if (IsCombatLogged())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] blind bot='%s' role=%s lvl=%u could not see '%s' for %u casts from "
                 "%.1fy, stepping in to %.1fy",
                 me->GetName(), GetRoleName(GetRole()), me->GetLevel(), pTarget->GetName(),
                 m_blindTicks, distance, standoff);
    }

    m_blindTicks = 0;
    return true;
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

    // And the escort, for the same reason as the group members above: a healer that cannot reach
    // the thing the run depends on has a problem it can solve by walking. This is the difference
    // between a healer that tried and failed and one that never tried, and on the events these
    // appear in the escort is usually the furthest forward thing in the room.
    if (Unit* pEscort = GetGuardedEscort())
    {
        if (pEscort->GetHealthPercent() < worst &&
            me->IsValidHelpfulTarget(pEscort) &&
            !(me->IsWithinDist(pEscort, reach) && me->IsWithinLOSInMap(pEscort)))
        {
            pTarget = pEscort;
        }
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
    //
    // Said out loud, because this is the one pass DecideLootRoll never gets to report and it looks
    // identical from the outside to a bot that judged the item and declined it. A group whose every
    // roll is claimed by the person in it writes no roll lines at all, and the log then reads as a
    // loot system that is not running rather than one standing aside.
    RollVote vote = ROLL_PASS;
    if (playerClaimed)
    {
        if (IsCombatLogged())
        {
            ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(itemId);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                     "[BotCombat] roll bot='%s' passes on '%s' (%u): a player needed it",
                     me->GetName(), pProto ? pProto->Name1 : "something", itemId);
        }
    }
    else
        vote = DecideLootRoll(itemId);

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

                // Gear alone is only half of what a group turns up with. Enchant what was just
                // equipped and put the hour-long consumables in the bags; drinking them happens
                // out of combat, in UseProvisionConsumables.
                ApplyProvisionEnchants();
                StockProvisionConsumables();

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

            // Gear is the one thing this branch must not touch, and for a long time that was
            // taken to mean it should touch nothing. It left roster members as level ones with
            // a level sixty in the level column: no proficiencies, so no weapon skill lines, so
            // Swords at 10 against a creature defending at 315 and a raid that missed nearly
            // every swing it took. Everything that is not gear is brought up to level here.
            MakeCharacterCurrentForLevel();

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
        // An owner who is merely offline is waited for rather than answered by deleting the party.
        // WaitForOfflineLeader carries the reasoning; the short version is that the group, and the
        // instance bind hanging off it, only survive a crashed client if the bots do.
        if (WaitForOfflineLeader())
            return;

        botEntry->requestRemoval = true;
        return;
    }

    // Back, so the clock stops. Reset here rather than in the helper: this is the one place that
    // knows the leader is present again, and a stale start time would shorten the next wait.
    m_leaderOfflineSince = 0;

    if (!pLeader->IsInWorld())
        return;

    // Cheap, and has to run before anything that reads m_tactics: the group walks through the
    // instance portal without any of this being reinitialised, so the map is the only signal that
    // the tactics have changed.
    RefreshDungeonTactics();

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
        // A wipe used to be outlived by the instruction that caused it. Attack orders, the pull
        // sequence and the hold that comes with it are all "do this now" state, and dying cleared
        // none of it, so the group would walk its corpse run, stand up, and resume: the puller
        // still pulling, everybody else still ordered onto a mob now back at full health. The
        // second wipe is a replay of the first and the group never gets to choose otherwise.
        //
        // Cleared once, on the tick the death is noticed, rather than on every dead tick. Both
        // EndPull and ReleaseHold speak to the pet, and a corpse has no business issuing pet
        // commands on a loop.
        if (!m_ordersClearedByDeath)
        {
            m_ordersClearedByDeath = true;

            if (IsPulling())
                EndPull();

            if (m_holdPosition)
                ReleaseHold();

            // EndPull and ReleaseHold each clear one of these, and a bot can die without either
            // being in progress - killed under a plain attack order, which is the common case in
            // a wipe.
            me->SetAttackOrders(ObjectGuid());
            m_pullTargetGuid.Clear();

            // The add a warrior had broken off to collect, and the target it meant to go back to.
            // Both name mobs from the fight that just killed it.
            m_gatherPeelTarget.Clear();
            m_gatherReturnTarget.Clear();
        }

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
    m_ordersClearedByDeath = false;

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
        // And a caster that still has mana. A wand is what a caster does when the bar is empty,
        // not while it is three quarters full, and once one is running the rotation does not get
        // another look in: one Antu'sul attempt has the warlock fire a wand at 22:51:33 and then
        // cast nothing at all for the next forty one seconds, mana climbing from 2,105 back to
        // 2,905 out of 3,370 the whole time, with the boss in range and alive in front of it.
        // Thirteen spells in a ninety five second fight, and it died at seventy one percent mana.
        //
        // The same note above already records this happening to a priest -- "a priest that had
        // started wanding stopped healing until the wand stopped" -- and the cancellation added
        // then only covered hunters and bots with no target. This is the rest of that fix.
        if (!me->GetVictim() ||
            (me->GetClass() == CLASS_HUNTER && me->GetCombatDistance(me->GetVictim()) < 8.0f) ||
            HasManaWorthCastingWith())
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
    {
        // Come back exactly when the cast ends rather than on the next point of the tick grid.
        //
        // The timer is reset to a flat interval at the top of every pass, so evaluation happened on
        // a fixed grid regardless of what the bot was doing, and a cast finishing between two grid
        // points left it idle for the remainder. That is not an occasional loss, it is every cast:
        // the phase is stable, so a two and a half second nuke on a quarter second grid gives up a
        // fixed slice of throughput forever, and a chain of instants on the global cooldown gives
        // up proportionally more because the window is shorter.
        //
        // Taking the smaller of the two keeps both properties. The intermediate wakes still happen
        // while a long cast runs, so the heal cancellation just above still works and channels stay
        // re-evaluable; but once the remaining time drops inside one interval the bot wakes on the
        // completion itself and the gap closes to nothing.
        if (Spell* pCurrentSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        {
            uint32 const remaining = pCurrentSpell->GetCastedTime();
            if (remaining && remaining < PB_UPDATE_INTERVAL)
                m_updateTimer.Reset(remaining);
        }

        return;
    }

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
        // Damage dealers converge on one target and stay on it. Every bot used to pick for itself
        // from whatever was nearest or had hit it last, so four bots in one pack routinely worked
        // four separate mobs: nothing died, everything kept hitting back, and the healer paid for
        // all of it. Killing one thing at a time is worth more than any rotation change, because a
        // dead mob deals no damage.
        //
        // The tank is excluded: it holds what it holds, and its target is what everyone else is
        // reading. So is the healer, which has no victim to speak of.
        Unit* pDesired = nullptr;
        bool const focusFires = (GetRole() == ROLE_MELEE_DPS || GetRole() == ROLE_RANGE_DPS);

        if (focusFires)
            pDesired = SelectGroupFocusTarget();

        // Except while peeling. The focus runs every tick and would pull the warrior straight back
        // off the add it just switched to, so the peel would be undone before it landed a swing and
        // then started again the moment the collecting logic looked - a warrior alternating between
        // two mobs and taking neither. The way back from a peel is GatherLooseEnemies' own, and it
        // goes to the target that was left rather than to whatever the focus happens to be now.
        if (!m_gatherPeelTarget.IsEmpty() && pVictim &&
            pVictim->GetObjectGuid() == m_gatherPeelTarget)
            pDesired = nullptr;

        // Unlike the rule this replaced, a live target is not a reason to stop looking: the whole
        // point of a focus is that it can move the group onto something else mid-fight, which is
        // what marking a skull is for and what nothing here previously honoured.
        bool const needsTarget = !pVictim || !IsValidHostileTarget(pVictim);

        if (needsTarget || (pDesired && pDesired != pVictim))
        {
            if (!pDesired)
                pDesired = SelectAttackTarget(pLeader);

            if (pDesired && pDesired != pVictim)
            {
                if (pVictim)
                    me->AttackStop(true);

                // Holding means not closing the distance. It does not mean standing there with no
                // target: acquiring one costs nothing while the bot stays put, and it lets a held
                // caster or hunter work on the mob as it comes in rather than waiting for it to
                // finish arriving. A held melee bot simply cannot reach yet, which is the wait.
                if (m_holdPosition)
                {
                    me->Attack(pDesired, true);
                }
                else
                {
                    AttackStart(pDesired);
                    return;
                }
            }
            else if (needsTarget && pVictim)
                me->AttackStop();
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

    // Ahead of everything else that moves the bot, because all of it -- the sight step, the
    // neighbour step, the chase, the follow -- is about where to stand within the fight, and this
    // is about being in the right fight at all. A bot feared down the pyramid stairs has no
    // business choosing a firing position at the bottom of them.
    if (ReturnToHeldGround())
        return;

    // Ahead of the hold, and deliberately not subject to it. A hold means do not close on the mob
    // and do not trail the leader; it has never meant stand behind a rock contributing nothing,
    // which is what a held bot with no line of sight actually does. The step this takes stops at
    // the standoff a ranged bot would have chosen anyway, so a held bot still ends up waiting,
    // just somewhere it can shoot from.
    // Before the bot's own sight recovery, because a healer that cannot see the person it is
    // keeping alive has a worse problem than one that cannot see what it is wanding.
    if (RecoverHealLineOfSight())
        return;

    if (me->IsInCombat() && RecoverLineOfSight())
        return;

    // Ahead of the follow and the chase below, because both of them will walk a bot straight back
    // into whatever it just stepped out of, and the refusal built into the generators only holds
    // once the bot is outside the band -- which is what this puts it.
    if (AvoidUnpulledNeighbours())
        return;

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
                // In range and out of sight is a different problem from out of range, and the
                // follow below solves only the second of them. It closes to a fixed distance on
                // a random bearing, which for a member standing behind a pillar is as likely to
                // pick the blind side as the clear one -- and having arrived, the healer is in
                // range, still cannot see, and stops moving because the follow is satisfied.
                //
                // So ask for sight directly when sight is what is missing. No standoff: the
                // whole point is that a healer stands wherever it has to in order to see the
                // person it is keeping alive.
                float const reach = GetMaxHealSpellRange();
                bool stepped = false;

                if (me->IsWithinDist(pOutOfReach, reach) && !me->IsWithinLOSInMap(pOutOfReach))
                {
                    float x, y, z;
                    if (FindFiringPosition(pOutOfReach, 0.0f, reach, PB_HEAL_SIGHT_STEP_TRAVEL,
                                           x, y, z))
                    {
                        me->GetMotionMaster()->Clear(false, true);
                        me->GetMotionMaster()->MoveIdle();
                        me->GetMotionMaster()->MovePoint(0, x, y, z, MOVE_PATHFINDING | MOVE_RUN_MODE);
                        stepped = true;

                        if (IsCombatLogged())
                        {
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                     "[BotCombat] healsight bot='%s' stepped to (%.1f %.1f) to see "
                                     "'%s', which was in range at %.1fy and behind something",
                                     me->GetName(), x, y, pOutOfReach->GetName(),
                                     me->GetDistance(pOutOfReach));
                        }
                    }
                }

                if (!stepped && pFollowing != pOutOfReach)
                    me->GetMotionMaster()->MoveFollow(pOutOfReach, PB_HEAL_REPOSITION_DIST,
                                                      frand(PB_MIN_FOLLOW_ANGLE, PB_MAX_FOLLOW_ANGLE));
            }
            // Testing the target and not merely the generator type, because a healer coming back
            // from a reposition is still following, just following the wrong unit, and a bare
            // type check leaves it trailing whoever it went to help for the rest of the fight.
            else if (pFollowing != pLeader)
            {
                float slotDistance, slotAngle;
                GetSafeFormationSlot(pLeader, slotDistance, slotAngle);
                me->GetMotionMaster()->MoveFollow(pLeader, slotDistance, slotAngle);
            }
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

    // Before the rotation, and outside the in-combat branch, because a pet is the one thing a bot
    // owns that keeps working while the bot itself is busy casting.
    UpdatePetCombat();

    if (me->IsInCombat())
        UpdateInCombatAI();

    // Last, so that it reads the global cooldown the rotation above has just started rather than
    // the one it inherited. Same argument as the cast timer: a chain of instants is governed by the
    // global cooldown, it is the shorter of the two windows and so the more expensive one to round
    // up, and waking on its expiry rather than on the next grid point is the difference between a
    // bot that acts as soon as it is allowed to and one that acts on average half an interval late.
    if (Spell* pCurrentSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        // Already scheduled against the cast, which finishes no earlier than the global cooldown
        // it shares a start with, and is the wake actually wanted.
        uint32 const remaining = pCurrentSpell->GetCastedTime();
        if (remaining && remaining < PB_UPDATE_INTERVAL)
            m_updateTimer.Reset(remaining);
    }
    else if (uint32 const gcdRemaining = me->GetGCDTimeRemaining(nullptr))
    {
        if (gcdRemaining < PB_UPDATE_INTERVAL)
            m_updateTimer.Reset(gcdRemaining);
    }
}


void PartyBotAI::UpdateOutOfCombatAI()
{
    // Before everything, and out of combat as much as in it. A Noxious Slime casts its cloud as
    // it dies, so the patch this exists for is most often laid down at the exact moment the
    // fight ends -- under the party, which is then standing in it eating and drinking while a
    // rule that only ran in combat would never be asked.
    if (StepOutOfGroundHazard())
        return;

    // A feigned hunter is out of combat by definition, so this is where the second half of the
    // trap sequence lands. It has to come before everything below, all of which would read the
    // hunter as idle and send it off to buff, drink or walk back to the group while the fight it
    // just stepped out of is still running.
    if (TryFreezingTrapSequence())
        return;

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

    // Elixirs, scrolls and weapon stones, below the class rotations because those hold the buffs
    // the rest of the group depends on and above nothing that matters: a bot with a full set of
    // consumables already up falls straight through this.
    if (UseProvisionConsumables())
        return;

    // Somebody hurt outranks somebody unbuffed. The shaman rotation already learned this against
    // totems; nothing had taught it to the buff chain, and the priest was logged casting Power
    // Word: Fortitude on a player sitting at a third health who wanted a heal.
    //
    // Above the combat check below, so a healer that has not been hit yet still heals the people
    // who have.
    if (GetRole() == ROLE_HEALER && FindAndHealInjuredAlly(90.0f, 90.0f))
        return;

    // No optional work at all once the fight is on. This whole routine is gated on *this bot*
    // being out of combat, which during a pull is most of the group: the tank engages, the healer
    // behind it is on nobody's threat list yet, and it spent that gap starting a seven hundred and
    // forty four mana buff. The group being in combat is the question worth asking, not whether
    // this particular bot has been hit yet.
    if (IsGroupInCombat())
        return;

    // Out of mana to spare, so stop here and let the next tick drink instead. Clearing the flag
    // is the point: DrinkAndEat runs earlier in the tick and refuses while a buff is in progress,
    // so leaving it set strands the bot at low mana rather than recovering it. It drinks to full
    // and finishes the buffs on a later pass.
    if (me->GetPowerType() == POWER_MANA &&
        me->GetPowerPercent(POWER_MANA) < PB_BUFF_MANA_FLOOR)
    {
        m_isBuffing = false;
        return;
    }

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

    // Rage alone is stored at ten times its displayed value, per GetCreatePowers: rage caps at
    // 1000 internally and energy at 100. Scaling energy the same way divided it by ten twice
    // over, so a rogue sitting on sixty energy logged as pw=6 and read as energy starvation.
    if (powerType == POWER_RAGE)
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
        // Named for what it means: whether this tank is top of its target's threat list. The
        // ranged and melee lines spell their position hold "holding", and one field name meaning
        // two different things by role has already caused one misreading of a capture.
        bool hasAggro = false;

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
                    hasAggro = (pTopUnit == me);
                }
            }
        }

        // Where the tank is and how far off its victim, which the other two roles have always
        // logged and this one never did. Without them a capture can show a fight being dragged
        // away from a camp five times in thirty seconds and give no way to ask the obvious
        // question, which is whether the tank is ending up back where it started each time.
        //
        // And whether it is swinging, which is the question behind the rage. Two tanks across two
        // runs held a median of eight rage and never passed twenty seven, which is too little to
        // run a threat list with and is why the group needed a hundred and thirteen peels. Rage is
        // earned by hitting and being hit, so before tuning any threshold it is worth knowing
        // whether the auto attack is even turning: melee is the state flag, swing is what is left
        // on the main hand timer.
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] tick bot='%s' role=tank lvl=%u hp=%.0f rage=%u victim='%s' vguid=%u "
                 "vhp=%.0f vdist=%.1f pos=%.1f %.1f %.1f moving=%u melee=%u swing=%u attackers=%u "
                 "nearby=%u mythreat=%.0f topthreat=%.0f top='%s' hasaggro=%u gcd=%u stance=%u "
                 "dmg=%u",
                 me->GetName(), me->GetLevel(), me->GetHealthPercent(), power,
                 pVictim ? pVictim->GetName() : "none",
                 pVictim ? pVictim->GetObjectGuid().GetCounter() : 0u,
                 pVictim ? pVictim->GetHealthPercent() : 0.0f,
                 pVictim ? me->GetDistance(pVictim) : 0.0f,
                 me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                 uint32(me->IsStopped() ? 0 : 1),
                 uint32(me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) ? 1 : 0),
                 me->GetAttackTimer(BASE_ATTACK),
                 uint32(me->GetAttackers().size()),
                 pVictim ? uint32(me->GetEnemyCountInRadiusAround(pVictim, 8.0f)) : 0u,
                 myThreat, topThreat, topName, uint32(hasAggro),
                 gcd, uint32(me->GetShapeshiftForm()), me->TakeDamageTally());
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
        // Combo points, and whose they are. A rogue's whole damage profile turns on whether the
        // points it has built are being spent, and the log could not answer that at all: the fix
        // for a rotation that produced 712 builders and no Eviscerate has to be verifiable by
        // something other than counting Eviscerates and hoping.
        uint32 comboPoints = 0;
        bool comboOnVictim = false;
        if (me->GetClass() == CLASS_ROGUE || me->GetClass() == CLASS_DRUID)
        {
            comboPoints = me->GetComboPoints();
            comboOnVictim = pVictim && me->GetComboTargetGuid() == pVictim->GetObjectGuid();
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] tick bot='%s' role=%s class=%u lvl=%u hp=%.0f pw=%u victim='%s' "
                 "vguid=%u vhp=%.0f vdist=%.1f melee=%u autorepeat=%u casting=%u moving=%u "
                 "holding=%u cp=%u cpmine=%u front=%u stealth=%u gcd=%u ttl=%.1f dmg=%u",
                 me->GetName(), GetRoleName(m_role), uint32(me->GetClass()), me->GetLevel(),
                 me->GetHealthPercent(), power,
                 pVictim ? pVictim->GetName() : "none",
                 pVictim ? pVictim->GetObjectGuid().GetCounter() : 0u,
                 pVictim ? pVictim->GetHealthPercent() : 0.0f,
                 pVictim ? me->GetDistance(pVictim) : 0.0f,
                 uint32(meleeOn ? 1 : 0), autoRepeat,
                 uint32(me->IsNonMeleeSpellCasted() ? 1 : 0),
                 uint32(me->IsStopped() ? 0 : 1),
                 uint32(m_holdPosition ? 1 : 0),
                 comboPoints, uint32(comboOnVictim ? 1 : 0),
                 uint32(m_chasingInFront ? 1 : 0),
                 uint32(me->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1 : 0),
                 gcd, EstimateSecondsToLive(pVictim), me->TakeDamageTally());
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
        else if (IsRationingHealsForTank() && GetEffectiveRole(pWorst) != ROLE_TANK)
            reason = "rationed_tank_only";
        else if (IsAlreadyHealing(pWorst->GetObjectGuid()))
            reason = "heal_in_flight";
        else
            reason = "reachable";
    }

    // Which mana band the rationing is in, so a healer that is deliberately holding back reads
    // differently in the log from one that has nothing to cast. Without it, the tightened
    // thresholds would look exactly like the idle healer they were added to stop.
    char const* ration = "none";
    if (me->IsInCombat() && me->GetPowerType() == POWER_MANA)
    {
        float const manaPercent = me->GetPowerPercent(POWER_MANA);
        if (IsRationingHealsForTank())
            ration = "tankonly";
        else if (manaPercent < CB_HEAL_MANA_CRITICAL_PERCENT)
            ration = "critical";
        else if (manaPercent < CB_HEAL_MANA_CONSERVE_PERCENT)
            ration = "conserve";
    }

    // Which gate stopped the heal, and whether the wand was up while it did. wreason says the
    // target was acceptable; this says what happened to the spell afterwards. Without it the
    // difference between "no target" and "target, but every heal refused" is invisible, and
    // the second one is what let a tank die with the healer at ninety percent mana.
    char const* healGate = "n/a";
    if (pWorst)
    {
        healGate = "no_heals";
        bool castable = false;
        for (SpellEntry const* pHeal : m_spellListDirectHeal)
        {
            if (CanTryToCastSpell(pWorst, pHeal))
            {
                castable = true;
                break;
            }
            // Ends on the cheapest heal, which is the one most likely to have been affordable,
            // so its refusal is the interesting one.
            healGate = DescribeCastRefusal(pWorst, pHeal);
        }
        if (castable)
            healGate = "ok";
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
             "[BotCombat] tick bot='%s' role=healer lvl=%u hp=%.0f mana=%.0f worst='%s' whp=%.0f "
             "wdist=%.1f reach=%.0f wreason=%s ration=%s incoming=%d casting=%u autorepeat=%u "
             "healgate=%s attackers=%u gcd=%u healed=%u dmg=%u",
             me->GetName(), me->GetLevel(), me->GetHealthPercent(),
             me->GetPowerPercent(POWER_MANA),
             pWorst ? pWorst->GetName() : "none",
             pWorst ? pWorst->GetHealthPercent() : 0.0f,
             pWorst ? me->GetDistance(pWorst) : 0.0f, reach, reason, ration,
             pWorst ? GetIncomingdamage(pWorst) : 0,
             uint32(me->IsNonMeleeSpellCasted() ? 1 : 0),
             uint32(me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) ? 1 : 0),
             healGate,
             uint32(me->GetAttackers().size()), gcd,
             me->TakeHealingTally(), me->TakeDamageTally());
}

void PartyBotAI::UpdateInCombatAI()
{
    if (IsCombatLogged())
        LogCombatTick();

    // Before every branch below, and deliberately not inside any of them. The estimate is built
    // from a difference between consecutive looks at the target, so it is only as good as the
    // regularity of the looking: sampling it from inside a rotation would stop sampling on every
    // tick that rotation returned early, which is most of the interesting ones.
    SampleVictimHealth();

    // Ahead of every early return below, because a damage dealer that took a different branch
    // this tick is still swinging.
    if (Unit* pVictim = me->GetVictim())
        HoldOpeningSwings(pVictim);

    // Before the interrupt, because both want to take the cast away from this bot and this one has
    // a deadline: a speculative heal has to be kept or dropped in the last half second before it
    // lands, and a tick spent elsewhere is a tick where that decision was not made. Reports true
    // only when it actually cancelled, so a cast being held reads as no decision and falls through
    // to everything below.
    if (ReconsiderHealInFlight())
        return;

    // Ahead of the step below and of everything under it, because this is the only damage in a
    // dungeon that nothing else in the tick can even see. A held attacker's swings are healable
    // and a bad standoff is survivable; a hundred and fifty a second from a patch of floor with
    // no attacker attached to it is neither, and the bot is taking it while every other rule
    // reports that nothing is wrong.
    if (StepOutOfGroundHazard())
        return;

    // Before the rotation, because standing in the swing radius of something that has been frozen
    // in place specifically to get it off this bot is free damage taken, and every tick spent
    // casting instead of stepping is another swing of it.
    if (StepAwayFromHeldAttacker())
        return;

    // The tank deciding where the fight happens, which is upstream of every other bot's
    // positioning problem: move the fight and the rogue's rear, the healer's range and the
    // caster's standoff all move with it. Does not return, because backing up a few yards is
    // something a tank does while continuing to hold threat, not instead of it.
    DragFightAwayFromNeighbours();

    // Melee getting back behind its target once whatever drove it round the front has gone.
    ReconsiderMeleeChaseAngle();

    // Interrupts first, ahead of the herding below and ahead of every role.
    //
    // This used to sit under GatherLooseEnemies, and that ordering was costing the fight. Herding
    // returns true whenever the tank fetches something, which against Antu'sul is constant -- one
    // capture has fifteen herd events and five peels in a ninety second fight -- and every one of
    // them consumed the tick before the interrupt check was ever reached. The result is an
    // interrupt that arrives two seconds into a three second cast: both logged interrupts of
    // Healing Wave of Antu'sul went out with castleft=1000ms and 800ms remaining, and the heal
    // landed anyway both times.
    //
    // A peel that happens a quarter of a second later costs almost nothing. A heal of two and a
    // half thousand health that lands because nobody looked costs the attempt.
    if (InterruptHostileCasters())
        return;

    // Then the potion, below the interrupt because a Healing Wave landing is worse than a tick of
    // drinking, and above the rotations because by the time a healer is empty the casts the potion
    // was going to buy have already been missed.
    if (TryUseRestorePotion())
        return;

    // The tank's taunt, ahead of the collecting below rather than after it.
    //
    // Both of them answer "something is eating the healer" and only one of them costs anything.
    // Tried in the old order, the body peel got first refusal and took it: the tank left the mob
    // it was holding, walked to the add, and the taunt path was never reached that tick because
    // collecting commits it. So the expensive tool was chosen while the free one sat unused, and
    // on an add that stays at range the expensive one cannot work at all.
    if (!IsInDuel() && m_role == ROLE_TANK && PeelForTheHealer())
        return;

    // Warriors collecting whatever is loose onto themselves and walking it back to the group.
    // Above the rotation because a caster being chewed on is worth more than this warrior's next
    // ability, and it commits the tick only when it actually did something.
    if (GatherLooseEnemies())
        return;

    // The hunter dropping combat to lay a trap under a summon the group has been told to leave
    // alone. Costs the tick when it fires and nothing at all otherwise, and it cannot run long:
    // the sequence carries its own deadline and stands itself down for thirty seconds afterwards.
    if (TryFreezingTrapSequence())
        return;

    if (!IsInDuel())
    {
        if (m_role == ROLE_TANK)
        {
            Unit* pVictim = me->GetVictim();

            // PeelForTheHealer used to be called here and is now called further up, above the
            // collecting. It belongs ahead of the two rules below for the reason it always did --
            // both of those ask about the tank's own target, it has none or the one it has has
            // turned on somebody else, and a tank happily holding one mob while a second walks
            // past it into the healer satisfies neither -- and it belongs ahead of the collecting
            // as well, because the collecting answers the same question by walking.

            // Defend party members - by taking a new target only when there is no target to keep.
            //
            // The condition here used to be "no victim, or my victim is attacking me", and the
            // second half is a tank's success case: the mob is on you, which is the job. So every
            // tick the tank was correctly holding aggro it went looking for somebody else's
            // attacker and switched to it, dropping whatever it had - including a skull the group
            // was told to kill. A loose add is a reason to taunt, which costs nothing and keeps the
            // current target; it is not a reason to abandon the mob already being held.
            if (!pVictim)
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
        // Nothing marked, so pick something sensible to take out of the fight. Behind the marked
        // path so that an explicit instruction is never overridden by a guess.
        else if (CrowdControlOffFocus())
            return;
    }

    // Dispelling competes with healing for the same tick and the same mana, and for anything whose
    // job is healing it used to win that competition unconditionally, because it sat here in front
    // of the entire rotation. A Scarlet Monastery capture has the priest's second most cast spell
    // being Dispel Magic - fifty one casts in combat, a median target at ninety one percent health,
    // and thirty of them repeats on the same ally inside twenty seconds - while the same healer
    // spent a fifth of its ticks under thirty percent mana. Frostbolt slows on a rogue were
    // outranking heals on the tank.
    //
    // So healers dispel from below the rotation, where a real heal has already had the tick. Every
    // other role keeps it here: they have no healing for it to displace.
    if (m_role != ROLE_HEALER && CheckForDispelTargets())
        return;

    // Behind the interrupt, which is the better answer to the same cast, and ahead of everything
    // that would rather stand still. See TakeCoverFromCast.
    if (TakeCoverFromCast())
        return;

    // Ahead of every class rotation, because half of them have their own version of this rule and
    // the other half have none, and the ones that have it disagree about the distance. A rule that
    // decides whether a clothed character is standing inside a raid boss's swing is not a rule to
    // leave to nine separate if-chains.
    if (BackOutOfMeleeRange())
        return;

    // And then the wider band, which only exists where an instance named one. Behind the melee
    // backout because something already swinging at this bot is the more urgent of the two, and
    // ahead of every rotation for the same reason that one is: a cast made from inside a twenty
    // yard area effect is a cast paid for twice.
    if (HoldTacticalStandoff())
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

    // Blood Fury and Berserking, on cooldowns several times longer than the fights they are used
    // in, so there is nothing to save them for. Below the rotation because they are a multiplier on
    // it rather than a substitute, and not returning, since neither costs the bot its tick.
    UseOffensiveRacial();

    // The healer's dispel, now that healing has had its chance at the tick. Above the speculative
    // heal because a debuff that is actually worth removing is worth more than a heal cast on
    // spec, and below everything that answers a health bar.
    if (m_role == ROLE_HEALER && CheckForDispelTargets())
        return;

    // Nothing needed healing this tick, which for a healer is the moment to start a cast anyway.
    // Below the rotation so a real heal always wins, and above KeepBusy because a heal already
    // two thirds cast when the tank takes a hit is worth considerably more than a wand shot.
    if (BeginSpeculativeHeal())
        return;

    // Last, and only reached when nothing above committed the tick. Every rotation is an if-chain
    // that falls out of the bottom when nothing matched, and until now falling out of the bottom
    // meant standing still.
    KeepBusy();
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
                !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS) &&
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
    AbandonFillerForHealing();

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

        // Every heal and every totem has now been declined, which for a healer used to end the
        // tick having cast nothing. Same borrowed target as the priest uses: a healer holds no
        // victim of its own, so the group's is looked up rather than read off, and it is only
        // ever shot at, never chased.
        if (Player* pLeader = GetPartyLeader())
            if (AddFillerDamage(SelectAttackTarget(pLeader)))
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

        // The pet is UpdatePetCombat's business now, which unlike this runs in combat too.
        UpdateInCombatAI_Hunter();
    }
}

void PartyBotAI::UpdateInCombatAI_Hunter()
{
    // Take the Feign Death off again, because nothing else will.
    //
    // Feign Death has a six minute duration and a feigned hunter cannot act, so "the rotation
    // resumes on the next tick and the aura drops the instant it does" -- which is what the
    // threat dump below claimed -- describes something that cannot happen. The rotation never
    // runs, so nothing removes the aura, so the rotation never runs. The trap sequence had this
    // exact bug and was fixed by giving it an explicit end; the threat dump kept it.
    //
    // Measured: the hunter dumps threat at 00:33:26 with mythreat=2322 against the tank's 2201,
    // correctly, and then produces no tick at all until 00:34:50. Eighty four seconds, from the
    // group's highest damage dealer, in a fight that stalled at twenty nine percent and wiped.
    //
    // The dump itself works on the cast: SetFeignDeath calls CombatStop and clears the threat
    // references immediately, so by the next tick the threat is already gone and there is
    // nothing left to protect. The panic use at low health wants longer -- it is buying time for
    // a heal -- so the two carry different deadlines rather than one compromise.
    if (m_spells.hunter.pFeignDeath && me->HasAura(m_spells.hunter.pFeignDeath->Id))
    {
        // The trap sequence owns its own feign and ends it in EndTrapAttempt. Two owners of one
        // aura would have this path cancelling the trap setup a tick after it began.
        if (!m_trapAttemptStart)
        {
            uint32 const now = WorldTimer::getMSTime();
            if (!m_feignUntil || now >= m_feignUntil)
            {
                me->RemoveAurasDueToSpell(m_spells.hunter.pFeignDeath->Id);
                m_feignUntil = 0;

                if (IsCombatLogged())
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] feign bot='%s' stood back up at %.0f%% health",
                             me->GetName(), me->GetHealthPercent());
                }
            }
            else
            {
                // Still down on purpose. Nothing else can be done while feigned anyway.
                return;
            }
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            BeginChasing(pVictim);
        }

        if (m_spells.hunter.pVolley &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS, pVictim) &&
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

        // Multi-Shot picks its extra targets itself, out of whatever is standing near the one it
        // is aimed at, so it is the group's most reliable way of waking its own crowd control.
        // One Antu'sul attempt has the rogue landing Blind on the Servant at 20:00:37 and the
        // hunter firing Multi-Shot in the same second: the Servant was loose again two seconds
        // later, went back to the healer, and the healer was dead inside a minute. That is the
        // whole value of the CC, thrown away by a shot worth one extra target's damage.
        // And not as a single target filler either. Multi-Shot costs roughly half again what
        // Arcane Shot does and only pays that back when it has extra targets to hit; fired at a
        // lone boss it is the most expensive way the hunter owns to deal one shot of damage.
        // The hunter's whole bar is eighteen hundred and sixty mana and one Antu'sul attempt had
        // it empty fifty eight seconds in, after which it could do nothing but auto shot for the
        // rest of the fight -- so what the early mana is spent on decides the hunter's damage far
        // more than the order the shots come in.
        if (m_spells.hunter.pMultiShot &&
            me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
            !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS, pVictim) &&
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

            // Aspect of the Monkey only when survival is genuinely the question.
            //
            // It adds dodge and nothing else -- no attack power, no damage of any kind -- so
            // swapping into it is the hunter turning its own damage off, and the trigger was
            // merely "something is within eight yards", which against a boss is the entire fight.
            // One Antu'sul capture has the hunter flipping Monkey, Hawk, Monkey, Hawk in forty
            // seconds: four global cooldowns, four mana payments, and every ranged shot in
            // between fired at the lower attack power.
            //
            // Against a boss in melee range the answer is not more dodge, it is to keep shooting
            // and let the tank hold it. Below the health floor that stops being true.
            if (m_spells.hunter.pAspectOfTheMonkey &&
                me->GetHealthPercent() < PB_HUNTER_MONKEY_HEALTH &&
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
                {
                    // The panic button, so stay down long enough for a heal to arrive.
                    m_feignUntil = WorldTimer::getMSTime() + PB_FEIGN_PANIC_MS;
                    return;
                }
            }
        }

        // The threat dump, which is a different decision from the panic button above: that one
        // waits until the hunter is nearly dead, and by then the boss has been on it for twenty
        // seconds and the healer has spent the fight's mana keeping it standing. This fires the
        // moment the lead is gone, whatever the health bar says.
        //
        // The aura is removed explicitly at the top of this function once m_feignUntil passes.
        // It does not come off by itself and it does not come off because the rotation ran: a
        // feigned hunter has no rotation. Assuming otherwise cost eighty four seconds of the
        // group's best damage in the attempt that stalled at twenty nine percent.
        if (m_spells.hunter.pFeignDeath &&
            ShouldDumpThreatWithFeignDeath(pVictim) &&
            CanTryToCastSpell(me, m_spells.hunter.pFeignDeath))
        {
            if (DoCastSpell(me, m_spells.hunter.pFeignDeath) == SPELL_CAST_OK)
            {
                // The threat is gone on the cast, so this only has to outlast the cast itself.
                m_feignUntil = WorldTimer::getMSTime() + PB_FEIGN_THREAT_DUMP_MS;

                if (IsCombatLogged())
                {
                    ThreatManager& threat = pVictim->GetThreatManager();
                    Player* pTank = GetGroupMainTank();
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] feign bot='%s' dropped threat on '%s' with Feign Death: "
                             "mythreat=%.0f tank='%s' tankthreat=%.0f wasvictim=%u",
                             me->GetName(), pVictim->GetName(), threat.getThreat(me),
                             pTank ? pTank->GetName() : "none",
                             pTank ? threat.getThreat(pTank) : 0.0f,
                             pVictim->GetVictim() == me ? 1 : 0);
                }
                return;
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {
            // Not into a snare immunity. Wing Clip's whole effect is the slow, so against
            // anything carrying MECHANIC_SNARE in its immunity mask -- which includes Antu'sul
            // and his Servants -- it is forty mana and a global cooldown for no effect at all,
            // and the log has it cast anyway.
            if (m_spells.hunter.pWingClip &&
               !pVictim->IsImmuneToMechanic(MECHANIC_SNARE) &&
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
            // Guarded against the victim, which is what the cast below actually targets. Asking
            // about the mage instead checked range and immunity on the wrong unit, so the guard
            // was not guarding anything.
            if (m_spells.mage.pConeofCold && !me->IsMoving() &&
                CanTryToCastSpell(pVictim, m_spells.mage.pConeofCold))
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
                !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS) &&
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

        // Below the two main nukes, and no longer carrying a twenty percent health gate it has no
        // use for. Scorch is a cheap fast filler, not an execute; up where it used to sit it fired
        // only in the last fifth of a fight, and ungating it in place would have replaced every
        // Frostbolt the mage ever cast. Here it is what gets cast when the nukes above could not
        // be, which is what a filler is for.
        if (m_spells.mage.pScorch &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
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

        // Wanding is handled by KeepBusy at the end of the tick, for every class at once and only
        // when there is a wand to fire.
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

    // No Shadow Protection here, deliberately.
    //
    // It is a situational buff wearing a maintenance buff's clothes. Fortitude and Divine Spirit
    // raise stats that help in every fight, so a priest keeps them up as a matter of course.
    // Shadow Protection helps only against one damage school, and the bot has no way to know
    // whether the next fight uses it -- so casting it always means paying for it always. At four
    // hundred and fifty mana a head that is thirteen hundred and fifty on a five man, better than
    // a third of a level forty two priest's pool, handed over before a boss that deals physical
    // and nature damage and casts nothing shadow at all.
    //
    // A player casts this when they know the fight calls for it. The bot does not know, so the
    // honest default is not to. If it should come back for particular encounters, the place for
    // that is a field on DungeonTactics naming the schools worth resisting, decided per map rather
    // than guessed here.

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
// The filler pair for this bot's class: one instant damage over time effect and one direct nuke.
//
// m_spells is a union, so it may only be read through the arm belonging to the bot's own class.
// Asking a priest for m_spells.shaman.pLightningBolt returns whichever priest pointer happens to
// share that offset, which is a real spell and entirely the wrong one. Both spells come from this
// one place so that the rotation below and the "was that cast a filler" test above cannot end up
// disagreeing about what a filler is.
void PartyBotAI::GetFillerDamageSpells(SpellEntry const*& pDot, SpellEntry const*& pNuke) const
{
    pDot = nullptr;
    pNuke = nullptr;

    switch (me->GetClass())
    {
        case CLASS_PRIEST:
            pDot = m_spells.priest.pShadowWordPain;
            pNuke = m_spells.priest.pSmite;
            break;
        case CLASS_SHAMAN:
            pDot = m_spells.shaman.pFlameShock;
            pNuke = m_spells.shaman.pLightningBolt;
            break;
        case CLASS_DRUID:
            pDot = m_spells.druid.pMoonfire;
            pNuke = m_spells.druid.pWrath;
            break;
        // A holy paladin has no ranged attack and cannot equip a wand, so the only filler damage
        // available to it is to walk into melee -- which is the one thing a healer must not do for
        // the sake of adding damage. It is left with nothing to do here deliberately.
        default:
            break;
    }
}

bool PartyBotAI::AddFillerDamage(Unit* pTarget)
{
    if (!pTarget || !IsValidHostileTarget(pTarget) || !me->IsWithinLOSInMap(pTarget))
        return false;

    bool const manaToSpare = me->GetPowerPercent(POWER_MANA) >= PB_FILLER_MANA_PERCENT &&
                            !SelectHealTarget(PB_FILLER_PARTY_HEALTH, PB_FILLER_PARTY_HEALTH);

    if (manaToSpare)
    {
        SpellEntry const* pDot = nullptr;
        SpellEntry const* pNuke = nullptr;
        GetFillerDamageSpells(pDot, pNuke);

        // Cheapest damage per point of mana in the book and instant, so it costs the group no
        // healing latency at all. Only worth applying once: a dot recast on top of itself throws
        // away every tick it had left.
        if (pDot &&
           !pTarget->HasAura(pDot->Id) &&
            CanTryToCastSpell(pTarget, pDot))
        {
            if (DoCastSpell(pTarget, pDot) == SPELL_CAST_OK)
                return true;
        }

        if (pNuke &&
            CanTryToCastSpell(pTarget, pNuke))
        {
            if (DoCastSpell(pTarget, pNuke) == SPELL_CAST_OK)
                return true;
        }
    }

    // The wand asks for no mana whatsoever, so it is not gated on having any to spare: a healer
    // saving every point for the tank should still be firing it. KeepBusy owns the firing itself,
    // for every class at once and only where there is a wand to fire.
    return KeepBusy();
}

// Whether the spell currently going out is one of the fillers above. Cheaper than tracking a flag
// through the cast and it cannot fall out of step with what was actually cast.
bool PartyBotAI::IsCastingFillerDamage() const
{
    Spell const* pSpell = me->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!pSpell || !pSpell->m_spellInfo)
        return false;

    SpellEntry const* pDot = nullptr;
    SpellEntry const* pNuke = nullptr;
    GetFillerDamageSpells(pDot, pNuke);

    uint32 const id = pSpell->m_spellInfo->Id;
    return (pNuke && id == pNuke->Id) ||
           (pDot && id == pDot->Id);
}

// The wand counts too.
//
// AddFillerDamage ends in KeepBusy, which fires the wand, and the wand is what a healer
// conserving mana falls through to -- so it is the filler a healer is actually running most
// of the time. It was invisible here because it lives in CURRENT_AUTOREPEAT_SPELL rather
// than CURRENT_GENERIC_SPELL, so the abandon above never fired for it and the healer stayed
// on the wand while the tank died. One capture has the priest wanding Aku'mai for twenty two
// seconds with full mana, watching the tank fall from 86 percent to dead without attempting
// a single heal.
bool PartyBotAI::IsCastingFillerAutoRepeat() const
{
    return me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;
}

// A filler nuke is worth abandoning the instant somebody actually needs healing. Nothing else can
// be cast while one is going out, so a filler started against a healthy group and left to run holds
// up the first heal of a fight that has just turned.
//
// Shared by every healer that has a filler, rather than sitting inside one class's rotation: the
// shaman and druid were given the same filler treatment as the priest, and a filler nobody will
// abandon is worse than no filler at all.
void PartyBotAI::AbandonFillerForHealing()
{
    if (GetRole() != ROLE_HEALER)
        return;

    if (!IsCastingFillerDamage() && !IsCastingFillerAutoRepeat())
        return;

    if (!SelectHealTarget(PB_FILLER_ABANDON_HEALTH, PB_FILLER_ABANDON_HEALTH))
        return;

    // Cancels the wand as well as a cast: InterruptNonMeleeSpells covers
    // CURRENT_AUTOREPEAT_SPELL, and KeepBusy will not start another while one is running.
    me->InterruptNonMeleeSpells(false);
}

void PartyBotAI::UpdateInCombatAI_Priest()
{
    AbandonFillerForHealing();

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
    // What counts as worth absorbing is stricter for a healer, because its bar is the group's.
    //
    // "Something is attacking me" was the test, and at Antu'sul something is always attacking the
    // healer -- the Broodlings see to that. The measured result is a three hundred mana shield
    // thrown on a priest at one hundred percent health in the first second of the fight. A healer
    // buys an absorb when it is actually losing health; anything else is a heal it will not be
    // able to cast later.
    bool const worthAbsorbing = GetRole() == ROLE_HEALER
        ? me->GetHealthPercent() < PB_HEALER_SELF_SHIELD_HEALTH
        : (!me->GetAttackers().empty() || me->GetHealthPercent() < 90.0f);

    if (m_spells.priest.pPowerWordShield &&
        worthAbsorbing &&
        !(GetRole() == ROLE_HEALER && SelectHealTarget(60.0f, 80.0f)) &&
        HasManaToSpendOnAbsorbs() &&
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
        if (m_spells.priest.pPowerWordShield && HasManaToSpendOnAbsorbs())
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
            // Holy Nova is the worst mana per point of healing the priest owns, and 83 casts of it
            // went out in one run. Not for a healer at all, at any mana level: the condition it
            // needs is three things in melee range of the priest, and a healer that finds itself
            // there is not looking for an area spell to cast, it is in the wrong place and
            // BackOutOfMeleeRange is about to walk it out. Leaving it available above a mana
            // threshold was the previous compromise and it only meant every priest in the raid
            // spent down to the threshold before healing became the priority.
            if (m_spells.priest.pHolyNova &&
                GetRole() != ROLE_HEALER &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS) &&
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

    // The pet is UpdatePetCombat's business now, which unlike this runs in combat too.
    if (me->GetVictim())
        UpdateInCombatAI_Warlock();
}

// Whether a damage-over-time spell will live long enough on this target to earn back the cast that
// applies it. On trash it does not: the mob dies inside a couple of direct casts, so the dot is a
// cast that delivered a fraction of its damage and a mob that took longer to die for it. Curse of
// Agony is the plainest case, ramping so that most of its total lands in its final third, but
// Corruption and Immolate lose on a short fight too, and between them they were taking every cast
// the rotation had -- Shadow Bolt sits last, behind all three, and never got a look in.
// One sample of the current target's health, kept so that the rotation can ask how long the
// target has left instead of guessing from how much of it is gone.
//
// Health percentage cannot answer that question and was what every previous attempt used. A mob
// at thirty percent is nearly dead if the party is killing it in six seconds and has half a
// minute left if the party is barely scratching it, and a rotation cannot tell those apart from
// the percentage alone -- which is how a rogue ended up spending its whole bar at two combo
// points on a boss and holding it at three on a boar until the boar died.
//
// Only downward movement counts. A mob that was healed says nothing about how fast it is dying,
// and a mob whose health went up because it evaded and reset says less than nothing.
void PartyBotAI::SampleVictimHealth()
{
    Unit const* pVictim = me->GetVictim();
    uint32 const now = WorldTimer::getMSTime();

    if (!pVictim || !pVictim->IsAlive())
    {
        m_ttlVictimGuid.Clear();
        m_ttlDamagePerSecond = 0.0f;
        return;
    }

    // A new target starts a new measurement. Carrying the old rate across would have the rogue
    // spend its opening bar on a full health mob because the last one died fast.
    if (pVictim->GetObjectGuid() != m_ttlVictimGuid)
    {
        m_ttlVictimGuid = pVictim->GetObjectGuid();
        m_ttlLastHealth = pVictim->GetHealth();
        m_ttlLastSample = now;
        m_ttlDamagePerSecond = 0.0f;
        return;
    }

    uint32 const elapsed = WorldTimer::getMSTimeDiff(m_ttlLastSample, now);
    if (elapsed < PB_TTL_SAMPLE_INTERVAL_MS)
        return;

    uint32 const health = pVictim->GetHealth();

    // A window in which the target lost nothing is evidence too, and feeding it in as a zero is
    // what lets the estimate decay when a fight stops rather than holding the last rate forever.
    // A mob that has been crowd controlled and left alone should read as living a long time,
    // because it is.
    float const sample = (health < m_ttlLastHealth)
                       ? float(m_ttlLastHealth - health) * 1000.0f / float(elapsed)
                       : 0.0f;

    m_ttlDamagePerSecond = (m_ttlDamagePerSecond > 0.0f)
                         ? (m_ttlDamagePerSecond * (1.0f - PB_TTL_SMOOTHING)) + (sample * PB_TTL_SMOOTHING)
                         : sample;

    m_ttlLastHealth = health;
    m_ttlLastSample = now;
}

// How many seconds the target has left at the rate it is currently losing health. Reports a long
// time when it does not know, which is the safe direction: a bot with no evidence should behave
// as it would on a fight worth investing in, and every caller treats a short answer as licence to
// spend everything it has.
float PartyBotAI::EstimateSecondsToLive(Unit const* pVictim) const
{
    if (!pVictim || !pVictim->IsAlive())
        return 0.0f;

    if (pVictim->GetObjectGuid() != m_ttlVictimGuid || m_ttlDamagePerSecond <= 0.0f)
        return PB_TTL_UNKNOWN_SECONDS;

    return float(pVictim->GetHealth()) / m_ttlDamagePerSecond;
}

// How long until this rogue can land one more combo point: the wait for enough energy to pay for
// a builder, plus the global cooldown that builder occupies.
//
// Measured against Sinister Strike rather than whichever builder the rotation will actually pick,
// because it is the one always available and the one the rogue falls back to when it cannot get
// behind the target. Backstab costs more, so the estimate is optimistic by the difference on the
// ticks where Backstab is what gets cast, which errs towards building - the same direction the
// unknown case errs, and the cheaper mistake of the two.
float PartyBotAI::EstimateSecondsPerComboPoint() const
{
    SpellEntry const* pBuilder = m_spells.rogue.pSinisterStrike;
    if (!pBuilder)
        return PB_ROGUE_GCD_SECONDS;

    // Talent reductions are real rage and energy off the cost, so the cost has to come from the
    // same place the cast will take it from rather than from the spell data. Passing false for
    // dropModCharge asks what it would cost without spending a talent's proc charge to find out.
    uint32 const cost = Spell::CalculatePowerCost(pBuilder, me, nullptr, nullptr, false);
    uint32 const energy = me->GetPower(POWER_ENERGY);

    // Twenty energy a regeneration tick, before the server's own rate multiplier.
    float const perSecond = (20.0f * sWorld.getConfig(CONFIG_FLOAT_RATE_POWER_ENERGY)) /
                            (float(REGEN_TIME_PLAYER_FULL) / 1000.0f);

    float wait = 0.0f;
    if (energy < cost && perSecond > 0.0f)
        wait = float(cost - energy) / perSecond;

    return wait + PB_ROGUE_GCD_SECONDS;
}

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

        // Demonic Sacrifice used to sit here, conditioned on nothing beyond the warlock having a
        // living pet, so every warlock bot destroyed its own pet the moment it could and then
        // fought the rest of the instance without one. The buff it leaves behind is worth having
        // only for a build that has given up on the pet entirely, and nothing in the bot's spell
        // data can tell that build apart from any other; until a spec profile exists to ask,
        // keeping the pet is the better of the two guesses by a long way.

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

        // Curse of Agony with the other damage over time effects rather than below Fear and
        // Drain Life, which is where it was and which meant it landed last or not at all. It is
        // the warlock's cheapest damage per point of mana and it runs for twenty four seconds, so
        // on anything that lives long enough to be worth a curse it wants to be up in the first
        // few seconds alongside Immolate and Corruption, not after the fight has turned.
        if (m_spells.warlock.pCurseofAgony &&
            IsWorthDotting(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofAgony))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pCurseofAgony) == SPELL_CAST_OK)
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

        // Rain of Fire underneath the single target damage over time effects, not above them.
        //
        // It is an eight second channel, and above the dots it was the first thing a warlock did
        // on any pull with three mobs in it -- which in Zul'Farrak is the pull that hatches four
        // broodlings in front of the boss. One capture has the warlock channel Rain of Fire at
        // 22:51:22 and not get Immolate onto Antu'sul until 22:51:31, nine seconds of the boss
        // fight spent on trash the tank was already holding. The dots go on first now; if there
        // is still a crowd worth the channel afterwards, it is still here.
        if (m_spells.warlock.pRainOfFire &&
           (me->GetEnemyCountInRadiusAround(pVictim, 10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pRainOfFire))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pRainOfFire) == SPELL_CAST_OK)
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

        // Underneath Shadow Bolt rather than above it, and no longer pretending to be an execute.
        // Searing Pain is a filler nuke: the twenty percent health gate it used to carry is the
        // one Shadowburn wants, copied onto a spell that has no such role, which left it dead for
        // almost every second of every fight. Down here it is what the warlock casts when the
        // Shadow Bolt above could not be cast, which is the only time it is the right answer.
        if (m_spells.warlock.pSearingPain &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        // Tapped at thirty percent rather than ten. Life Tap is how a warlock funds the rest of
        // the fight, and at ten percent the mana is already gone: the bot spent the intervening
        // seconds unable to afford a Shadow Bolt, which is precisely the stretch the tap exists to
        // prevent. The health floor is what keeps this from being a way to die.
        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        // The wand tail that used to sit here is gone. KeepBusy fires one at the end of every
        // combat tick that committed to nothing, which covers this case and several the mana
        // threshold here never did - no target in casting range, everything on cooldown, silenced -
        // and unlike this it first checks there is actually a wand equipped.
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

    // The pull, which has to be decided before the stance rule below rather than after it: that
    // rule pins a tank in Defensive Stance, Charge is Battle Stance only, and so a tank warrior
    // could never Charge anything. That was written down as an accepted trade -- "losing the gap
    // closer costs a tank bot very little, because it is following the party rather than
    // initiating" -- and the premise is no longer true. The group is being asked to let the tank
    // arrive first, and Charge is how a warrior does that: it crosses the gap faster than anyone
    // else can walk it, stuns what it lands on, and arrives with rage already in the bar.
    //
    // The thrash this replaced is worth remembering, because the shape is easy to recreate: a
    // capture with thirty one Battle Stances, thirty one Defensive Stances and twenty nine Charges
    // in half an hour, the two rules taking turns and the tank opening every fight of a dungeon on
    // an empty rage bar. The guard against it is that the swap is only ever made when the Charge
    // is genuinely there to be cast -- target unengaged, in band, off cooldown -- so the stance
    // change is always immediately followed by the Charge rather than by another stance change.
    if (tanking && !m_holdPosition && ShouldChargeToPull())
    {
        if (me->GetShapeshiftForm() != FORM_BATTLESTANCE)
        {
            if (m_spells.warrior.pBattleStance &&
                CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
            {
                if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
                    return;
            }
        }
        else if (CanTryToCastSpell(me->GetVictim(), m_spells.warrior.pCharge))
        {
            if (DoCastSpell(me->GetVictim(), m_spells.warrior.pCharge) == SPELL_CAST_OK)
            {
                if (IsCombatLogged())
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                             "[BotCombat] charge bot='%s' role=tank opened on '%s' from %.1fy",
                             me->GetName(), me->GetVictim()->GetName(),
                             me->GetDistance(me->GetVictim()));
                }
                return;
            }
        }
    }

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

// Whether there is a Charge to open with right now.
//
// Deliberately narrow. Every condition here exists to make sure that answering yes is followed by
// a Charge and not by a second opinion, because the cost of being wrong is a stance swap that
// undoes itself and a tank that starts the fight with no rage.
bool PartyBotAI::ShouldChargeToPull() const
{
    if (!m_spells.warrior.pCharge || !m_spells.warrior.pBattleStance)
        return false;

    // Only as an opener. Charge is out-of-combat only, and a tank already fighting has nothing to
    // gain from leaving Defensive Stance.
    if (me->IsInCombat())
        return false;

    Unit* pVictim = me->GetVictim();
    if (!pVictim || !pVictim->IsAlive() || pVictim->IsInCombat())
        return false;

    if (!IsValidHostileTarget(pVictim) || !me->IsWithinLOSInMap(pVictim))
        return false;

    if (!me->IsSpellReady(m_spells.warrior.pCharge))
        return false;

    // In the band. Charge has a minimum range as well as a maximum, and a tank standing inside the
    // minimum is close enough to walk the rest.
    float const distance = me->GetDistance(pVictim);
    if (distance < PB_CHARGE_MIN_RANGE || distance > PB_CHARGE_MAX_RANGE)
        return false;

    // And not into something the group has not agreed to fight. Charge crosses the ground between
    // in a straight line at speed, which is the one movement no detour can be applied to, so the
    // ordinary route rule is asked about the whole of it up front.
    return !me->WasPullRouteRefusedRecently(PB_SPRINT_AFTER_REFUSAL_MS) &&
           !PathWouldAggroUnengaged(pVictim->GetPositionX(), pVictim->GetPositionY(),
                                    pVictim->GetPositionZ());
}

// Whether this healer can still afford to spend mana on absorbing damage rather than undoing it.
//
// Power Word: Shield is the worst conversion in the priest's book -- roughly three hundred mana for
// four hundred and forty absorbed, against Greater Heal's nine hundred for three hundred and
// seventy -- and it is bought first, which is the wrong way round when the pool has to last. The
// Antu'sul attempt that got closest has the priest spending about twelve hundred mana on four
// shields, running dry forty eight seconds in, and then casting nothing but rank one Lesser Heal
// while the tank fell from forty four percent to dead. Two of those shields went out at under a
// third of a bar remaining.
//
// Only a healer is held to this. A shadow priest's shield is what keeps its own cast going and it
// is not the group's mana bar being rationed.
bool PartyBotAI::HasManaToSpendOnAbsorbs() const
{
    if (GetRole() != ROLE_HEALER)
        return true;

    return me->GetPowerPercent(POWER_MANA) >= PB_SHIELD_MANA_FLOOR;
}

// Whether this bot should keep its global cooldown free for an interrupt rather than spend it.
//
// The last measurable gap in stopping Antu'sul's heals, and the one the castleft instrumentation
// was added to find. At 22:44:20 the rogue spent its global on Slice and Dice; Healing Wave of
// Antu'sul started inside that global; the global cleared at 22:44:21 and the Kick went out at
// 22:44:22 with eight hundred milliseconds of a three second cast left. The log recorded an
// interrupt and the boss gained a full Wave a second later. The ability was never the problem --
// it was off cooldown, in range and affordable the whole time. The bot had simply spent the tick
// the cast began on.
//
// So the reserve extends from the ability to the global itself. While a mob known to own a heal
// worth stopping is inside the health band where that heal switches on, a bot holding a ready
// interrupt stops casting filler and auto attacks instead. It costs a Sinister Strike or a Sunder
// and it buys the difference between reacting in two hundred milliseconds and reacting in two
// seconds, against a heal worth twenty four hundred health a cast.
//
// Narrow on purpose. It wants a ready interrupt, so a bot with nothing to hold back is never
// slowed; it wants a mob whose own book contains the heal, read from the creature rather than
// guessed; and it wants that mob low enough that the heal is actually live, because a boss at
// full health has not started healing and the group needs the damage to get it there.
bool PartyBotAI::ShouldReserveGlobalCooldownForInterrupt(Unit const* pVictim) const
{
    if (!pVictim || IsInDuel())
        return false;

    // Where this particular creature's heals switch on, read from the instance table rather
    // than from a constant. The constant was set to sixty five because that is where Antu'sul
    // starts casting Flash Heal, which made every bot in the game hold its rotation from sixty
    // five percent of every boss, most of which never heal at all.
    //
    // A creature with no entry answers zero and is never reserved against, which is the right
    // default: the reserve costs a Sunder or a Sinister Strike every tick it is on, and that is
    // only worth paying where there is a heal to stop.
    float const watchBelow = m_tactics
                           ? m_tactics->GetHealWatchPercent(pVictim->GetEntry())
                           : 0.0f;

    if (watchBelow <= 0.0f || pVictim->GetHealthPercent() > watchBelow)
        return false;

    if (GetWorstKnownCastPriority(pVictim) < PB_INTERRUPT_HEAL)
        return false;

    // Already casting, in which case InterruptHostileCasters is about to spend the ability this
    // very tick and there is nothing to reserve it for.
    if (GetInterruptPriority(pVictim) >= PB_INTERRUPT_HEAL)
        return false;

    std::vector<SpellEntry const*> candidates;
    GetInterruptSpells(candidates);

    for (SpellEntry const* pCandidate : candidates)
    {
        if (!me->IsSpellReady(pCandidate))
            continue;

        if (me->GetPower(Powers(pCandidate->powerType)) < Spell::CalculatePowerCost(pCandidate, me))
            continue;

        if (pCandidate->rangeIndex == SPELL_RANGE_IDX_COMBAT &&
            !me->CanReachWithMeleeAutoAttack(pVictim))
            continue;

        // Holding the global for an ability this target is immune to is the worst version of
        // this: the bot stops casting, the heal lands anyway, and the damage that would have
        // shortened the fight was never dealt. Against Antu'sul the reserve was suppressing the
        // tank's rotation for the whole stretch below sixty five percent, for nothing.
        if (!CanInterruptWith(pCandidate, pVictim))
            continue;

        return true;
    }

    return false;
}

// Whether this bot has enough mana that wanding is the wrong thing to be doing.
//
// Only for classes whose damage comes out of a mana bar in the first place. A hunter wands
// nothing, a warrior has no bar, and a rogue's energy is not what this is about.
bool PartyBotAI::HasManaWorthCastingWith() const
{
    if (me->GetPowerType() != POWER_MANA || me->GetClass() == CLASS_HUNTER)
        return false;

    // Damage dealers only, and never a healer. This is the whole of the regression it caused.
    //
    // A healer's rotation is supposed to produce nothing when nobody needs healing, and its mana
    // is reserved for the heals rather than spent on damage -- so "has mana, therefore should be
    // casting" is true of a warlock and false of a priest. Applied to the healer it refused the
    // wand two hundred and twenty seven times in one fight at around twenty percent mana, and
    // because the refusal happens above the ranged attack rather than instead of it, what was
    // left was the melee auto attack the attack order had already switched on: the priest walked
    // into the boss and swung a mace at it for the whole fight.
    // A healer wands only on a bar it has no other use for.
    //
    // Scoping this to the damage dealers fixed the priest swinging a mace and replaced it with the
    // priest firing a wand, which is the same mistake at range. The Antu'sul attempt that reached
    // twenty eight percent has the healer shoot Antu'sul more than twenty times from twelve yards
    // down to three point nine, collect the boss, and spend three Power Word: Shields keeping
    // itself alive inside its own swing radius -- nine hundred mana, a quarter of the bar, bought
    // by damage worth twenty seven a shot. It was rationing to the tank alone forty seconds later
    // and casting rank one Lesser Heal by the end.
    //
    // Above ninety percent there is nothing to ration and the shot is free. Below it the mana is
    // spoken for, and so is the threat.
    if (m_role == ROLE_HEALER)
        return me->GetPowerPercent(POWER_MANA) < PB_HEALER_WAND_MANA_CEILING;

    if (m_role != ROLE_RANGE_DPS)
        return false;

    return me->GetPowerPercent(POWER_MANA) >= PB_WAND_MANA_FLOOR;
}

bool PartyBotAI::ShouldTauntTarget(Unit const* pVictim) const
{
    // Somebody else's target. The rule below already refuses to taunt off another tank, and
    // that was enough with one tank in the group; with four it left every off-tank free to
    // taunt the main tank's mob the moment a caster took it, so a single slip was answered by
    // four taunts and the mob finished on whichever off-tank taunted last. The tank that is
    // supposed to be holding it is the only one that should be reaching for a taunt.
    if (!IsAssignedTankFor(pVictim))
        return false;

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

    // Shield Bash is worth more than a Sunder. Below the stance swap, which has to happen first
    // because Shield Bash cannot be cast outside Defensive, and below nothing else: the tank's
    // own survival cooldowns are further down and a dead tank interrupts nothing, but those are
    // health-gated and will fire through this when they are actually needed.
    if (pVictim && me->GetHealthPercent() > PB_TANK_RESERVE_HEALTH_FLOOR &&
        ShouldReserveGlobalCooldownForInterrupt(pVictim))
        return;

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
        // In melee reach before shouting, because a shout is centred on the warrior and not on
        // the target: out of range it debuffs nothing, so the aura guard above never becomes
        // true and the shout is bought again on every cooldown forever. That is the Molten Core
        // capture's sixty four Demoralizing Shouts against thirty three Sunder Armors, from a
        // tank that had lost the giant and was chasing it around the room the whole time.
        if (m_spells.warrior.pDemoralizingShout &&
            me->CanReachWithMeleeAutoAttack(pVictim) &&
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

    // Spend the surplus. Heroic Strike and Cleave are off the global cooldown and land on the
    // next swing rather than costing a cast, so the only thing they compete for is rage.
    //
    // This used to sit above the threat list behind a tuned floor of sixty rage, scaled to
    // forty four at level thirty four. Two tanks across two runs never exceeded twenty seven
    // over more than fifteen hundred ticks, so the floor was not conservative, it was
    // unreachable: Heroic Strike and Cleave were cast zero times in six hundred and fourteen
    // casts. Both halves of that are now different.
    //
    // It sits last, so it can only take a tick the threat abilities did not want - which in
    // practice means during the global cooldown one of them just started, exactly where a free
    // ability belongs. And the floor is no longer a number to argue about. What it has to
    // guarantee is that spending here does not cost the next Sunder Armor, and the spell costs
    // answer that directly, at every level and every rank.
    if (SpellEntry const* pDump = (m_spells.warrior.pCleave &&
                                   me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
                                   !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS, pVictim))
                                ? m_spells.warrior.pCleave
                                : m_spells.warrior.pHeroicStrike)
    {
        uint32 floor = Spell::CalculatePowerCost(pDump, me);
        if (m_spells.warrior.pSunderArmor)
            floor += Spell::CalculatePowerCost(m_spells.warrior.pSunderArmor, me);

        if (me->GetPower(POWER_RAGE) >= floor &&
            CanTryToCastSpell(pVictim, pDump))
        {
            if (DoCastSpell(pVictim, pDump) == SPELL_CAST_OK)
                return;
        }
    }

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
       !me->CanReachWithMeleeAutoAttack(pVictim))
        BeginChasing(pVictim);
}

void PartyBotAI::UpdateInCombatAI_Warrior()
{
    if (Unit* pVictim = me->GetVictim())
    {
        // No interrupt branch here. Shield Bash and Pummel are spent by InterruptHostileCasters,
        // which is the only place that knows what is being cast, whether the mob has something
        // worse coming, and whether another bot has already taken this cast away.
        //
        // What used to stand here fired either of them at any non-melee cast at all, and against
        // Antu'sul that lost the fight on its own. He summons and drops totems on an eleven second
        // timer and heals on a twelve second one, so the raw branch spent Shield Bash on a totem
        // roughly every time it came off cooldown -- the log has it at 20:00:28, 20:00:48,
        // 20:01:07 and 20:01:19, its cooldown to the second -- and the reserve logged nointerrupt
        // in the same second, having correctly decided to hold it for the heal. Both interrupts
        // then landed on the same Healing Wave at 20:01:19 because this path does not consult the
        // share window either, and the next Wave three seconds later returned a third of his
        // health bar with nothing left to stop it.

        // A tank wants a different list in a different order from a warrior who is there to do
        // damage, and sharing one costs it most of what it has: the order below is priority,
        // since the first thing that casts returns, and the shared version spends the early
        // slots on Execute, Overpower and Rend while Sunder Armor waits behind them.
        if (m_role == ROLE_TANK)
        {
            UpdateInCombatAI_WarriorTank(pVictim);
            return;
        }

        // Everything from here is a damage warrior, and the two abilities that decide how much
        // damage one does were both missing from this list entirely. Splitting the tank's rotation
        // out left them behind in it: across a whole run the tank used Bloodrage 113 times and
        // Battle Shout 60, and the damage warrior managed four of each.
        //
        // Bloodrage first. It is free, it is off the global cooldown, it is twenty rage on a one
        // minute cooldown, and a level twenty warrior spends most of a fight under fifteen rage -
        // 165 ticks out of 194 in the capture - which is below the cost of every ability it owns.
        // A warrior with no rage is a warrior auto-attacking, so this is worth more than anything
        // it could be spent on.
        if (m_spells.warrior.pBloodrage &&
            me->GetPower(POWER_RAGE) < PB_WARRIOR_DPS_RAGE_LOW &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            if (DoCastSpell(me, m_spells.warrior.pBloodrage) == SPELL_CAST_OK)
                return;
        }

        // Then Battle Shout, which is not this warrior's damage but the whole group's: attack
        // power for every melee in the party, on a two minute duration that nothing was renewing
        // once a fight had started. Out of combat buffing covered the pull and nothing after it.
        if (m_spells.warrior.pBattleShout &&
           !me->HasAura(m_spells.warrior.pBattleShout->Id) &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;
        }

        // Overpower is not an ability the warrior picks, it is one the target hands over. It only
        // becomes castable after that target dodges, which this core records by giving the warrior
        // a combo point on it, and CheckPower turns a missing one into SPELL_FAILED_BAD_TARGETS.
        //
        // Nothing here asked, so the rotation offered it on nearly every tick of every fight and
        // the server refused nearly every one: two thousand and five attempts in one Wailing
        // Caverns clear, one thousand nine hundred and eighty eight of them rejected. The check is
        // the same shape as the rogue finisher gate, and for the same reason - combo points belong
        // to a target, so a stale point on the last mob is not a licence to Overpower this one.
        if (m_spells.warrior.pOverpower &&
            me->GetComboPoints() > 0 &&
            me->GetComboTargetGuid() == pVictim->GetObjectGuid() &&
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
            !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS, pVictim) &&
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
            !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS) &&
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

        // The rage dump, and the threshold was set for a warrior that has rage. Thirty is what a
        // sixty warrior sits on between abilities; a twenty warrior reaches it a handful of times
        // a fight, so Heroic Strike went out twelve times across a whole run against Overpower's
        // 218. Twenty is enough for the fifteen it costs with a little left over, and being at the
        // bottom of the list is correct for a dump: everything above it is either cheaper per
        // point of damage or has a cooldown to respect.
        if (me->GetPower(POWER_RAGE) > PB_WARRIOR_DPS_RAGE_DUMP)
        {
            if (m_spells.warrior.pCleave && me->GetEnemyCountInRadiusAround(pVictim, 8.0f) > 1 &&
                !IsBreakableCrowdControlInRange(PB_AOE_CC_SAFETY_RADIUS, pVictim))
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

    // Stealth is for before a fight, not during one. Having a victim counted as a reason to
    // stealth, and a victim is exactly what a rogue has once the fight has started, so it spent
    // its openers re-stealthing into melee where the next swing broke it again: one run has 104
    // Stealth casts and not a single Ambush, Garrote or Cheap Shot to show for them.
    //
    // Battlegrounds keep the old behaviour, where dropping out of a fight to re-stealth is a real
    // move rather than a wasted global cooldown.
    if (me->IsInCombat() && !me->InBattleGround())
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
    // Poisons go on through CastWeaponBuff, which leaves no cast line, so a whole run's telemetry
    // could not answer whether the rogues were fighting with poisoned weapons or bare ones - and
    // poisons are a real share of a rogue's damage. One line each way settles it: which poison
    // went on which hand, or that the bot has none to put on.
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
        {
            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] poison bot='%s' applied '%s' to the main hand",
                         me->GetName(), m_spells.rogue.pMainHandPoison->SpellName[0].c_str());
            }
            return;
        }
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
        {
            if (IsCombatLogged())
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                         "[BotCombat] poison bot='%s' applied '%s' to the off hand",
                         me->GetName(), m_spells.rogue.pOffHandPoison->SpellName[0].c_str());
            }
            return;
        }
    }

    if (!m_spells.rogue.pMainHandPoison && IsCombatLogged() &&
        (time(nullptr) - m_lastPoisonLog) >= PB_POISON_LOG_INTERVAL)
    {
        m_lastPoisonLog = time(nullptr);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "[BotCombat] poison bot='%s' has no damage poison it can make, fighting bare",
                 me->GetName());
    }

    if (EnterStealthIfNeeded(m_spells.rogue.pStealth))
        return;

    // A victim is not a reason to start a fight. This ran the whole combat rotation on nothing more
    // than one being set, so a rogue left holding a target the group was not fighting would stealth
    // up, close on it and open, and the first anyone knew of the pull was the tank taunting it back.
    // Whatever set that victim -- a stale order, the leader's target, the last thing it fought -- is
    // not an instruction to engage, and out of combat there is nobody to notice the difference.
    //
    // Three things make opening legitimate, and all three are asked rather than assumed. Being the
    // designated puller, which is the one role whose job is to open. Standing under an explicit
    // attack order, which is what `partybot attackstart` sets and what the suites drive. Or a victim
    // the group is already engaged with, which is the ordinary case of a bot that simply has not
    // been hit yet. Anything else waits, which is what a rogue at the anchor should be doing.
    if (Unit* pVictim = me->GetVictim())
    {
        if (IsPulling() || me->HasAttackOrders() || IsEngagedWithGroup(pVictim))
            UpdateInCombatAI_Rogue();
    }
}

void PartyBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        // Kick is worth more than anything below it. Auto attacks keep running; only the abilities
        // that would occupy the global are held, and only against a mob that is about to heal.
        if (ShouldReserveGlobalCooldownForInterrupt(pVictim))
            return;

        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            // Hold the swing until there is something to open with.
            //
            // Auto-attack is what ends stealth, and it was ending it before the opener could ever
            // be reached. AttackStart turns melee on the moment a target is picked, so the rogue
            // approached with its swing timer live and the first swing landed on arrival: stealth
            // gone, and the block below reached on a tick where the aura had already dropped. One
            // clear ran ninety two Stealths and produced a single Ambush attempt, which then failed
            // for standing in the wrong place. Ambush is worth roughly two and a half Sinister
            // Strikes and Garrote is most of a caster's opener, so this was the largest single
            // piece of rogue damage the bots were leaving on the floor.
            //
            // Melee goes back on below whatever happens, and the branch underneath turns it on for
            // any tick where the rogue is not stealthed, so there is no path that leaves it walking
            // around unable to swing.
            bool const hasOpener = m_spells.rogue.pAmbush || m_spells.rogue.pGarrote ||
                                   m_spells.rogue.pCheapShot;

            if (hasOpener && !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                if (me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                {
                    me->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                    me->SendMeleeAttackStop(pVictim);
                }

                return;
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                    {
                        me->Attack(pVictim, true);
                        return;
                    }
                }
            }
            else
            {
                // Behind, or not at all. Ambush is a positional and the server refuses it with
                // SPELL_FAILED_NOT_BEHIND, which is how the one attempt in that clear was spent.
                // Cheap Shot underneath has no such requirement, so a rogue held in front by the
                // pull-safety logic still opens with something.
                if (m_spells.rogue.pAmbush &&
                    me->IsBehindTarget(pVictim) &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                    {
                        me->Attack(pVictim, true);
                        return;
                    }
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                    {
                        me->Attack(pVictim, true);
                        return;
                    }
                }
            }

            // In range and nothing opened, so stop waiting and fight.
            me->Attack(pVictim, true);
        }
        else
        {
            // Stealth is gone, so whatever held the swing back no longer applies.
            if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                me->Attack(pVictim, true);

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

        // Combo points belong to a target, not to the rogue, so a full bar earned on the last mob
        // is worth nothing against this one. Reading the count alone said otherwise and the
        // finisher was refused with SPELL_FAILED_NO_COMBO_POINTS - twenty times in one run, each
        // one a wasted tick immediately after a target switch.
        uint32 const comboPoints = (me->GetComboTargetGuid() == pVictim->GetObjectGuid())
                                 ? me->GetComboPoints() : 0;

        // The two numbers the whole finisher decision turns on: how long the mob has left, and
        // how long this rogue needs to earn one more point.
        //
        // Every previous attempt at this picked a combo point threshold and argued about the
        // number. Five was unreachable on trash, four was unreachable too, and two spent the bar
        // on everything including bosses. There is no such number, because the right one is not a
        // property of the rogue at all: it is a property of how long the thing in front of it is
        // going to live. A Razorfen Kraul capture has both rogues at four hundred and forty four
        // engaged ticks without once reaching four points, spending forty four Eviscerates at two
        // and three - all of them from the "target is nearly dead" clause, none of them chosen -
        // and not one Slice and Dice in seventeen minutes. The thresholds were not mistuned. They
        // were answering the wrong question.
        //
        // So ask the right one, every tick: can I afford one more point and still spend it? If
        // yes, build, because Eviscerate pays its thirty five energy once however many points it
        // spends and a bigger one is strictly better. If no, spend what is in hand now, because
        // the alternative is a mob that dies with the bar still full.
        //
        // That single comparison produces both behaviours the old thresholds could not hold at the
        // same time. On a boar dying in six seconds it spends at two or three, correctly. On a
        // boss it never comes true, so the rogue builds to five, spends, and builds again - which
        // is what a full bar is for and what no fixed threshold below five would ever have done.
        // It also chains: refusing to wait at three because the mob dies in four seconds is the
        // same test that, one point and four seconds earlier, said building to three was fine.
        float const secondsToLive = EstimateSecondsToLive(pVictim);
        float const secondsPerPoint = EstimateSecondsPerComboPoint();
        bool const canAffordAnotherPoint =
            secondsToLive > (secondsPerPoint + PB_ROGUE_FINISHER_LEAD_SECONDS);

        // Slice and Dice buys attack speed for the rest of the fight and no damage at all up
        // front, so it is worth a bar only where there is a rest of the fight to buy. Below that
        // it is a finisher that did nothing, which is exactly how it beat Eviscerate to every
        // point a rogue earned for as long as it was gated on combo points alone.
        //
        // Refreshed a little early rather than on lapse. Waiting for it to fall off means the
        // haste is already gone by the time the rogue starts paying for it again, and the rebuy
        // lands at whatever happens to be in hand instead of at a bar worth spending.
        bool wantsSliceAndDice = false;
        if (m_spells.rogue.pSliceAndDice && secondsToLive > PB_ROGUE_SND_MIN_FIGHT_SECONDS)
        {
            SpellAuraHolder const* pSnd = me->GetSpellAuraHolder(m_spells.rogue.pSliceAndDice->Id);
            float const sndLeft = pSnd ? float(pSnd->GetAuraDuration()) / 1000.0f : 0.0f;

            // A negative duration is a permanent aura, which nothing puts on Slice and Dice, but
            // reading it as "about to expire" would put the rogue in a refresh loop if anything did.
            wantsSliceAndDice = pSnd
                              ? (sndLeft >= 0.0f && sndLeft < PB_ROGUE_SND_REFRESH_SECONDS)
                              : true;
        }

        // Rupture spreads its damage over a duration that scales with the points spent, so it
        // wants a fight that outlasts it and a full bar to make it with. Its duration is six
        // seconds plus two a point, and asking for comfortably longer than that rather than
        // merely longer is what keeps it off the mob that dies two ticks into it.
        float const ruptureSeconds = 6.0f + (2.0f * float(comboPoints));

        bool const spendNow =
            comboPoints >= PB_ROGUE_FINISHER_MIN_COMBO &&
           (comboPoints >= PB_ROGUE_MAX_COMBO ||        // another builder would overcap the bar
           !canAffordAnotherPoint ||                    // the points die with the mob otherwise
           (wantsSliceAndDice && comboPoints >= PB_ROGUE_SND_MIN_COMBO));

        if (spendNow)
        {
            // A finisher chosen at random was the single worst decision any rotation made. Two of
            // the four are not damage at all: bosses are immune to Kidney Shot outright, and Expose
            // Armor overwrites the warrior's Sunder Armor stacks, so a quarter of the rogue's
            // finishers were actively taking damage away from the rest of the group. It is a
            // priority list, and it always was; ordering it is the whole fix.
            SpellEntry const* pComboSpell = nullptr;

            // Slice and Dice first where the fight has earned it, because a multiplier on every
            // remaining auto attack beats any single use of the energy. The fight length test is
            // already inside wantsSliceAndDice, so by here there is a rest of the fight to buy.
            if (wantsSliceAndDice && comboPoints >= PB_ROGUE_SND_MIN_COMBO)
            {
                pComboSpell = m_spells.rogue.pSliceAndDice;
            }
            // Then Rupture on a fight long enough to run it out, at a full bar and no less: a two
            // point Rupture on a mob that dies in six seconds is the worst of both.
            else if (m_spells.rogue.pRupture &&
                     comboPoints >= PB_ROGUE_MAX_COMBO &&
                     secondsToLive > (ruptureSeconds * PB_ROGUE_RUPTURE_FIGHT_MULTIPLE) &&
                    !pVictim->HasAura(m_spells.rogue.pRupture->Id))
            {
                pComboSpell = m_spells.rogue.pRupture;
            }
            // Otherwise the damage, which is what the points were being saved for.
            else if (m_spells.rogue.pEviscerate)
            {
                pComboSpell = m_spells.rogue.pEviscerate;
            }

            if (pComboSpell && CanTryToCastSpell(pVictim, pComboSpell))
            {
                if (DoCastSpell(pVictim, pComboSpell) == SPELL_CAST_OK)
                    return;
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

        // Kick is not cast from here. It used to be, on the plain rule "the target is casting, so
        // kick it", and that rule quietly overrode the whole interrupt ranking: the driver would
        // decide a cast was not worth spending Kick on and hold it, this would fire two lines later
        // and spend it anyway. Lord Serpentis is the clean example. At 19:44:36 the rogue held Kick
        // on a priority one cast and then kicked it regardless; one second later Serpentis began
        // the sleep, the driver ranked it top priority, and the only ability the rogue owns for it
        // was on cooldown. The log says so in as many words - "none were castable".
        //
        // One owner for the ability, and it is the thing that knows what the mob can do.

        // Gouge keeps one use: getting something off the rogue when it is the one in trouble. On
        // an extra attacker, never on the group's target, and then straight back to the focus so
        // the incapacitate is not immediately undone by this rogue's own next swing.
        if (m_spells.rogue.pGouge &&
            me->GetHealthPercent() < 30.0f)
        {
            if (Unit* pExtra = SelectAttackerDifferentFrom(pVictim))
            {
                if (CanTryToCastSpell(pExtra, m_spells.rogue.pGouge))
                {
                    if (DoCastSpell(pExtra, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    {
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            // Below thirty five rather than below eighty. Eighty percent health is the ordinary
            // state of anything in melee, so the check passed almost immediately and the rogue's
            // one real defensive cooldown was spent as an opener, every fight, and was never
            // available for the fight that went wrong.
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 35.0f) &&
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

        // Backstab is refused from the front, and nothing here used to ask. It sits near the top
        // of the rogue's list, so every tick spent in front of the target began by throwing the
        // attempt away: forty seven refusals with SPELL_FAILED_NOT_BEHIND in a single run, which
        // is forty seven Sinister Strikes that never happened.
        if (m_spells.rogue.pBackstab &&
            me->IsBehindTarget(pVictim) &&
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

        // Out of melee reach is not on its own a reason to close the gap faster. Two of the ways a
        // bot gets there are cases where closing at all is the mistake: the pull rule has just
        // refused its route because walking it would wake something, or it has been told to hold
        // position. Sprint answered both with speed, and the rule then had a bot straining at a
        // leash it could not see -- until the route cleared for an instant and it arrived, alone,
        // in front of a pack the tank had not taken yet.
        //
        // The refusal is asked about rather than the distance, because the distance is the same in
        // the case Sprint is actually for: a mob that ran, or a bot that fell behind on a corridor
        // nothing objects to. Those still get it.
        //
        // The third case, and the one the others did not cover: a target nobody has pulled, with
        // the tank still behind. Sprint there is a rogue arriving first by design -- every live
        // Antu'sul approach has Stealth, Cold Blood and Sprint go out within five seconds of each
        // other while the tank is still walking -- and arriving first at an unengaged boss is the
        // pull. Declined outright rather than delayed, because a rogue that walks instead of
        // sprinting still gets there.
        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
           !m_holdPosition &&
           !IsAheadOfTankOnPull(pVictim) &&
           !me->WasPullRouteRefusedRecently(PB_SPRINT_AFTER_REFUSAL_MS) &&
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

    AbandonFillerForHealing();

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

        // Same filler the priest and shaman run, and for the same reason: with the group healthy
        // a healer reached the end of its tick having cast nothing at all. Caster form only,
        // because Moonfire and Wrath cannot be cast shapeshifted and a resto druid that has just
        // declined every heal is standing in caster form anyway.
        if (GetRole() == ROLE_HEALER && form == FORM_NONE)
        {
            if (Player* pLeader = GetPartyLeader())
                if (AddFillerDamage(SelectAttackTarget(pLeader)))
                    return;
        }

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
                // Caster forms only, and a melee druid out of form is not a caster. This sits in
                // the FORM_NONE branch, which a feral reaches whenever something has shifted it
                // out, and kiting to twenty five yards is the last thing a bot wants while it is
                // trying to get back into bear. The other melee retreat paths already draw this
                // line -- backout, breaksight and StepAwayFromHeldAttacker all refuse a melee
                // role -- and this was the one that did not.
                if (m_role == ROLE_TANK || m_role == ROLE_MELEE_DPS)
                    return;

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
