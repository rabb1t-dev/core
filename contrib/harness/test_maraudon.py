#!/usr/bin/env python3
"""Maraudon, boss by boss, driven by a five bot party at level 47.

Eight fixed bosses across four disconnected wings, so unlike Zul'Farrak this is not one event
with stages -- it is a stage per boss, each independently runnable with --only. Meshlok the
Harvester is deliberately absent: he is a rare that shares a patrol route with a Cavern Shambler
and is simply not there most of the time, and a stage that passes by being skipped is worse than
no stage. What every stage asserts beyond "the boss died" is the encounter-specific bit of
DungeonTactics that boss exists to exercise:

  vyletongue   Lord Vyletongue dies before his two Putridus Shadowstalker guards. He is the one
               of the three that can leave -- Blink is a twenty yard leap every twenty to thirty
               seconds -- so he is the one that has to die first.
  noxxion      The party kills Noxxion's Spawn rather than standing still. Every forty seconds he
               interrupts himself, goes faction 35 and unselectable, and spawns five of them for
               fifteen seconds; a party that has not been told to turn for them spends a third of
               the fight with no legal target. Sampled while the spawns are up.
  razorlash    Nothing special. He is a melee-only check that the walk through Foulspore Cavern
               left the party in a fit state to fight.
  celebras     The party stays on Celebras rather than chasing Corrupt Force of Nature. He
               summons two every twenty seconds for as long as he lives, so turning for them is a
               treadmill: the fight stalls at eighty percent with nobody dying.
  landslide    The party stays on Landslide once he is under fifty percent and the Theradrim
               Shardlings are out. They despawn when he dies, so every second on one is wasted
               twice.
  theradras    No ranged bot and no healer stands inside twenty yards of Princess Theradras while
               she is alive. Twenty is Dust Field's radius -- eight ticks of a hundred and thirty
               one to a hundred and sixty eight plus a knockback -- and the caster ladder will
               happily settle at twenty, fifteen or ten without the table's floor.
               Also: no Stolid Snapjaw is ever pulled. Twenty six neutral turtles stand around
               her approach and a stray area effect turns them into a second pull.
  rotgrip      Nothing special, in the water below her.
  gizlock      No ranged bot and no healer stands inside ten yards of Tinkerer Gizlock. That is
               the radius of Goblin Dragon Gun's cone, which fires eight times for a hundred and
               fifty seven to a hundred and ninety two a tick -- and it is where this suite
               deliberately disagrees with the published strategy, which says to stack.
  clouds       Nobody stands in a Noxious Cloud. A hundred and fifty nature damage a second for
               twenty seconds in a five yard patch, dropped by Noxious Slime as it dies -- which
               is to say underneath whoever killed it. Read with `.harness dynobj`, which exists
               for this and nothing else.

Run from the repo root on the server host.
"""

import argparse
import re
import sys
import time

from vmangos_harness import Harness

LEADER = "Harnessbot"
MARAUDON = 349

# Forty seven. Read off creature_template rather than off the instance's advertised 45-52: the
# bosses run 47 (Vyletongue) to 51 (Princess Theradras), and the trash on the way to her is a pack
# of level 48-49 Primordial Behemoths with a health multiplier of six. A party at the bottom of
# the range loses the back half of this instance on attrition rather than on any mechanic, which
# is the failure that is hardest to tell from a bug.
LEVEL = 47

# Entries, read out of creature_template on this server per the dungeon section of README.md.
NPC_THERADRIM_SHARDLING      = 11783
NPC_PUTRIDUS_SHADOWSTALKER   = 11792
NPC_PRINCESS_THERADRAS       = 12201
NPC_LANDSLIDE                = 12203
NPC_PRIMORDIAL_BEHEMOTH      = 12206
NPC_BARBED_LASHER            = 12219
NPC_NOXIOUS_SLIME            = 12221
NPC_CREEPING_SLUDGE          = 12222
NPC_CELEBRAS_THE_CURSED      = 12225
NPC_LORD_VYLETONGUE          = 12236
NPC_NOXXION                  = 13282
NPC_NOXXIONS_SPAWN           = 13456
NPC_STOLID_SNAPJAW           = 13599
NPC_ROTGRIP                  = 13596
NPC_TINKERER_GIZLOCK         = 13601
NPC_CORRUPT_FORCE_OF_NATURE  = 13743

SPELL_NOXIOUS_CLOUD          = 21070

ANY_HOSTILE = 0

# Things standing in these rooms that a clear must never try to kill. `.harness enemy 0` filters on
# faction, and Celebras the Redeemed gets through it: he is spawned invisible in Celebras the
# Cursed's room from the moment the instance loads and only becomes the quest giver once the boss
# dies. A clear that picked him up spent its entire timeout ordering the party at something that
# takes no damage, and failed the encounter standing ten yards away.
# All faction 35 -- friendly to everything, so nothing can damage them -- and all standing in
# rooms the suite clears. Celebras the Redeemed waits invisible in his own room from the moment
# the instance loads; Zaetar's Spirit stands in Princess Theradras's cavern; the two Spirits are
# the Vylestem Vine summons in Foulspore Cavern.
NEVER_PULL = {13716, 12238, 12242, 12243}

# Where each boss stands, out of the creature table for map 349, and where the party is staged for
# it. Staging is a short distance back from the boss on open ground so the approach is a pull
# rather than a teleport into melee -- `go xyz` moves the whole party at once and landing on top of
# a boss tests nothing about the fight's opening.
#
# The staging heights are the boss's own z. They are not measured floor: every one of these is the
# same room as the boss, on the same level, so the position correction has real ground within a
# yard of the height it is handed. That is the one case where `go xyz` without a ground probe is
# safe -- see the README's warning about probing, which applies to picking a *spot*, not to
# standing near something already standing there.
BOSSES = {
    "vyletongue": (NPC_LORD_VYLETONGUE,      (748.9, -219.6, -47.7)),
    "noxxion":    (NPC_NOXXION,              (1130.4, -191.3, -80.0)),
    "razorlash":  (12258,                    (978.9, -10.2, -62.5)),
    "celebras":   (NPC_CELEBRAS_THE_CURSED,  (726.1, 78.0, -86.6)),
    "landslide":  (NPC_LANDSLIDE,            (356.7, -185.5, -59.8)),
    "theradras":  (NPC_PRINCESS_THERADRAS,   (27.9, 83.2, -124.5)),
    "rotgrip":    (NPC_ROTGRIP,              (42.1, -66.0, -199.6)),
    "gizlock":    (NPC_TINKERER_GIZLOCK,     (134.9, -313.7, -173.6)),
}

# Out on the entrance map, for unbinding between runs. The Valley of Spears in Desolace, outside
# the orange door.
OUTSIDE = (-1433.0, 2954.0, 96.0, 1)

# How far back from a boss the party is staged. Far enough that the approach is a real pull and
# inside the thirty five yards a hunter opens from.
STAGE_BACK = 30.0

# Scoped to the room and not to the instance, which is the mistake the README calls out first.
# Maraudon reuses entries freely -- there are two Tinkerer Gizlock spawns, sixteen Primordial
# Behemoths and twenty four Theradrim Shardlings -- so a three hundred yard sweep finds the wrong
# copy of everything.
ROOM_RADIUS = 45.0
PACK_RADIUS = 28.0
# Two rooms need more than a pack's worth cleared, and both for a reason in the encounter rather
# than for safety. Tinkerer Gizlock puts the whole zone in combat with him on aggro; Princess
# Theradras is approached through Primordial Behemoths that arrive in pairs and are the hardest
# trash in the instance.
# Forty rather than sixty. Sixty reached the Theradrim Guardians on the path above his hollow,
# which split into Shardlings as they died and wiped the party before it ever reached him; forty
# covers the hollow itself, which is what the zone-aggro actually brings.
GIZLOCK_CLEAR_RADIUS = 40.0
THERADRAS_CLEAR_RADIUS = 50.0

BOSS_TIMEOUT = 420.0
CLEAR_TIMEOUT = 240.0
POLL = 3.0
REST_TIMEOUT = 240.0
# How long a rest waits for the fight it is following to actually end. Generous enough to cover a
# straggler running back to its spawn and short enough that a party genuinely pinned down is
# reported rather than waited on.
COMBAT_SETTLE_TIMEOUT = 90.0
REST_HEALTH = 90.0
REST_MANA = 85.0

# The two distances the table is asserted against, and both are the spell's own radius rather than
# the table's number. The table asks for twelve and twenty five; the test asks only that the bot
# is outside the thing the number exists to clear, so a later tuning of the standoff does not fail
# a suite that is really about the mechanic.
DUST_FIELD_RADIUS = 20.0
DRAGON_GUN_RADIUS = 10.0

# Melee are exempt from both, for the obvious reason: a melee bot outside the radius is a melee bot
# that is not attacking. The table only pushes ranged and healers out and this only checks them.
RANGED_ROLES = ("mage", "hunter", "priest")

COMPOSITION = ["warrior tank", "rogue melee", "mage caster", "hunter ranged", "priest healer"]

STALL_SECONDS = 45.0
MAX_FORCED_PULLS = 6


def server_uptime_seconds(harness):
    """How long the world has been up, or None. A restart is the only thing that makes it go down."""
    text = harness.run("server info", allow_failure=True)
    m = re.search(r"Server uptime:\s*(.+?)\.", text)
    if not m:
        return None
    units = {"day": 86400, "hour": 3600, "minute": 60, "second": 1}
    total = 0
    for value, word in re.findall(r"(\d+)\s*([A-Za-z]+)", m.group(1)):
        key = word.rstrip("s").lower()
        if key in units:
            total += int(value) * units[key]
    return total


def party(harness):
    info = harness.info(LEADER)
    if not info:
        return []
    return [m["name"] for m in info["members_detail"] if m.get("name") != LEADER]


# `.harness info` reports a numeric class on each member line, not a name.
CLASS_NAMES = {
    1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest",
    7: "shaman", 8: "mage", 9: "warlock", 11: "druid",
}


def classes(harness, members):
    """{name: class} off the group lines, so a stage can tell ranged from melee without guessing.

    Class rather than role because role is not reported anywhere and is not what the standoff
    turns on: BeginChasing keeps a standoff for ROLE_RANGE_DPS and ROLE_HEALER, and in this
    party's composition those are exactly the mage, the hunter and the priest.
    """
    info = harness.info(LEADER)
    if not info:
        return {}
    out = {}
    for m in info["members_detail"]:
        if m.get("name") not in members:
            continue
        out[m["name"]] = CLASS_NAMES.get(int(m.get("class", 0)), "unknown")
    return out


def standing(harness, members):
    return [m for m in members
            if int((harness.info(m) or {"deathstate": 1})["deathstate"]) == 0]


def live(harness, entries, rng):
    out = {}
    for entry in entries:
        for guid, row in harness.enemies(LEADER, entry, rng).items():
            if int(row["alive"]):
                out[guid] = row
    return out


def in_combat(harness, members):
    """Any party member currently in combat."""
    info = harness.info(LEADER)
    if not info:
        return []
    return [m["name"] for m in info["members_detail"] if int(m.get("incombat", 0))]


def rest(harness, members, label):
    """Wait out of combat until health and mana are back, or say why it could not.

    The combat check is not a nicety. Staged twenty six yards from Princess Theradras -- inside
    her aggro radius -- the party pulled her on arrival and this sat there calling it a rest for
    the full four minutes while she killed them, then handed the fight a party at one percent
    health and sixteen percent mana and reported a wipe eighteen seconds in at 98.7%. Resting
    through a fight is not resting, and the group's own `incombat` says so.
    """
    # Wait for the fight to end before trying to rest, rather than refusing outright. Refusing
    # was the first fix and it over-corrected: in Tinkerer Gizlock's hollow, where something is
    # always engaged, it meant the party took his nine pre-clear pulls back to back with no food
    # and no drink and walked into a level fifty boss tired. The bug being fixed was resting
    # *through* a fight, not declining to wait for one to finish.
    settle = time.time()
    while time.time() - settle < COMBAT_SETTLE_TIMEOUT:
        if not in_combat(harness, members):
            break
        time.sleep(5.0)
    else:
        print(f"  no rest after {label}: still in combat after "
              f"{COMBAT_SETTLE_TIMEOUT:.0f}s ({', '.join(sorted(in_combat(harness, members)))})")
        return

    start = time.time()
    worst_hp = worst_mana = 100.0
    while time.time() - start < REST_TIMEOUT:
        if in_combat(harness, members):
            print(f"  rest after {label} interrupted: the group is in combat")
            return
        worst_hp = worst_mana = 100.0
        dead = []
        for m in members:
            info = harness.info(m)
            if not info:
                continue
            if int(info["deathstate"]) != 0:
                dead.append(m)
                continue
            worst_hp = min(worst_hp, 100.0 * float(info["health"]) / float(info["maxhealth"]))
            if int(info.get("powertype", 0)) == 0 and float(info.get("maxpower", 0)) > 0:
                worst_mana = min(worst_mana, 100.0 * float(info["power"]) / float(info["maxpower"]))
        for m in dead:
            harness.run(f"harness exec {m} revive", allow_failure=True)
        if not dead and worst_hp >= REST_HEALTH and worst_mana >= REST_MANA:
            print(f"  rested after {label}: worst hp {worst_hp:.0f}%, mana {worst_mana:.0f}%")
            return
        time.sleep(5.0)
    print(f"  WARN rest after {label} timed out: hp {worst_hp:.0f}%, mana {worst_mana:.0f}%")


# Somewhere inside the instance with nothing standing on it, used to break combat between stages.
# The orange entrance landing, which is where the area trigger drops a party and is therefore the
# one patch of floor in Maraudon guaranteed to be empty.
SAFE_SPOT = (1016.83, -458.52, -43.47)


def disengage(harness, members, label):
    """Get the party out of whatever it is in before the next stage starts.

    Added after a run where the first stage failed with the party still in combat and every stage
    after it inherited that: Celebras was fought by a party already fighting something else and
    wiped at 93.5%, and the report blamed Celebras. A stage has to begin from a known state or
    its result is about the stage before it.
    """
    for m in members:
        harness.run(f"harness exec {m} revive", allow_failure=True)
    harness.execute(LEADER, "go xyz %f %f %f %d" % (SAFE_SPOT + (MARAUDON,)))
    time.sleep(8.0)

    start = time.time()
    while time.time() - start < 90.0:
        fighting = in_combat(harness, members)
        if not fighting:
            return None
        time.sleep(5.0)

    return f"could not break combat before {label}: {sorted(in_combat(harness, members))}"


def dismiss(harness):
    harness.run(f"harness exec {LEADER} partybot removeall", allow_failure=True)
    time.sleep(2.0)
    for name in party(harness):
        harness.run(f"harness exec {name} partybot remove", allow_failure=True)
    time.sleep(2.0)


def progress_fingerprint(harness, entries, rng):
    """Something died, or something lost a real chunk of health.

    Health bucketed to ten percent so that a disengaged mob regenerating does not read as the
    fight progressing, which is the mistake that made the Zul'Farrak version of this almost never
    fire.
    """
    rows = []
    for entry in entries:
        for guid, row in harness.enemies(LEADER, entry, rng).items():
            if int(row["alive"]):
                rows.append((guid, int(float(row["percent"]) // 10)))
    return (len(rows), tuple(sorted(rows)))


def clear_pack(harness, members, rng=PACK_RADIUS, limit=14, skip=(), watch=None):
    """Pull whatever is standing here, nearest first, one at a time, resting between.

    Entry zero rather than a list, per the README: Maraudon's rooms hold entries no hand-written
    list would have. Noxxion's pool has Vile Larva and Creeping Sludge mixed in with the elites,
    and Earth Song Falls adds a Theradrim Shardling to every Guardian that dies.

    `watch` is called on every poll and is not optional decoration. A boss standing in the room
    being cleared is in the fight whether the clear wanted it or not -- Lord Vyletongue joined the
    second trash pull and died in it -- so a stage that only starts watching once clear_pack
    returns has already missed the thing it is measuring.
    """
    cleared = 0
    skipped = set(NEVER_PULL)
    while cleared < limit:
        rows = {g: r for g, r in harness.enemies(LEADER, ANY_HOSTILE, rng).items()
                if int(r["alive"])
                and int(r["entry"]) not in skip
                and int(r["entry"]) not in skipped}
        if not rows:
            return None, cleared

        guid, row = min(rows.items(), key=lambda kv: float(kv[1]["dist"]))
        entry = int(row["entry"])
        name = row.get("name", str(entry))

        start = time.time()
        last_pct = None
        last_change = time.time()
        killed = False
        while time.time() - start < CLEAR_TIMEOUT:
            still = harness.enemies(LEADER, entry, rng).get(int(guid))
            if still is None or not int(still["alive"]):
                killed = True
                break
            # Re-issued every poll rather than latched, which is the single most misleading bug
            # the README records: a party that disengaged is a party nothing ever re-orders.
            if harness.select(LEADER, entry, rng) is not None:
                harness.execute(LEADER, "partybot attackstart")
            if watch:
                watch()
            if not standing(harness, members):
                return f"the party wiped clearing {name} after {cleared} pulls", cleared

            pct = int(float(still["percent"]) // 10)
            if pct != last_pct:
                last_pct = pct
                last_change = time.time()
            elif time.time() - last_change > STALL_SECONDS:
                # Not a failure of the stage, just of this target. A room holds things a clear
                # cannot kill and should not be trying to: Celebras the Redeemed stands in his
                # own room from the moment the instance loads, is faction 35 and takes no damage,
                # and a clear that treated him as a pull spent its whole timeout on him and
                # failed the encounter that was standing right there. Drop him and move on.
                print(f"  giving up on {name} (entry {entry}): no progress in {STALL_SECONDS:.0f}s")
                skipped.add(entry)
                break
            time.sleep(POLL)
        else:
            print(f"  giving up on {name} (entry {entry}): not dead inside {CLEAR_TIMEOUT:.0f}s")
            skipped.add(entry)

        if not killed:
            continue

        cleared += 1
        rest(harness, members, f"pull {cleared}")

    return None, cleared


# The bearings a stage will try, and the shortfall it will accept from the mesh.
#
# Seeing the boss is not the same as being able to walk to him, and the difference cost a run.
# The first version of this backed off thirty yards along -x and settled for the boss being in
# range: at Noxxion that put the party on the wrong side of his pool wall, where every bot could
# see him, every bot was ordered onto him, and the tank stood at 28.6 yards with moving=1, an
# unchanging position and dmg=0 for the whole four hundred and twenty seconds. From outside that
# is indistinguishable from a party refusing to fight.
#
# So ask the navigation mesh, which is what `.harness path` is for: teleport to the candidate, ask
# for a route to the boss, and require a route that actually arrives. Detour degrades quietly --
# it answers an unreachable destination with a partial route or a straight line rather than a
# failure -- so the shortfall is what has to be read, not the fact that a path came back.
STAGE_BEARINGS = 8
# Two yards. Enough to absorb the boss's own bounding radius and the mesh's last polygon, and far
# short of the thirty yards a straight-line answer falls short by.
STAGE_MAX_SHORTFALL = 2.0
# What disqualifies a route, read as flag names rather than as a value because the interesting
# answers are combinations. NOPATH is the one that matters here and it is the one that would have
# been missed by reading `shortfall` alone: a refused route still comes back as two points from A
# to B with a shortfall of 0.0, because Detour reports the straight line it drew instead. Measured
# at Noxxion, eight bearings at thirty yards: 45, 90, 135, 180 and 225 degrees are all NOPATH and
# every one of them reports shortfall 0.0, while the three that work report 0.1.
BAD_PATH_FLAGS = ("NOPATH", "NOT_USING_PATH", "INCOMPLETE", "SHORTCUT", "BLANK")


def stage_at(harness, boss_entry, position):
    """Put the party on ground a pull's length back from the boss, that can actually reach him.

    Tries bearings around him in turn. A candidate has to satisfy two things: the boss is in
    range from it, and the mesh answers a route to him that arrives. Returns the boss's live rows,
    or an empty dict if no bearing works.
    """
    import math

    x, y, z = position
    best_empty = {}

    for i in range(STAGE_BEARINGS):
        angle = 2.0 * math.pi * i / STAGE_BEARINGS
        sx = x + math.cos(angle) * STAGE_BACK
        sy = y + math.sin(angle) * STAGE_BACK

        harness.execute(LEADER, "go xyz %f %f %f %d" % (sx, sy, z, MARAUDON))
        time.sleep(6.0)

        rows = harness.enemies(LEADER, boss_entry, ROOM_RADIUS + STAGE_BACK)
        alive = {g: r for g, r in rows.items() if int(r["alive"])}
        if not alive:
            continue
        best_empty = alive

        row = next(iter(alive.values()))
        try:
            route = harness.path(LEADER, float(row["x"]), float(row["y"]), float(row["z"]))
        except Exception:
            continue

        shortfall = float(route.get("shortfall", 999.0))
        kind = route.get("type", "")
        bad = any(flag in kind for flag in BAD_PATH_FLAGS) or "NORMAL" not in kind

        if shortfall <= STAGE_MAX_SHORTFALL and not bad:
            print(f"  staged at bearing {i * 360 // STAGE_BEARINGS} deg, "
                  f"{float(row['dist']):.0f}y out, mesh shortfall {shortfall:.1f}y")
            return alive

        print(f"  bearing {i * 360 // STAGE_BEARINGS} deg rejected: "
              f"shortfall {shortfall:.1f}y type={kind}")

    # Every bearing was refused. Report the boss anyway if one was ever in range, so the stage
    # fails on the fight rather than on the staging -- but say so, because "no reachable bearing"
    # and "no boss" are different problems and only one of them is the bots'.
    if best_empty:
        print("  WARN: no bearing had a clean route to the boss; staging on the last one that "
              "could see him")
    return best_empty


def ranged_too_close(harness, members, roles, boss_row, radius):
    """Ranged bots and healers standing inside a radius the table is supposed to keep them out of.

    Measured from the boss's position rather than from `dist`, which is the distance to the
    *leader*. The leader stands wherever the stage left it and is not what the standoff is
    measured against.
    """
    bx, by = float(boss_row["x"]), float(boss_row["y"])
    close = []
    for m in members:
        if roles.get(m) not in RANGED_ROLES:
            continue
        info = harness.info(m)
        if not info or int(info["deathstate"]) != 0 or int(info["map"]) != MARAUDON:
            continue
        dx = float(info["x"]) - bx
        dy = float(info["y"]) - by
        d = (dx * dx + dy * dy) ** 0.5
        if d < radius:
            close.append(f"{m}@{d:.0f}y")
    return close


def bots_in_clouds(harness):
    """Party members standing in a hostile Noxious Cloud right now."""
    out = set()
    for row in harness.dynobjs(LEADER, SPELL_NOXIOUS_CLOUD, 60.0).values():
        if not int(row.get("hostile", 0)):
            continue
        out.update(row["inside"])
    return out


def fight_boss(harness, members, roles, boss_entry, watch, seen_alive=False,
               also_count=()):
    """Hold a boss until it dies, sampling whatever this encounter is really about.

    `watch` is a callable given (elapsed, boss_row) on every poll; it records into its own closure.
    Returns None or a sentence.

    `also_count` are entries whose health counts as progress alongside the boss's. Noxxion is why:
    for fifteen seconds out of every forty his health does not move because he cannot be attacked,
    and a fingerprint taken on his entry alone reads that as a stalled fight while the party is
    busy killing the five spawns that made him untouchable.

    `seen_alive` is the witness the *stage* already has and this function cannot get for itself,
    and leaving it out cost a whole run. Lord Vyletongue stands among the pack his room is cleared
    of; the party pulled him with it and killed him, so by the time this was called he was a corpse
    ten yards away. With nothing alive to see, the "it died" branch was never armed, neither branch
    fired, and the loop sat out its full four hundred and twenty seconds before reporting "the boss
    was not down" about a boss that was down. Seeded from stage_at, which witnessed him alive.
    """
    # Walk the party in before ordering the attack, so the fight happens where the boss stands.
    #
    # Staging thirty yards back is right for the pull and wrong for the fight, and Princess
    # Theradras is where that cost a run. The party pulled her, she walked the thirty yards out
    # to them, they held her at the staging point and took her to seventy four percent -- and
    # then she hit her leash and reset to a hundred, standing in the same spot. Three times over
    # two hundred and twenty four seconds, which reported as "wiped with the boss at 97.7%"
    # about a boss the party had damaged three times.
    #
    # Nothing walked them back in afterwards either, which is worth knowing separately: the melee
    # logged the identical position for twenty seconds across the reset. Re-ordering the attack
    # every poll is not enough when the target has evaded.
    rows = harness.enemies(LEADER, boss_entry, ROOM_RADIUS + STAGE_BACK)
    alive = {g: r for g, r in rows.items() if int(r["alive"])}
    if alive:
        row = next(iter(alive.values()))
        harness.execute(LEADER, "go xyz %f %f %f %d" % (
            float(row["x"]), float(row["y"]), float(row["z"]), MARAUDON))
        time.sleep(8.0)

    start = time.time()
    last_shape = None
    last_change = time.time()
    forced = 0
    home = None

    while time.time() - start < BOSS_TIMEOUT:
        elapsed = round(time.time() - start)
        rows = harness.enemies(LEADER, boss_entry, ROOM_RADIUS + STAGE_BACK)
        alive = {g: r for g, r in rows.items() if int(r["alive"])}

        if alive:
            seen_alive = True
            row = next(iter(alive.values()))
            watch(elapsed, row)

            # Keep the party with the boss. A boss that leashes walks home and the party does not
            # follow, so without this the fight is over and nothing says so.
            if home is None:
                home = (float(row["x"]), float(row["y"]), float(row["z"]))
            drift = ((float(row["x"]) - home[0]) ** 2 +
                     (float(row["y"]) - home[1]) ** 2) ** 0.5
            if drift > 15.0 or float(row["dist"]) > 25.0:
                harness.execute(LEADER, "go xyz %f %f %f %d" % (
                    float(row["x"]), float(row["y"]), float(row["z"]), MARAUDON))
                home = (float(row["x"]), float(row["y"]), float(row["z"]))
                time.sleep(4.0)
        elif seen_alive:
            # Witnessed, per the README: the boss was seen alive here and is now on the grid dead.
            # A boss that was never seen is "unknown", not "killed".
            if rows:
                return None

            # Gone from the room search, which for a patrolling boss means "swam off", not
            # "died". Rotgrip has six waypoints through the water of Zaetar's Cave and left the
            # seventy five yard sweep three seconds into his stage; this reported him as having
            # left the grid rather than following him. Ask the whole instance before believing
            # it, and if he is out there, walk the party to him.
            wide = harness.enemies(LEADER, boss_entry, 300.0)
            wide_alive = {g: r for g, r in wide.items() if int(r["alive"])}
            if wide_alive:
                row = next(iter(wide_alive.values()))
                print(f"  boss wandered to {float(row['dist']):.0f}y; following")
                harness.execute(LEADER, "go xyz %f %f %f %d" % (
                    float(row["x"]), float(row["y"]), float(row["z"]), MARAUDON))
                time.sleep(8.0)
                continue
            if wide:
                return None
            return f"the boss left the grid after {elapsed}s rather than dying on it"

        if not standing(harness, members):
            pct = next(iter(alive.values()))["percent"] if alive else "?"
            return f"the party wiped {elapsed}s in with the boss at {pct}%"

        # Re-issued every poll. Against Noxxion this is not a nicety: for fifteen seconds out of
        # every forty he is not a legal target at all, and the order has to be re-placed on the
        # spawns and then back on him.
        target = boss_entry if alive else ANY_HOSTILE
        if harness.select(LEADER, target, ROOM_RADIUS + STAGE_BACK) is not None:
            harness.execute(LEADER, "partybot attackstart")

        shape = progress_fingerprint(harness, [boss_entry] + list(also_count),
                                     ROOM_RADIUS + STAGE_BACK)
        if shape != last_shape:
            last_shape = shape
            last_change = time.time()
        elif shape and time.time() - last_change > STALL_SECONDS and forced < MAX_FORCED_PULLS:
            if alive:
                row = next(iter(alive.values()))
                harness.execute(LEADER, "go xyz %f %f %f %d" % (
                    float(row["x"]), float(row["y"]), float(row["z"]), MARAUDON))
                forced += 1
                last_change = time.time()
                print(f"  STALL: walked the leader onto the boss (#{forced})")
        time.sleep(POLL)

    return f"the boss was not down inside {BOSS_TIMEOUT:.0f}s"


# -- the stages ------------------------------------------------------------


def stage_simple(harness, members, roles, key, clear_radius=PACK_RADIUS):
    entry, position = BOSSES[key]
    if not stage_at(harness, entry, position):
        return f"{key}: nothing of entry {entry} alive in the room"
    err, cleared = clear_pack(harness, members, rng=clear_radius, skip=(entry,))
    if err:
        return f"{key}: {err}"
    print(f"  cleared {cleared} around {key}")
    rest(harness, members, f"the walk to {key}")
    return fight_boss(harness, members, roles, entry, lambda *_: None, seen_alive=True)


def stage_vyletongue(harness, members, roles):
    entry, position = BOSSES["vyletongue"]
    if not stage_at(harness, entry, position):
        return "vyletongue: he is not in his room"

    deaths = []
    seen = {"Vyletongue"}

    def note_deaths(*_):
        for e, name in ((entry, "Vyletongue"), (NPC_PUTRIDUS_SHADOWSTALKER, "Shadowstalker")):
            rows = harness.enemies(LEADER, e, ROOM_RADIUS)
            if any(int(r["alive"]) for r in rows.values()):
                seen.add(name)
            elif name in seen and name not in deaths:
                deaths.append(name)

    # His two guards are deliberately not cleared first: the point of the stage is the kill order
    # between him and them, and clearing them beforehand removes the thing being tested. The
    # watcher runs through the clear as well as the fight, because he does not wait to be pulled --
    # the measured run had him join the second trash pull and die in it, and a watcher that only
    # started afterwards recorded nothing at all.
    err, cleared = clear_pack(harness, members,
                              skip=(entry, NPC_PUTRIDUS_SHADOWSTALKER),
                              watch=note_deaths)
    if err:
        return f"vyletongue: {err}"
    print(f"  cleared {cleared} around Lord Vyletongue, leaving him and his guards")
    rest(harness, members, "the walk to Lord Vyletongue")
    note_deaths()

    err = fight_boss(harness, members, roles, entry, note_deaths, seen_alive=True)
    if err:
        return f"vyletongue: {err}"

    print(f"  died in order: {deaths}")
    if "Shadowstalker" in deaths and "Vyletongue" in deaths:
        if deaths.index("Shadowstalker") < deaths.index("Vyletongue"):
            return ("vyletongue: both guards died before he did, so the party chased the two "
                    "things that cannot run from the one that can")
    return None


def stage_noxxion(harness, members, roles):
    entry, position = BOSSES["noxxion"]
    if not stage_at(harness, entry, position):
        return "noxxion: he is not in his pool"
    err, cleared = clear_pack(harness, members, skip=(entry,))
    if err:
        return f"noxxion: {err}"
    print(f"  cleared {cleared} around Noxxion")
    rest(harness, members, "the walk to Noxxion")

    samples = {"spawns_seen": 0, "on_spawns": 0, "idle": 0}

    def watch(elapsed, row):
        spawns = live(harness, [NPC_NOXXIONS_SPAWN], ROOM_RADIUS)
        if not spawns:
            return
        samples["spawns_seen"] += 1
        attacked = sum(int(r.get("attackers", 0)) for r in spawns.values())
        if attacked:
            samples["on_spawns"] += 1
        else:
            samples["idle"] += 1

    err = fight_boss(harness, members, roles, entry, watch, seen_alive=True,
                     also_count=(NPC_NOXXIONS_SPAWN,))
    if err:
        return f"noxxion: {err}"

    print(f"  spawn samples: {samples['spawns_seen']} with spawns up, "
          f"{samples['on_spawns']} of them with the party on one")
    if samples["spawns_seen"] == 0:
        print("  WARN: never sampled a split, so the spawn tactic was not exercised")
    elif samples["on_spawns"] == 0:
        return ("noxxion: the spawns were up for %d samples and the party never touched one, "
                "which is the whole of that fight" % samples["spawns_seen"])
    return None


def stage_stay_on_boss(harness, members, roles, key, add_entry, add_name,
                       below_percent=100.0):
    """A boss whose summons the party is told to walk past.

    Samples only while an add is actually alive and the boss is under `below_percent`, because
    above that there is no add yet and the assertion would be about nothing.
    """
    entry, position = BOSSES[key]
    if not stage_at(harness, entry, position):
        return f"{key}: not in the room"
    err, cleared = clear_pack(harness, members, skip=(entry, add_entry))
    if err:
        return f"{key}: {err}"
    print(f"  cleared {cleared} around {key}")
    rest(harness, members, f"the walk to {key}")

    samples = {"with_adds": 0, "on_adds": 0, "on_boss": 0}

    def watch(elapsed, row):
        if float(row["percent"]) > below_percent:
            return
        adds = live(harness, [add_entry], ROOM_RADIUS)
        if not adds:
            return
        samples["with_adds"] += 1
        on_adds = sum(int(r.get("attackers", 0)) for r in adds.values())
        on_boss = int(row.get("attackers", 0))
        if on_boss >= on_adds:
            samples["on_boss"] += 1
        else:
            samples["on_adds"] += 1

    err = fight_boss(harness, members, roles, entry, watch, seen_alive=True,
                     also_count=(add_entry,))
    if err:
        return f"{key}: {err}"

    print(f"  {add_name} samples: {samples['with_adds']} with adds up, "
          f"{samples['on_boss']} of them with most of the party still on the boss")
    if samples["with_adds"] == 0:
        print(f"  WARN: never sampled a live {add_name}, so the burn gate was not exercised")
    elif samples["on_boss"] * 2 < samples["with_adds"]:
        return (f"{key}: the party spent {samples['on_adds']} of {samples['with_adds']} samples "
                f"on {add_name} rather than on the boss, which is the treadmill the burn gate "
                f"exists to refuse")
    return None


def stage_theradras(harness, members, roles):
    entry, position = BOSSES["theradras"]
    if not stage_at(harness, entry, position):
        return "theradras: she is not in her cavern"

    # The Behemoths on the approach are cleared; the turtles are not touched, and whether they
    # stay untouched is half of what this stage is for.
    err, cleared = clear_pack(harness, members, rng=THERADRAS_CLEAR_RADIUS, limit=20,
                              skip=(entry, NPC_STOLID_SNAPJAW))
    if err:
        return f"theradras: {err}"
    print(f"  cleared {cleared} around Princess Theradras")
    rest(harness, members, "the walk to Princess Theradras")

    turtles_before = len(live(harness, [NPC_STOLID_SNAPJAW], ROOM_RADIUS + STAGE_BACK))
    breaches = []
    turtle_pulls = []

    def watch(elapsed, row):
        close = ranged_too_close(harness, members, roles, row, DUST_FIELD_RADIUS)
        if close:
            breaches.append((elapsed, close))
        pulled = [r.get("name", "turtle")
                  for r in live(harness, [NPC_STOLID_SNAPJAW], ROOM_RADIUS + STAGE_BACK).values()
                  if int(r.get("incombat", 0))]
        if pulled:
            turtle_pulls.append((elapsed, len(pulled)))

    err = fight_boss(harness, members, roles, entry, watch, seen_alive=True)
    if err:
        return f"theradras: {err}"

    turtles_after = len(live(harness, [NPC_STOLID_SNAPJAW], ROOM_RADIUS + STAGE_BACK))
    print(f"  turtles: {turtles_before} before, {turtles_after} after")

    problems = []
    if breaches:
        problems.append(f"ranged inside Dust Field ({DUST_FIELD_RADIUS:.0f}y) at {breaches[:5]}")
    if turtle_pulls:
        problems.append(f"Stolid Snapjaws pulled at {turtle_pulls[:5]}")
    if turtles_after < turtles_before:
        problems.append(f"{turtles_before - turtles_after} turtles were killed")
    return ("theradras: " + "; ".join(problems)) if problems else None


def stage_gizlock(harness, members, roles):
    entry, position = BOSSES["gizlock"]
    if not stage_at(harness, entry, position):
        return "gizlock: he is not in his hollow"

    # A wide clear, and it is the encounter that demands it rather than caution. His EventAI runs
    # SCRIPT_COMMAND_SET_IN_COMBAT_WITH_ZONE on aggro, so pulling him pulls everything in the
    # zone with him -- which in his hollow means the linked Subterranean Diemetradon groups. The
    # first measured attempt cleared twenty eight yards, engaged, and spent the fight with the
    # mage at nine percent being chewed by a Diemetradon; the party reached twenty eight percent
    # on Gizlock and wiped. That is the guide's "make sure all mobs and patrols are clear before
    # engaging" expressed as a number.
    err, cleared = clear_pack(harness, members, rng=GIZLOCK_CLEAR_RADIUS, limit=20,
                              skip=(entry,))
    if err:
        return f"gizlock: {err}"
    print(f"  cleared {cleared} around Tinkerer Gizlock")
    rest(harness, members, "the walk to Tinkerer Gizlock")

    breaches = []

    def watch(elapsed, row):
        close = ranged_too_close(harness, members, roles, row, DRAGON_GUN_RADIUS)
        if close:
            breaches.append((elapsed, close))

    err = fight_boss(harness, members, roles, entry, watch, seen_alive=True)
    if err:
        return f"gizlock: {err}"

    if breaches:
        return (f"gizlock: ranged inside the Goblin Dragon Gun cone "
                f"({DRAGON_GUN_RADIUS:.0f}y) at {breaches[:5]}")
    return None


# A Noxious Slime camp in Foulspore Cavern, four of them within six yards of each other, read out
# of the creature table rather than found by wandering. The clouds stage has to stand somewhere
# specific: run with --only it starts from outside the instance, and a three hundred yard search
# from there finds nothing at all.
SLIME_CAMP = (874.0, -131.8, -87.0)


def stage_clouds(harness, members, roles):
    """Kill a Noxious Slime and watch nobody stand in what it leaves behind.

    The Slime is chosen over the Sludge deliberately: it casts the cloud on death, which puts the
    patch under whoever was in melee at the moment the fight ended and the party had no reason to
    move. That is the case the rule exists for and the case a timed drop would not reproduce.
    """
    harness.execute(LEADER, "go xyz %f %f %f %d" % (
        SLIME_CAMP[0] - STAGE_BACK, SLIME_CAMP[1], SLIME_CAMP[2], MARAUDON))
    time.sleep(10.0)

    slimes = harness.enemies(LEADER, NPC_NOXIOUS_SLIME, 300.0)
    alive = {g: r for g, r in slimes.items() if int(r["alive"])}
    if not alive:
        return "clouds: no Noxious Slime alive anywhere near the party"

    guid, row = min(alive.items(), key=lambda kv: float(kv[1]["dist"]))
    harness.execute(LEADER, "go xyz %f %f %f %d" % (
        float(row["x"]), float(row["y"]) - 20.0, float(row["z"]), MARAUDON))
    time.sleep(10.0)

    err, cleared = clear_pack(harness, members, limit=1)
    if err:
        return f"clouds: {err}"
    if not cleared:
        return "clouds: nothing was pulled, so no cloud was ever dropped"

    # Twenty seconds is the cloud's whole life. Sampled for twenty five so that a bot which
    # stepped out and drifted back in is still caught.
    start = time.time()
    seen_cloud = False
    standers = []
    while time.time() - start < 25.0:
        clouds = harness.dynobjs(LEADER, SPELL_NOXIOUS_CLOUD, 60.0)
        hostile = {g: r for g, r in clouds.items() if int(r.get("hostile", 0))}
        if hostile:
            seen_cloud = True
            inside = bots_in_clouds(harness)
            if inside:
                standers.append((round(time.time() - start), sorted(inside)))
        time.sleep(1.5)

    if not seen_cloud:
        print("  WARN: the Slime died without leaving a cloud, so the rule was not exercised")
        return None

    # One sample is the tick between the cloud appearing and the bot setting off, which is not a
    # failure -- the rule cannot act before the patch exists. Two consecutive seconds of standing
    # in it is.
    print(f"  cloud samples with somebody inside: {len(standers)}")
    if len(standers) > 2:
        return (f"clouds: bots stood in a Noxious Cloud for {len(standers)} samples: "
                f"{standers[:5]}")
    return None


STAGES = [
    ("vyletongue", stage_vyletongue),
    ("noxxion",    stage_noxxion),
    ("razorlash",  lambda h, m, r: stage_simple(h, m, r, "razorlash")),
    ("clouds",     stage_clouds),
    ("celebras",   lambda h, m, r: stage_stay_on_boss(
        h, m, r, "celebras", NPC_CORRUPT_FORCE_OF_NATURE, "Corrupt Force of Nature")),
    ("landslide",  lambda h, m, r: stage_stay_on_boss(
        h, m, r, "landslide", NPC_THERADRIM_SHARDLING, "Theradrim Shardling",
        below_percent=50.0)),
    ("theradras",  stage_theradras),
    ("rotgrip",    lambda h, m, r: stage_simple(h, m, r, "rotgrip")),
    ("gizlock",    stage_gizlock),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", default=[],
                        choices=[name for name, _ in STAGES],
                        help="run just these stages, repeatable")
    parser.add_argument("--level", type=int, default=LEVEL)
    parser.add_argument("--keep", action="store_true",
                        help="do not unbind the instance first, so a stage can be re-run "
                             "against a copy that is already part cleared")
    args = parser.parse_args()

    wanted = [(name, fn) for name, fn in STAGES if not args.only or name in args.only]

    harness = Harness.from_env()
    harness.login(LEADER)
    harness.run(f"harness exec {LEADER} character level {LEADER} {args.level}",
                allow_failure=True)
    # `character level` takes the character out of the world briefly, so a revive straight after
    # it fails with "Player not found". Tolerated and re-logged rather than raced.
    for _ in range(20):
        if harness.info(LEADER):
            break
        time.sleep(1.0)
        try:
            harness.login(LEADER)
        except Exception:
            pass
    harness.run(f"harness exec {LEADER} revive", allow_failure=True)
    dismiss(harness)

    if not args.keep:
        harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)
        time.sleep(3.0)
        harness.execute(LEADER, "instance unbind all")
        time.sleep(2.0)

    # Formed out on the entrance map rather than where the leader was standing. A leader still
    # inside a five man counts against that instance's player cap, and the reported symptom is
    # "only 4 of 5 bots joined".
    print(f"forming: {', '.join(COMPOSITION)} at level {args.level}")
    for spec in COMPOSITION:
        harness.execute(LEADER, f"partybot add {spec} {args.level}")
        time.sleep(1.5)
    time.sleep(4.0)

    uptime_before = server_uptime_seconds(harness)

    members = party(harness)
    if len(members) != len(COMPOSITION):
        print(f"FAIL setup: expected {len(COMPOSITION)} bots, got {sorted(members)}")
        dismiss(harness)
        return 1
    roles = classes(harness, members)
    print(f"party: {sorted(members)}")
    print(f"roles: {roles}")

    failures = []
    for name, fn in wanted:
        print(f"== {name}")
        stuck = disengage(harness, members, name)
        if stuck:
            print(f"  WARN {stuck}")
        error = fn(harness, members, roles)
        if error:
            print(f"FAIL {error}")
            failures.append(error)
            # Carry on to the next boss rather than stopping. Each stage stages itself, so one
            # lost fight does not invalidate the seven that follow, and one run that reports
            # eight results is worth more than eight runs that report one.
            for m in members:
                harness.run(f"harness exec {m} revive", allow_failure=True)
        else:
            print(f"PASS {name}")
        rest(harness, members, name)

    uptime_after = server_uptime_seconds(harness)
    if (uptime_after is not None and uptime_before is not None
            and uptime_after < uptime_before):
        print(f"VOID: the world restarted during this run (uptime {uptime_before}s -> "
              f"{uptime_after}s). Nothing above is a result.")
        dismiss(harness)
        return 2

    alive = standing(harness, members)
    dismiss(harness)
    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)

    if failures:
        print(f"FAILED {len(failures)}/{len(wanted)} stages")
        return 1

    print(f"PASS: {len(wanted)} Maraudon stages, {len(alive)}/{len(members)} bots standing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
