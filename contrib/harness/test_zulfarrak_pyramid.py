#!/usr/bin/env python3
"""Zul'Farrak's pyramid event, driven end to end by a five bot party.

The event is the reason this file exists. Five Troll Cages are opened at the top of the pyramid,
which frees Sergeant Bly and his four; fifty four Sandfury trolls are then summoned at the foot of
the stairs in three waves and released up them a handful at a time, with each wave gated on all of
it being dead. The third wave brings Nekrum Gutchewer and Shadowpriest Sezz'ziz. When the last
troll dies Bly's crew turn on the group, and that fight is at the bottom of the stairs.

Written as named stages, because "the bots lost" covers a spawn bug, a pacing bug, a bot that
walked down the stairs and a fight that was simply too hard, and those want different fixes.

What each stage is actually asserting, beyond nobody dying:

  cages       Opening all five cages spawns ONE wave, not five. This is the regression that made
              the event unwinnable: every cage drove the instance phase back to CAGES_OPEN and
              re-armed Weegli's arrival, and each arrival summoned another twenty three trolls.
  pacing      The number of trolls that have climbed to the landing stays inside the release cap
              rather than arriving all at once. Sampled throughout the fight, not at the end.
  holdline    No bot ever stands below the top step while the event is running. This is the rule
              that keeps a bot from walking down to meet a troll and arriving among the forty that
              have not been released.
  killorder   When Nekrum and Sezz'ziz are both up, the party is on Nekrum.
  bly         Oro and Murta die before Bly does.

Run from the repo root on the server host.
"""

import re
import sys
import time

from vmangos_harness import Harness

LEADER = "Harnessbot"
ZF = 209
# Forty nine, not forty six. The pyramid event is the back half of Zul'Farrak -- Nekrum is 45-46,
# Sezz'ziz 47 and Chief Ukorz 48 -- and the instance's own range runs to the low fifties. A level
# forty six party is under-levelled for it, and the measured run says so: it cleared wave one
# comfortably, ground down through wave two, and wiped at four hundred and fourteen seconds with
# thirty eight of forty six trolls killed. That is a party losing on attrition over a seven minute
# fight, not one losing to a mechanic.
LEVEL = 49

# Read out of creature_template on this server rather than from a guide, per the dungeon section of
# contrib/harness/README.md. Nekrum is 7796 and not 7274; that was worth checking, because the
# tactics table orders the third wave around it.
NPC_SANDFURY_SLAVE = 7787
NPC_SANDFURY_DRUDGE = 7788
NPC_SANDFURY_CRETIN = 7789
NPC_SANDFURY_ACOLYTE = 8876
NPC_SANDFURY_ZEALOT = 8877
WAVE_TROLLS = [NPC_SANDFURY_SLAVE, NPC_SANDFURY_DRUDGE, NPC_SANDFURY_CRETIN,
               NPC_SANDFURY_ACOLYTE, NPC_SANDFURY_ZEALOT]

NPC_SEZZZIZ = 7275          # Heal 12039 on 14-25s, Renew, Psychic Scream 13704 on 22-32s
NPC_NEKRUM = 7796           # Fevered Plague only. No heal, nothing that undoes being focused
NPC_BLY = 7604
NPC_RAVEN = 7605
NPC_ORO = 7606              # Rain of Fire, Immolate, Curse of Weakness
NPC_WEEGLI = 7607
NPC_MURTA = 7608            # Heal 11642 / Renew 11640 on friendlies within 40y, EventAI not spells
BLY_CREW = [NPC_BLY, NPC_RAVEN, NPC_ORO, NPC_WEEGLI, NPC_MURTA]

# The five Troll Cages, read out of the gameobject table for map 209. Each is opened in turn, which
# is the point: before the fix, cages two through five each re-spawned wave one.
CAGES = [
    (141071, 1881.33, 1297.46, 48.3304),
    (141074, 1883.33, 1298.99, 48.2929),
    (141073, 1886.88, 1298.94, 48.2268),
    (141072, 1889.76, 1297.68, 48.1727),
    (141070, 1890.96, 1294.47, 48.1535),
]

# The landing at the top of the stairs, which is the ground the party holds. Matches the hold line
# in DungeonTactics.cpp: centre (1885, 1270, 42), radius 25, floor 39.5.
LANDING = (1885.0, 1270.0, 42.0)
HOLD_FLOOR = 39.5
# Anything below this is on the stairway or the courtyard, which is where a bot must not be.
STAIR_TOP_Z = 39.5
# How close to the leader, who is standing in the middle of the landing, counts as having made it
# up the stairs. The hold radius, so this and the bot rule are measuring the same patch of ground.
UP_THE_STAIRS_DIST = 25.0

# The foot of the stairs, where wave three is spawned and Bly's crew are sent to meet it.
#
# Measured with `.harness ground` over x 1880..1890 by y 1198..1246 at six yard spacing, not
# guessed. The first version of this said (1885, 1240, 11.0) and the floor at y 1240 is actually
# at 19.99 -- it is still the staircase there, and the descent was teleporting the whole party
# nine yards inside it. That is why run eight left the tank stranded at z 16 and every target in
# the fight sitting at a hundred percent health: nothing could path anywhere.
#
# y 1228 is the first ground past the bottom step, at 9.4 to 9.85. The instance script agrees --
# its own PRE_WAVE_3 sends Bly to (1887.92, 1228.18, 9.98) -- which is the cross-check that the
# measurement found the floor and not something under it.
FOOT_OF_STAIRS = (1885.0, 1228.0, 9.9, ZF)
# Where the crew end up after the event, a little further into the courtyard.
BLY_GROUND = (1885.0, 1210.0, 8.9, ZF)

OUTSIDE = (-6835.0, -2915.0, 8.9, 1)

STAGE_TIMEOUT = 900.0
EVENT_POLL = 4.0
REST_TIMEOUT = 180.0
REST_HEALTH = 90.0
REST_MANA = 80.0

# Scoped to the pyramid rather than the instance. Zul'Farrak reuses these entries in the courtyard
# packs on the way in, so a 300 yard search finds trolls that have nothing to do with the event.
EVENT_RADIUS = 90.0
CREW_RADIUS = 60.0

COMPOSITION = ["warrior tank", "rogue melee", "mage caster", "hunter ranged", "priest healer"]

# What the instance script allows up the stairs at once: PYRAMID_MAX_RELEASED_ALIVE. Sampled with a
# margin, because a sample can land between a release and the first kill, and because a troll that
# has just died is still on the grid for a moment.
RELEASE_CAP = 6
RELEASE_CAP_MARGIN = 3


def server_uptime_seconds(harness):
    """How long the world has been up, in seconds, or None if it cannot be read.

    `server info` reports an uptime and not a start time, so a run checks for the number going
    *down*: uptime always changes, a restart is the only thing that makes it smaller.
    """
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


def standing(harness, members):
    return [m for m in members
            if int((harness.info(m) or {"deathstate": 1})["deathstate"]) == 0]


def live(harness, entries, rng):
    """Every live creature of these entries within range of the leader, as {guid: row}."""
    out = {}
    for entry in entries:
        for guid, row in harness.enemies(LEADER, entry, rng).items():
            if int(row["alive"]):
                out[guid] = row
    return out


def rest(harness, members, label):
    start = time.time()
    worst_hp = worst_mana = 100.0
    while time.time() - start < REST_TIMEOUT:
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


def dismiss(harness):
    harness.run(f"harness exec {LEADER} partybot removeall", allow_failure=True)
    time.sleep(2.0)
    for name in party(harness):
        harness.run(f"harness exec {name} partybot remove", allow_failure=True)
    time.sleep(2.0)


# Everything static standing on the pyramid top, which a real group kills on the way up. Zero means
# "any hostile" -- naming entries by hand is what the README warns against, and the top of this
# pyramid holds Executioners, Axe Throwers and Blood Drinkers in varying numbers.
ANY_HOSTILE = 0
GUARD_RADIUS = 35.0
GUARD_TIMEOUT = 180.0


def clear_guards(harness, members):
    """Pull the pyramid-top guards one at a time, before the event is started."""
    cleared = 0
    while cleared < 12:
        rows = {g: r for g, r in harness.enemies(LEADER, ANY_HOSTILE, GUARD_RADIUS).items()
                if int(r["alive"]) and int(r["entry"]) not in BLY_CREW}
        if not rows:
            print(f"  guards: cleared {cleared}")
            return None

        guid, row = min(rows.items(), key=lambda kv: float(kv[1]["dist"]))
        entry = int(row["entry"])
        print(f"  guards: pulling {row.get('name', entry)} guid {guid} at {float(row['dist']):.0f}y")

        start = time.time()
        while time.time() - start < GUARD_TIMEOUT:
            still = harness.enemies(LEADER, entry, GUARD_RADIUS).get(int(guid))
            if still is None or not int(still["alive"]):
                break
            if harness.select(LEADER, entry, GUARD_RADIUS) is not None:
                harness.execute(LEADER, "partybot attackstart")
            if not standing(harness, members):
                return f"the party wiped on the pyramid guards after {cleared} cleared"
            time.sleep(3.0)
        else:
            return f"timed out on guard entry {entry} guid {guid}"

        cleared += 1
        rest(harness, members, f"guard {cleared}")

    return None


def open_cages(harness):
    """Use all five cages, one at a time, and report how many trolls that summoned.

    Using every cage rather than just the first is the whole regression test. `gobject select`
    takes the nearest gameobject within ten yards, so the leader is put on each cage in turn.
    """
    used = 0
    for entry, x, y, z in CAGES:
        harness.execute(LEADER, "go xyz %f %f %f %d" % (x, y, z + 1.0, ZF))
        time.sleep(2.0)
        harness.execute(LEADER, "gobject select")
        text = harness.run(f"harness exec {LEADER} gobject use", allow_failure=True)
        if "have used" in text:
            used += 1
        print(f"  cage {entry}: {'used' if 'have used' in text else 'REFUSED'}")
        time.sleep(1.5)
    return used


def count_trolls(harness):
    """(total alive in the pyramid, how many have climbed to the landing).

    Measured as distance from the leader rather than by height, because `.harness enemy` reports
    `dist` and not `z`. The leader stands on the landing for the whole of the event, and the foot
    of the stairs is thirty three yards of descent below it, so anything within the hold radius has
    climbed and anything beyond it has not. Same number the height test would have given, off a
    field that already exists.
    """
    rows = live(harness, WAVE_TROLLS, EVENT_RADIUS)
    up = sum(1 for r in rows.values() if float(r["dist"]) <= UP_THE_STAIRS_DIST)
    return len(rows), up


def bots_below_the_stairs(harness, members):
    """Any bot standing below the top step, which is the one place none of them may be."""
    out = []
    for m in members:
        info = harness.info(m)
        if not info or int(info["map"]) != ZF:
            continue
        if int(info["deathstate"]) != 0:
            continue
        if float(info["z"]) < STAIR_TOP_Z:
            out.append(f"{m}@{float(info['x']):.0f},{float(info['y']):.0f},{float(info['z']):.1f}")
    return out


def focus_of(harness, members):
    """Which of the third wave pair the party is actually working on.

    Read off attacker counts and health rather than `.harness threat`, which is what this did
    first and got wrong: the `victim=` on that command's summary line is the victim of the
    creature being attacked -- a party member -- not the creature the bot is attacking. The
    samples came back as sets of bot names and the assertion was meaningless.
    """
    on = set()
    best = None
    for entry, name in ((NPC_NEKRUM, "Nekrum"), (NPC_SEZZZIZ, "Sezz'ziz")):
        for row in harness.enemies(LEADER, entry, EVENT_RADIUS).values():
            if not int(row["alive"]):
                continue
            attackers = int(row.get("attackers", 0))
            if attackers and (best is None or attackers > best[0]):
                best = (attackers, name)
    if best:
        on.add(best[1])
    return on


# Stall recovery.
#
# A gauntlet can stop making progress without anybody dying, and when it does the party stands
# there while the mobs it disengaged from heal back to full. The answer a player would reach for
# is to walk up and hit something.
#
# These waypoints march the leader down the courtyard along the line wave three is spawned on --
# the instance script puts it between y 1207 and y 1221 -- so each recovery closes the distance
# and the bots follow into range rather than hovering at the edge of it. The heights are the ones
# `.harness ground` measured over x 1880..1890 by y 1198..1246, not guesses: getting that wrong
# once already put the whole party nine yards inside the staircase.
FORCE_PULL_STEPS = [
    (1885.0, 1230.0, 9.6),
    (1885.0, 1222.0, 9.0),
    (1885.0, 1214.0, 8.9),
    (1885.0, 1206.0, 8.9),
]
STALL_SECONDS = 45.0
MAX_FORCED_PULLS = 8


def progress_fingerprint(harness, entries, rng):
    """What counts as the fight moving forward: something died, or something lost real health.

    Not the exact health of everything, which is what this measured first and why it almost never
    fired. A mob that has disengaged regenerates, so its percentage keeps changing and the fight
    reads as progressing while nothing whatever is happening -- one run sat in a nine minute
    stalemate and triggered two forced pulls in the whole of it. Health is therefore bucketed to
    ten percent, so regeneration ticks do not count as progress but a real chunk of damage does,
    and the count of live mobs is carried separately so a kill always registers.
    """
    rows = []
    for entry in entries:
        for guid, row in harness.enemies(LEADER, entry, rng).items():
            if int(row["alive"]):
                rows.append((guid, int(float(row["percent"]) // 10)))
    return (len(rows), tuple(sorted(rows)))


def force_pull(harness, entries, rng, attempt, label):
    """Walk the leader at the nearest live enemy and order the party onto it."""
    step = FORCE_PULL_STEPS[min(attempt, len(FORCE_PULL_STEPS) - 1)]
    harness.execute(LEADER, "go xyz %f %f %f %d" % (step + (ZF,)))
    time.sleep(6.0)

    best = None
    for entry in entries:
        for guid, row in harness.enemies(LEADER, entry, rng).items():
            if not int(row["alive"]):
                continue
            d = float(row["dist"])
            if best is None or d < best[1]:
                best = (entry, d, row.get("name", str(entry)))

    if best is None:
        return False

    entry, dist, name = best
    if harness.select(LEADER, entry, rng) is None:
        return False

    harness.execute(LEADER, "partybot attackstart")
    print(f"  STALL {label}: forced pull #{attempt + 1} on {name} at {dist:.0f}y "
          f"from ({step[0]:.0f},{step[1]:.0f})")
    return True


def run_event(harness, members):
    """Hold the landing for waves one and two, then go down for the third.

    Completion is not "no wave trolls in range". That fires in the ten second PRE_WAVE_2 gap,
    where wave one is dead and wave two has not spawned, and the first version of this reported
    the whole event cleared in a hundred and sixty seconds having fought exactly one wave of it.

    The third wave is also not like the other two. Nothing releases it up the stairs -- the script
    spawns it at the foot and walks Bly's crew down to meet it, and the group is meant to follow.
    So the hold ends when Nekrum and Sezz'ziz appear, which is what wave three arriving looks
    like from outside, and the party is walked down to fight them where they stand.
    """
    start = time.time()
    peak_up = 0
    cap_breaches = []
    holdline_breaches = []
    killorder_samples = []
    nekrum_seen = sezz_seen = False
    descended = False
    all_entries = [NPC_NEKRUM, NPC_SEZZZIZ] + WAVE_TROLLS
    last_shape = None
    last_change = time.time()
    forced = 0

    while time.time() - start < STAGE_TIMEOUT:
        elapsed = round(time.time() - start)
        total, up = count_trolls(harness)
        nekrum = live(harness, [NPC_NEKRUM], EVENT_RADIUS)
        sezz = live(harness, [NPC_SEZZZIZ], EVENT_RADIUS)
        nekrum_seen = nekrum_seen or bool(nekrum)
        sezz_seen = sezz_seen or bool(sezz)

        if not descended:
            peak_up = max(peak_up, up)
            if up > RELEASE_CAP + RELEASE_CAP_MARGIN:
                cap_breaches.append((elapsed, up))
            adrift = bots_below_the_stairs(harness, members)
            if adrift:
                holdline_breaches.append((elapsed, adrift))

        if nekrum and sezz:
            killorder_samples.append(focus_of(harness, members))

        if not standing(harness, members):
            return {"error": (f"the party wiped {elapsed}s in, wave3={nekrum_seen and sezz_seen}, "
                              f"{total} wave trolls alive ({up} up the stairs)")}

        # Wave three is up. Stop holding and go down to it, which is what the script's own
        # PRE_WAVE_3 does with Bly's crew.
        if (nekrum or sezz) and not descended:
            print(f"  wave 3 at {elapsed}s: walking the party down to the foot of the stairs")
            harness.execute(LEADER, "go xyz %f %f %f %d" % FOOT_OF_STAIRS)
            descended = True
            time.sleep(8.0)
            continue

        # Nothing has changed for a while, so push the fight along rather than watching it
        # reset. Only after the descent: a stall while the party is still holding the landing is
        # the instance's business and forcing a pull there would walk them off the choke.
        shape = progress_fingerprint(harness, all_entries, EVENT_RADIUS)
        if shape != last_shape:
            last_shape = shape
            last_change = time.time()
        elif (shape and time.time() - last_change > STALL_SECONDS
              and forced < MAX_FORCED_PULLS):
            if descended:
                if force_pull(harness, all_entries, EVENT_RADIUS, forced, "wave 3"):
                    forced += 1
                    last_change = time.time()
                    continue
            else:
                # Stalled while still holding the landing, which run fifteen did for the whole
                # nine hundred seconds with forced_pulls=0 because this was gated on the descent.
                # The party is not walked anywhere here -- that would give up the choke the event
                # is built on -- it is only re-pointed at the nearest troll, which is enough when
                # the stall is a released troll that stopped short rather than a lost fight.
                if harness.select(LEADER, ANY_HOSTILE, UP_THE_STAIRS_DIST + 15.0) is not None:
                    harness.execute(LEADER, "partybot attackstart")
                    forced += 1
                    last_change = time.time()
                    print(f"  STALL hold: re-pointed the party at the nearest troll "
                          f"(#{forced}, no move)")
                    continue

        if descended:
            # The nearest of them, re-ordered every poll rather than latched.
            #
            # Not "Nekrum, then Sezz'ziz, then the rest", which is what this did first and what
            # stalled run five. Sezz'ziz stands in the middle of the wave three cluster, so
            # ordering the party at him across twenty six yards of unengaged trolls leaves the
            # ranged bots oscillating between a chase that wants to close and a pull rule that
            # will not let them, firing nothing, until the mobs reset. Pull the nearest and let
            # the focus ordering in DungeonTactics pick Nekrum over Sezz'ziz once the fight is
            # actually joined, which is where that ordering is meant to apply.
            nearest = None
            for entry in [NPC_NEKRUM, NPC_SEZZZIZ] + WAVE_TROLLS:
                for row in live(harness, [entry], EVENT_RADIUS).values():
                    d = float(row["dist"])
                    if nearest is None or d < nearest[1]:
                        nearest = (entry, d)
            if nearest and harness.select(LEADER, nearest[0], EVENT_RADIUS) is not None:
                harness.execute(LEADER, "partybot attackstart")

        if nekrum_seen and sezz_seen and not nekrum and not sezz and total == 0:
            return {
                "error": None,
                "seconds": elapsed,
                "peak_up": peak_up,
                "cap_breaches": cap_breaches,
                "holdline_breaches": holdline_breaches,
                "killorder": killorder_samples,
                "forced": forced,
            }

        time.sleep(EVENT_POLL)

    return {"error": (f"the event did not finish inside {STAGE_TIMEOUT:.0f}s "
                      f"(nekrum_seen={nekrum_seen} sezz_seen={sezz_seen} "
                      f"descended={descended} forced_pulls={forced})")}


def fight_bly(harness, members):
    """Take Bly's crew, checking Oro and Murta go before Bly.

    Only counts a crew member as killed once it has been seen alive first. Without that, a crew
    that never turned hostile reads as one that was already dead, and the stage passes having
    fought nothing.
    """
    order = []
    seen = set()
    harness.execute(LEADER, "go xyz %f %f %f %d" % BLY_GROUND)
    time.sleep(10.0)

    # Start it the way the encounter does, through Bly's gossip, rather than by swinging at him.
    # The option only exists once the instance reports PYRAMID_KILLED_ALL_TROLLS, so this doubles
    # as the check that the event really finished -- and selecting it is what sends Weegli off to
    # blow the end door instead of fighting, which is the difference between the four enemies the
    # script intends and the five a test gets by opening with a sword.
    # Whether the escort survived at all, said plainly. Bly dead is not "the event did not
    # finish" -- run nine finished the whole pyramid and then found all five of the crew killed
    # by the trolls, which reads as a gossip failure and is really a protection failure.
    crew_alive = [n for e, n in ((NPC_BLY, "Bly"), (NPC_RAVEN, "Raven"), (NPC_ORO, "Oro"),
                                 (NPC_WEEGLI, "Weegli"), (NPC_MURTA, "Murta"))
                  if live(harness, [e], CREW_RADIUS)]
    print(f"  escort alive after the event: {crew_alive or 'none'}")

    # Two of the five, not all five. Oro, Murta and Raven dying to the waves is an ordinary
    # outcome of this event and players lose them routinely; the encounter carries on regardless.
    # The two that matter are the two it cannot carry on without: Weegli, who is the only thing
    # that opens the end door to Chief Ukorz, and Bly, whose gossip is what starts the crew fight
    # at all. Asserting on all five would fail runs that a player would call a success.
    missing = [n for n in ("Weegli", "Bly") if n not in crew_alive]
    if missing:
        return (f"the escort the encounter needs did not survive: {missing} dead "
                f"(survivors: {crew_alive or 'none'}). Weegli opens the end door and Bly's "
                f"gossip starts the crew fight."), order

    text = harness.run(f"harness gossip {LEADER} {NPC_BLY} 40 0", allow_failure=True)
    print(f"  bly gossip: {text.strip()}")
    if "options=0" in text or "gossip none" in text:
        return ("Bly offered no gossip option, so the instance does not consider the pyramid "
                "event finished"), order
    time.sleep(6.0)

    start = time.time()
    last_shape = None
    last_change = time.time()
    forced = 0
    while time.time() - start < STAGE_TIMEOUT:
        # Same stall recovery as the event stage. Bly has ten times a normal elite's health, so
        # this fight is long enough that a disengagement is easy to miss and expensive to sit
        # through -- he regenerates out of combat like anything else.
        shape = progress_fingerprint(harness, BLY_CREW, CREW_RADIUS)
        if shape != last_shape:
            last_shape = shape
            last_change = time.time()
        elif (shape and time.time() - last_change > STALL_SECONDS
              and forced < MAX_FORCED_PULLS):
            if force_pull(harness, BLY_CREW, CREW_RADIUS, forced, "Bly"):
                forced += 1
                last_change = time.time()
                continue

        alive_now = set()
        for entry in BLY_CREW:
            if live(harness, [entry], CREW_RADIUS):
                alive_now.add(entry)
                seen.add(entry)

        for entry in (NPC_ORO, NPC_MURTA, NPC_BLY):
            if entry in seen and entry not in alive_now and entry not in order:
                order.append(entry)

        if NPC_BLY in seen and NPC_BLY not in alive_now:
            return None, order

        if not seen:
            return "none of Bly's crew ever became attackable", order

        target = None
        for entry in (NPC_ORO, NPC_MURTA, NPC_BLY):
            if entry in alive_now:
                target = entry
                break
        if target and harness.select(LEADER, target, CREW_RADIUS) is not None:
            harness.execute(LEADER, "partybot attackstart")

        if not standing(harness, members):
            return (f"the party wiped on Bly's crew with {sorted(alive_now)} still up, "
                    f"killed {[n for n in order]}"), order
        time.sleep(3.0)

    return f"Bly's crew not down inside {STAGE_TIMEOUT:.0f}s", order


def main():
    harness = Harness.from_env()
    harness.login(LEADER)
    harness.run(f"harness exec {LEADER} character level {LEADER} {LEVEL}", allow_failure=True)
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

    # A fresh copy, or the cages are already open and the trolls already dead.
    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)
    time.sleep(3.0)
    harness.execute(LEADER, "instance unbind all")
    time.sleep(2.0)

    print(f"forming: {', '.join(COMPOSITION)} at level {LEVEL}")
    for spec in COMPOSITION:
        harness.execute(LEADER, f"partybot add {spec} {LEVEL}")
        time.sleep(1.5)
    time.sleep(4.0)

    # Noted before the encounter and checked after it. The world can restart itself with no
    # warning to anything driving it -- the honor maintenancer schedules a nine hundred second
    # restart of its own accord -- and from out here that is indistinguishable from a stuck
    # encounter: the instance and the party vanish, and the suite waits out its whole timeout on
    # creatures that no longer exist. One run was lost to exactly that and read as a wave three
    # failure. AutoHonorRestart is off on this box now; this is so the next cause of it cannot
    # cost a diagnosis as well as a run.
    uptime_before = server_uptime_seconds(harness)

    members = party(harness)
    if len(members) != len(COMPOSITION):
        print(f"FAIL setup: expected {len(COMPOSITION)} bots, got {sorted(members)}")
        dismiss(harness)
        return 1
    print(f"party: {sorted(members)}")

    # Onto the landing first, so the party is standing on the ground it is meant to hold before
    # anything is released, and then up to the cages to start it.
    harness.execute(LEADER, "go xyz %f %f %f %d" % (LANDING + (ZF,)))
    time.sleep(12.0)

    # Clear the pyramid top before starting anything. Sandfury Executioner guid 81552 is a static
    # spawn twenty yards from the middle of the landing, and the first run of this left it standing:
    # Bly's crew are freed hostile to it, engage it immediately, and the party then fought the whole
    # of wave one with two static guards already on it. The README says to clear the pack first and
    # this is what that means here.
    error = clear_guards(harness, members)
    if error:
        print(f"FAIL guards: {error}")
        dismiss(harness)
        return 1

    before, _ = count_trolls(harness)
    if before:
        print(f"WARN: {before} wave trolls already alive in a supposedly fresh instance")

    used = open_cages(harness)
    print(f"cages used: {used}/{len(CAGES)}")
    if used == 0:
        print("FAIL cages: none of the five could be used, so the event never started")
        dismiss(harness)
        return 1

    # Back to the landing and let the bots hold it.
    harness.execute(LEADER, "go xyz %f %f %f %d" % (LANDING + (ZF,)))
    time.sleep(20.0)

    spawned, _ = count_trolls(harness)
    print(f"trolls alive after all {used} cages: {spawned}")
    # Wave one is twenty three. Five cages used to mean five wave ones.
    cage_ok = spawned <= 30
    if not cage_ok:
        print(f"FAIL cages: {spawned} trolls spawned from {used} cages; one wave is 23. "
              f"The cage re-trigger is back.")

    result = run_event(harness, members)
    if result["error"]:
        print(f"FAIL event: {result['error']}")
        dismiss(harness)
        return 1

    print(f"  event cleared in {result['seconds']}s")
    print(f"  peak trolls above the top step at once: {result['peak_up']} (cap {RELEASE_CAP})")

    rest(harness, members, "the pyramid event")

    bly_error, order = fight_bly(harness, members)

    names = {NPC_ORO: "Oro", NPC_MURTA: "Murta", NPC_BLY: "Bly"}
    print(f"  crew died in order: {[names.get(e, e) for e in order]}")

    failures = []
    if not cage_ok:
        failures.append(f"cages spawned {spawned} trolls")
    if result["cap_breaches"]:
        failures.append(f"release cap exceeded at {result['cap_breaches'][:5]}")
    if result["holdline_breaches"]:
        failures.append(f"bots below the top step at {result['holdline_breaches'][:5]}")
    if result["killorder"]:
        on_nekrum = [s for s in result["killorder"] if any("Nekrum" in v for v in s)]
        if not on_nekrum:
            failures.append(f"never on Nekrum while both were up: {result['killorder'][:5]}")
        else:
            print(f"  kill order: on Nekrum in {len(on_nekrum)}/{len(result['killorder'])} samples")
    else:
        print("  WARN: never sampled Nekrum and Sezz'ziz alive at the same time")
    if bly_error:
        failures.append(bly_error)
    elif NPC_BLY in order and order.index(NPC_BLY) == 0:
        failures.append("Bly died before Oro and Murta")

    uptime_after = server_uptime_seconds(harness)
    if (uptime_after is not None and uptime_before is not None
            and uptime_after < uptime_before):
        print(f"VOID: the world restarted during this run (uptime {uptime_before}s -> "
              f"{uptime_after}s). Nothing below is a result.")
        dismiss(harness)
        return 2

    alive = standing(harness, members)
    dismiss(harness)
    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)

    if failures:
        for f in failures:
            print(f"FAIL {f}")
        return 1

    print(f"PASS: pyramid event and Bly's crew cleared, {len(alive)}/{len(members)} bots standing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
