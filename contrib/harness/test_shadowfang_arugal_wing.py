#!/usr/bin/env python3
"""Shadowfang Keep's last wing, done in order by a four bot party.

Wolf Master Nandos, who calls three worgs at eighty percent; the door his death opens, checked as
instance state rather than assumed; the walk through it on foot rather than by teleport; the three
Sons of Arugal on the sunken floor; and Archmage Arugal.

Written as a sequence of named stages so that a failure says which one, because "the bots did not
kill Arugal" covers a stuck door, a stuck corridor and a lost fight, and those want different fixes.
"""

import re
import sys
import time

from vmangos_harness import Harness

LEADER = "Harnessbot"
SFK = 33
LEVEL = 26

NPC_NANDOS = 3927
NPC_SON_OF_ARUGAL = 2529
NPC_ARUGAL = 4275
# What Nandos calls at eighty percent, via spells 7487, 7488 and 7489. They outlive him, and the
# first version of this walked off to the door while three worgs were still chewing on the party --
# which is not a navigation failure, it is a test that did not wait for the fight to end.
# Zero means every hostile in range, which is the only reliable way to clear a room. Naming entries
# by hand missed Lupine Delusions and Wolfguard Worgs, and the stage then waited out its timeout on
# the mobs it had not listed. See the dungeon section of contrib/harness/README.md.
ANY_HOSTILE = 0
GO_ARUGAL_LAIR = 18971

# Asked for by class and role both, so the shaman heals and the second warrior does not tank.
COMPOSITION = ["warrior tank", "warrior melee", "mage caster", "shaman healer"]

OUTSIDE = (1595.0, 240.0, -52.0, 0)
# In the Nandos room, a few yards west of him.
NANDOS_ROOM = (-126.0, 2162.0, 155.7, SFK)
# Through the door and along the entrance ledge, then down onto the sunken floor. Walked in legs so
# the bots follow on foot and a corridor they cannot handle shows up as bots left behind.
WALK = [
    (-118.0, 2162.0, 155.7),
    (-112.0, 2161.0, 155.7),
    (-106.0, 2160.0, 155.7),
    (-100.0, 2155.0, 144.9),
    (-95.0, 2150.0, 144.9),
]

STAGE_TIMEOUT = 240.0
WALK_LEG_TIMEOUT = 45.0
# Bots eat and drink on their own out of combat, but only if given the time. Without a wait between
# stages the party walked out of Nandos straight into his worgs on no mana and lost a fight it wins
# rested, which says nothing about either the bots or the encounter.
REST_TIMEOUT = 180.0
REST_HEALTH = 90.0
REST_MANA = 80.0


# How wide each stage looks for its own mobs. Narrow on purpose. Searching three hundred yards for
# entry 3861 finds every Bleak Worg in the keep, not the three Nandos just called, and the first
# version of this ordered a four bot party at level twenty six onto nine of them at once and got the
# wipe it deserved. Shadowfang reuses entries across the whole instance: eight Sons of Arugal are
# spawned in it and only three are in Arugal's room, so the same mistake was waiting at that stage.
ADD_RADIUS = 30.0
SONS_RADIUS = 25.0
BOSS_RADIUS = 50.0


def live(harness, entry, rng):
    """Creatures of an entry within range of the leader that are still alive."""
    return {g: r for g, r in harness.enemies(LEADER, entry, rng).items() if int(r["alive"])}


def party(harness):
    info = harness.info(LEADER)
    if not info:
        return []
    return [m["name"] for m in info["members_detail"] if m.get("name") != LEADER]


def standing(harness, members):
    return [m for m in members
            if int((harness.info(m) or {"deathstate": 1})["deathstate"]) == 0]


def rest(harness, members, label):
    """Wait for the party to eat and drink back up, and revive anyone who died."""
    start = time.time()
    while time.time() - start < REST_TIMEOUT:
        worst_hp = 100.0
        worst_mana = 100.0
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
            print(f"  rested after {label}: worst hp {worst_hp:.0f}%, worst mana {worst_mana:.0f}%")
            return
        time.sleep(5.0)

    print(f"  WARN rest after {label} timed out: worst hp {worst_hp:.0f}%, mana {worst_mana:.0f}%")


def dismiss(harness):
    harness.run(f"harness exec {LEADER} partybot removeall", allow_failure=True)
    time.sleep(2.0)
    for name in party(harness):
        harness.run(f"harness exec {name} partybot remove", allow_failure=True)
    time.sleep(2.0)


def door_state(harness):
    text = harness.run(f"harness gobject {LEADER} {GO_ARUGAL_LAIR} 200", allow_failure=True)
    m = re.search(r"statename=(\w+)", text)
    return m.group(1) if m else None


# Bosses are not trash. A trash clear that does not exclude them kills whatever boss happens to
# stand in the pack -- Nandos is four yards from where the party lands, and a thirty yard radius also
# reaches Fenrus the Devourer in the next room -- and the boss stage that follows then reports
# "nothing alive to fight", which reads as a broken encounter rather than a boss already looted.
BOSS_ENTRIES = {NPC_NANDOS, NPC_ARUGAL, 4274, 3914, 3887, 14682}


def nearest(harness, entries, rng, exclude=frozenset()):
    """The closest live creature among these entries, as (entry, guid, row, distance).

    The entry returned is the creature's own, read off the line, not the one that was searched for.
    Searching with 0 means "any hostile", and passing that sentinel on to `.harness select` selects
    nothing, so the stage sat watching a mob it had never ordered anybody to attack.
    """
    best = None
    for wanted in entries:
        for guid, row in harness.enemies(LEADER, wanted, rng).items():
            if not int(row["alive"]):
                continue
            if int(row["entry"]) in exclude:
                continue
            distance = float(row.get("dist", 0.0))
            if best is None or distance < best[3]:
                best = (int(row["entry"]), guid, row, distance)
    return best


def clear(harness, entries, members, label, rng, required=True, exclude=frozenset()):
    """Clear everything of these entries in range, one pull at a time.

    One at a time because that is how the encounter is played and because the alternative was what
    this test used to do: order the whole party onto every creature of an entry inside the search
    radius at once. Against six Lupine Horrors, which summon more of themselves, four bots at level
    twenty six got through one in four minutes. A pull, a fight, a rest, then the next.
    """
    cleared = 0
    while True:
        target = nearest(harness, entries, rng, exclude)
        if target is None:
            if cleared == 0 and required:
                return f"{label}: nothing alive within {rng:.0f}y to fight"
            print(f"  {label}: cleared {cleared}")
            return None

        entry, guid, row, distance = target
        print(f"  {label}: pulling {row.get('name', entry)} guid {guid} at {distance:.0f}y")
        start = time.time()
        while time.time() - start < STAGE_TIMEOUT:
            still = harness.enemies(LEADER, entry, rng).get(int(guid))
            if still is None or not int(still["alive"]):
                break

            # Re-ordered every poll rather than latched. Issuing it once and then only watching the
            # target's health meant that any disengagement -- an evade and reset, aggro scattering
            # off a summon -- left nobody re-ordering the party, and the stage burned its whole
            # timeout on a party standing still with mana climbing.
            if harness.select(LEADER, entry, rng) is not None:
                harness.execute(LEADER, "partybot attackstart")

            if not standing(harness, members):
                return (f"{label}: the party wiped on entry {entry} guid {guid} at "
                        f"{still.get('percent', still.get('health'))} health, {cleared} cleared first")
            time.sleep(3.0)
        else:
            return (f"{label}: timed out on entry {entry} guid {guid} after "
                    f"{STAGE_TIMEOUT:.0f}s, {cleared} cleared first")

        cleared += 1
        rest(harness, members, f"{label} pull {cleared}")


def walk(harness, members):
    """Move the leader in legs and let the bots follow on their own feet."""
    for i, (x, y, z) in enumerate(WALK):
        harness.execute(LEADER, "go xyz %f %f %f %d" % (x, y, z, SFK))
        start = time.time()
        while time.time() - start < WALK_LEG_TIMEOUT:
            here = 0
            for m in members:
                info = harness.info(m)
                if not info:
                    continue
                if (int(info["map"]) == SFK and
                        abs(float(info["x"]) - x) < 15.0 and abs(float(info["y"]) - y) < 15.0):
                    here += 1
            if here == len(members):
                break
            time.sleep(2.0)
        else:
            behind = []
            for m in members:
                info = harness.info(m)
                behind.append(f"{m}@{float(info['x']):.0f},{float(info['y']):.0f},{float(info['z']):.0f}"
                              if info else f"{m}@gone")
            return f"walk: leg {i + 1} of {len(WALK)} to {x:.0f},{y:.0f} left bots behind: {behind}"
        print(f"  walk leg {i + 1}/{len(WALK)} to {x:.0f},{y:.0f},{z:.0f}: whole party arrived")
    return None


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

    # A fresh copy, so Nandos is alive and his door is shut to begin with.
    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)
    time.sleep(3.0)
    harness.execute(LEADER, "instance unbind all")
    time.sleep(2.0)

    print(f"forming: {', '.join(COMPOSITION)} at level {LEVEL}")
    for spec in COMPOSITION:
        harness.execute(LEADER, f"partybot add {spec} {LEVEL}")
        time.sleep(1.5)
    time.sleep(4.0)

    members = party(harness)
    if len(members) != len(COMPOSITION):
        print(f"FAIL setup: expected {len(COMPOSITION)} bots, got {sorted(members)}")
        dismiss(harness)
        return 1
    print(f"party: {sorted(members)}")

    harness.execute(LEADER, "go xyz %f %f %f %d" % NANDOS_ROOM)
    time.sleep(12.0)

    shut = door_state(harness)
    print(f"door before Nandos: {shut}")
    if shut != "shut":
        print(f"WARN: expected the lair door shut in a fresh instance, it is {shut}")

    stages = []

    # The room's own worgs first. A real group does not pull a boss into the pack standing next to
    # him, and Nandos calls three more of his own once the fight starts.
    error = clear(harness, [ANY_HOSTILE], members, "room trash", ADD_RADIUS,
                  required=False, exclude=BOSS_ENTRIES)
    stages.append(("room worgs", error))
    if error:
        print(f"FAIL {error}")
        dismiss(harness)
        return 1

    error = clear(harness, [NPC_NANDOS], members, "Nandos", BOSS_RADIUS)
    stages.append(("Nandos", error))
    if error:
        print(f"FAIL {error}")
        dismiss(harness)
        return 1
    print("  Nandos down")

    # The three he called at eighty percent outlive him and go before anybody walks anywhere.
    error = clear(harness, [ANY_HOSTILE], members, "called worgs", ADD_RADIUS,
                  required=False, exclude=BOSS_ENTRIES)
    stages.append(("called worgs", error))
    if error:
        print(f"FAIL {error}")
        dismiss(harness)
        return 1

    time.sleep(4.0)
    opened = door_state(harness)
    print(f"door after Nandos: {opened}")
    if opened != "open":
        print(f"FAIL door: Nandos died but the lair door is {opened}")
        dismiss(harness)
        return 1

    error = walk(harness, members)
    stages.append(("walk", error))
    if error:
        print(f"FAIL {error}")
        dismiss(harness)
        return 1

    error = clear(harness, [NPC_SON_OF_ARUGAL], members, "Sons of Arugal", SONS_RADIUS)
    stages.append(("Sons", error))
    if error:
        print(f"FAIL {error}")
        dismiss(harness)
        return 1
    print("  Sons of Arugal down")

    error = clear(harness, [NPC_ARUGAL], members, "Archmage Arugal", BOSS_RADIUS)
    stages.append(("Arugal", error))
    if error:
        print(f"FAIL {error}")
        dismiss(harness)
        return 1

    alive = standing(harness, members)
    print(f"PASS: wing cleared in order, {len(alive)}/{len(members)} bots still standing")
    dismiss(harness)
    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
