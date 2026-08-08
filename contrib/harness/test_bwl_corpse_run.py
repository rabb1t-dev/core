#!/usr/bin/env python3
"""Watch a party bot run back into Blackwing Lair after dying inside it.

Blackwing Lair is the one instance with no portal a ghost can walk into. The way back is a
scripted trigger beside the Orb of Command out on the continent that answers only to the dead,
so this exercises a path nothing else does: the corpse run has to fall back to the map's ghost
entrance, cross about eighteen hundred yards of Blackrock Mountain, and step into a trigger
that no search of the teleport table would have found.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_bwl_corpse_run.py
"""

import sys
import time

from vmangos_harness import Harness

LEADER = "Harnessbot"

ALIVE = 0
GHOST = 3

MAP_BLACKWING_LAIR = 469

# Just inside, where the entrance portal drops a raid.
INSIDE = (-7672.32, -1107.05, 396.651, MAP_BLACKWING_LAIR)

# Blackhand's Command. The trigger refuses anyone who has not been rewarded it, so without
# this the bot completes the whole run and then stands on the orb doing nothing.
QUEST_BLACKHANDS_COMMAND = 7761

# A raid map refuses a party. Five bots plus the leader is the smallest group that converts.
COMPOSITION = ["dps"] * 5

# Generous: the route is roughly 1800 yards and a ghost runs it at about 20 yards a second,
# after waiting out the release and the recovery timeout.
RUN_TIMEOUT = 400.0


def bots(harness):
    info = harness.info(LEADER)
    if not info:
        return {}
    return {m["name"]: m for m in info["members_detail"] if m["name"] != LEADER}


def reset(harness):
    deadline = time.time() + 90.0
    while time.time() < deadline:
        current = bots(harness)
        if not current:
            return None
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(3.0)
    return f"roster would not clear; {sorted(bots(harness))} remain"


def watch(harness, victim, predicate, timeout, description, fail_if=None):
    """Poll the victim, narrating where it is, until predicate holds."""
    deadline = time.time() + timeout
    started = time.time()
    last = None

    while time.time() < deadline:
        info = harness.info(victim)
        if info:
            state = int(info["deathstate"])
            where = (int(info["map"]), int(info["zone"]),
                     round(float(info["x"])), round(float(info["y"])))
            if where != last:
                print(f"    {time.time() - started:5.0f}s  map {info['map']:>3}  "
                      f"zone {info['zone']:>4}  deathstate {state}  "
                      f"{float(info['x']):8.0f} {float(info['y']):8.0f} {float(info['z']):7.0f}")
                last = where
            if fail_if:
                reason = fail_if(info)
                if reason:
                    return None, reason
            if predicate(info):
                return time.time() - started, None
        time.sleep(3.0)

    return None, f"timed out waiting for {description}"


def gave_up(info):
    """Coming back to life anywhere but inside means the run did not do it.

    The spirit healer revives at the graveyard once a ghost stops making progress, and a live
    bot then follow-teleports to its leader. Ending up inside the instance is therefore not on
    its own evidence of anything, and this test passed once on exactly that.
    """
    if int(info["deathstate"]) == ALIVE and int(info["map"]) != MAP_BLACKWING_LAIR:
        return (f"the spirit healer revived {info['name']} at "
                f"{float(info['x']):.0f} {float(info['y']):.0f}; the corpse run stalled")
    return None


def main():
    harness = Harness.from_env()
    harness.login(LEADER)
    harness.execute(LEADER, f"character level {LEADER} 60")
    harness.execute(LEADER, "revive")

    error = reset(harness)
    if error:
        print(f"FAIL: {error}")
        return 1

    print("forming a raid and moving it into Blackwing Lair")
    for role in COMPOSITION:
        harness.execute(LEADER, f"partybot add {role}")
        time.sleep(1.0)

    time.sleep(5.0)
    members = bots(harness)
    if len(members) != len(COMPOSITION):
        print(f"FAIL: expected {len(COMPOSITION)} bots, got {sorted(members)}")
        return 1

    harness.execute(LEADER, "go xyz %f %f %f %d" % INSIDE)
    time.sleep(15.0)

    victim = sorted(members)[0]
    info = harness.info(victim)
    if not info or int(info["map"]) != MAP_BLACKWING_LAIR:
        print(f"FAIL: {victim} did not follow into the instance; it is on map "
              f"{info['map'] if info else '?'}")
        reset(harness)
        return 1

    print(f"attuning {victim} and killing it inside the instance")
    print(harness.run(f"harness rewardquest {victim} {QUEST_BLACKHANDS_COMMAND}"))
    harness.execute(victim, "die")

    print("  releasing:")
    elapsed, error = watch(
        harness, victim,
        lambda i: int(i["deathstate"]) == GHOST and int(i["map"]) != MAP_BLACKWING_LAIR,
        120.0, "the ghost to release out of the instance",
    )
    if error:
        print(f"FAIL: {error}")
        reset(harness)
        return 1
    print(f"  released to the continent after {elapsed:.0f}s")

    print("  running back:")
    elapsed, error = watch(
        harness, victim,
        lambda i: int(i["map"]) == MAP_BLACKWING_LAIR and int(i["deathstate"]) == ALIVE,
        RUN_TIMEOUT, "the ghost to run back and re-enter",
        fail_if=gave_up,
    )

    failures = []
    if error:
        failures.append(error)
    else:
        print(f"  back inside Blackwing Lair, alive, {elapsed:.0f}s after releasing")

    reset(harness)
    harness.execute(LEADER, "revive")

    for failure in failures:
        print(f"FAIL: {failure}")
    print("PASS" if not failures else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
