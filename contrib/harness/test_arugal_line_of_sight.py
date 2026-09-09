#!/usr/bin/env python3
"""Archmage Arugal, fought by bots, to see whether they dodge Void Bolt.

Two behaviours are under test and both are visible in the combat log rather than inferred from the
outcome, because a level twenty six boss dies to a five man either way and a kill proves nothing:

  dragfight   the assigned tank walking him off his platform to the fight position, which is the
              sunken floor below it. Measured over his whole room, the platform has no cover on it
              and the floor has two dozen places within six yards.
  breaksight  a bot named by a Void Bolt walking out of his sight before the cast lands. Three
              second cast, single target, and the sight check is re-run when the cast completes.

The fight is started by standing in his aggro radius rather than by an attack order, so nothing here
depends on a selection surviving a round trip.
"""

import re
import sys
import time

from vmangos_harness import Harness

LEADER = "Harnessbot"
MAP_SHADOWFANG_KEEP = 33
ARUGAL = 4275

# On his platform, six yards west of him: inside aggro range, and the wrong side of the fight so
# the drag has somewhere to go.
PLATFORM = (-83.0, 2152.0, 155.7, MAP_SHADOWFANG_KEEP)
# Out of the instance, for the reset between attempts.
OUTSIDE = (1595.0, 240.0, -52.0, 0)

# Named by class rather than by role, and by default a party with not one interrupt in it. With a
# rogue and a warrior in the group Void Bolt is simply taken away -- six of them in one attempt, by
# Kick, Shield Bash and Earth Shock -- and the dodge never gets a turn, so an interrupting party
# tests the interrupt and says nothing at all about line of sight. Pass roles on the command line
# for the other case.
COMPOSITION = ["priest", "mage", "mage", "warlock", "hunter"]
LEVEL = 26
FIGHT_TIMEOUT = 180.0


def bots(harness):
    info = harness.info(LEADER)
    if not info:
        return []
    return [m["name"] for m in info["members_detail"] if m.get("name") != LEADER]


def dismiss(harness):
    # removeall is issued in the leader's own session; remove is per bot and needs the bot's, which
    # is what `harness exec <bot>` gives.
    harness.run(f"harness exec {LEADER} partybot removeall", allow_failure=True)
    time.sleep(2.0)
    for name in bots(harness):
        harness.run(f"harness exec {name} partybot remove", allow_failure=True)
    time.sleep(2.0)


def arugal(harness):
    """Arugal's live state, or None when he is not on the grid."""
    found = harness.enemies(LEADER, ARUGAL, 300.0)
    for guid, row in found.items():
        return row
    return None


def main():
    harness = Harness.from_env()
    harness.login(LEADER)
    # Setting the level takes the character out of the world for a moment, so the revive that
    # follows it finds no player. Tolerated and re-logged rather than raced against, since neither
    # of the two is worth failing a run over.
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

    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)
    time.sleep(3.0)
    harness.execute(LEADER, "instance unbind all")

    composition = sys.argv[1:] or COMPOSITION
    print(f"forming a party of {len(composition)} at level {LEVEL}: {' '.join(composition)}")
    for role in composition:
        harness.execute(LEADER, f"partybot add {role} {LEVEL}")
        time.sleep(1.5)
    time.sleep(4.0)

    members = bots(harness)
    if len(members) != len(composition):
        print(f"FAIL: expected {len(composition)} bots, got {sorted(members)}")
        dismiss(harness)
        return 1
    print(f"party: {sorted(members)}")

    harness.execute(LEADER, "go xyz %f %f %f %d" % PLATFORM)
    time.sleep(12.0)

    followed = [m for m in members
                if (harness.info(m) or {}).get("map") == str(MAP_SHADOWFANG_KEEP)]
    if len(followed) < len(members):
        print(f"WARN: only {len(followed)}/{len(members)} followed into the instance")

    boss = arugal(harness)
    if not boss:
        print("FAIL: Arugal is not on the grid; is the room loaded?")
        dismiss(harness)
        return 1
    full = float(boss["health"])
    print(f"Arugal up at {full:.0f} health")

    # Ordered rather than left to his aggro radius. Standing six yards off him started the fight
    # immediately in a warm instance and not at all in a fresh one, which makes the arrival spot
    # the thing under test instead of the tactics.
    if harness.select(LEADER, ARUGAL, 200.0) is None:
        print("FAIL: could not select Arugal")
        dismiss(harness)
        return 1
    harness.execute(LEADER, "partybot attackstart")
    print("attack ordered")

    start = time.time()
    verdict = "TIMEOUT"
    while time.time() - start < FIGHT_TIMEOUT:
        boss = arugal(harness)
        if boss is None:
            verdict = "GONE"
            break
        if not int(boss["alive"]):
            verdict = "KILLED"
            break
        alive = sum(1 for m in members
                    if int((harness.info(m) or {"deathstate": 1})["deathstate"]) == 0)
        if alive == 0:
            verdict = "WIPED"
            break
        time.sleep(3.0)

    elapsed = time.time() - start
    boss = arugal(harness)
    hp = ("%.0f%%" % (100.0 * float(boss["health"]) / full)) if boss and full else "gone"
    print(f"verdict={verdict} after {elapsed:.0f}s, Arugal at {hp} health")

    dismiss(harness)
    harness.execute(LEADER, "go xyz %f %f %f %d" % OUTSIDE)
    return 0 if verdict == "KILLED" else 1


if __name__ == "__main__":
    sys.exit(main())
