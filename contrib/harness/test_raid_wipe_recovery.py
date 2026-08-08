#!/usr/bin/env python3
"""Wipe a full forty-man raid inside a real raid instance and make it recover unaided.

Everything else so far has proved the corpse run one bot at a time. That is the right way to
find route bugs and the wrong way to find anything that only appears at scale, and a guild
raid is the case this all exists to serve: forty bodies on the floor, nobody left standing to
resurrect anyone, forty ghosts released to the same graveyard walking the same road back.

The leader dies with everyone else, which is the honest version of a wipe and also the harder
one: with nobody left inside, the instance has no live player holding it open, and every corpse
in it belongs to someone who has left. If the map unloads before they get back, it takes the
corpses with it and the raid has nothing to run to.

Success is that all forty walk back in. A bot the spirit healer collects has been rescued by
the deadlock breaker rather than recovering, so it counts as a failure however alive it ends up.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_raid_wipe_recovery.py
"""

import argparse
import collections
import os
import subprocess
import sys
import time

from vmangos_harness import Harness, CommandError

LEADER = "Harnessbot"

# Player::DeathState, as reported by .harness info.
ALIVE = 0
CORPSE = 2
GHOST = 3

# Open ground in the Barrens. Recruiting happens here rather than wherever the leader was left,
# because doing it inside an instance runs into that instance's player cap.
STAGING = (-600.0, -2515.0, 92.0, 1)

# What a ghost needs before the way back in will admit it, applied to every member since every
# member has to get itself through the door on its own.
ATTUNEMENTS = {
    249: "harness exec {who} additem 16309",    # Onyxia: the Drakefire Amulet, held not worn
    409: "harness rewardquest {who} 7848",      # Molten Core: Attunement to the Core
    469: "harness rewardquest {who} 7761",      # Blackwing Lair: Blackhand's Command
    533: "harness rewardquest {who} 9378",      # Naxxramas: The Dread Citadel
}

# Ahn'Qiraj is sealed until the war effort finishes, and the seal is an entrance condition that
# no amount of attuning satisfies.
AQ_GATE_EVENT = 83
AQ_MAPS = (509, 531)

GROUP_TIMEOUT = 300.0
FOLLOW_TIMEOUT = 180.0
RELEASE_TIMEOUT = 240.0
RUN_TIMEOUT = 900.0
POLL = 3.0

Instance = collections.namedtuple("Instance", "map_id name limit x y z")


def query(sql):
    command = os.environ.get("VMANGOS_MYSQL", "sudo mysql mangos")
    result = subprocess.run(
        command.split() + ["-N", "-B", "-e", sql],
        capture_output=True, text=True, check=True,
    )
    return [line.split("\t") for line in result.stdout.splitlines() if line]


def load_raids():
    """Every raid, its player cap, and a point inside that a portal actually lands people on.

    The cap decides how many bots each one gets. Filling a twenty man raid with thirty nine
    would simply be turned away at the door, and by the instance rather than by anything this
    is trying to test.
    """
    rows = query("""
        SELECT mt.entry, mt.map_name, mt.player_limit,
               tp.target_position_x, tp.target_position_y, tp.target_position_z
        FROM map_template mt
        JOIN areatrigger_teleport tp ON tp.target_map = mt.entry
        WHERE mt.map_type = 2 AND mt.player_limit > 0
        ORDER BY mt.entry, tp.id
    """)

    raids = collections.OrderedDict()
    for row in rows:
        map_id = int(row[0])
        if map_id in raids:
            continue
        raids[map_id] = Instance(map_id, row[1], int(row[2]),
                                 *(float(v) for v in row[3:6]))
    return raids


def info(harness, who):
    try:
        return harness.info(who)
    except CommandError:
        return None


def roster(harness):
    current = info(harness, LEADER)
    if not current:
        return {}
    return {m["name"]: m for m in current["members_detail"] if m["name"] != LEADER}


def clear_roster(harness):
    deadline = time.time() + 180.0
    while time.time() < deadline:
        current = roster(harness)
        if not current:
            return None
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)
    return f"roster would not clear; {len(roster(harness))} remain"


def build_raid(harness, wanted):
    """Bring the raid to exactly this strength, topping up rather than filling once.

    An add does not always take, and asking again is a great deal simpler than working out
    why. At this size that matters more than it did at five: one add in forty going astray
    would otherwise sink the whole run.
    """
    deadline = time.time() + GROUP_TIMEOUT
    while time.time() < deadline:
        current = roster(harness)
        if len(current) == wanted and all(int(m["deathstate"]) == ALIVE
                                          for m in current.values()):
            return sorted(current), None

        for name in sorted(current)[wanted:]:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        for _ in range(wanted - len(current)):
            harness.run(f"harness exec {LEADER} partybot add dps", allow_failure=True)
        time.sleep(POLL)

    return None, f"only {len(roster(harness))} of {wanted} bots joined"


def census(harness, names, instance):
    """Where everyone is and what state they are in, as one pass over the raid."""
    seen = {}
    for name in names:
        current = info(harness, name)
        if current:
            seen[name] = (int(current["deathstate"]), int(current["map"]))
    return seen


def wait_for_raid(harness, names, instance, predicate, timeout, description,
                  progress=None):
    """Poll the whole raid until predicate holds for all of it."""
    deadline = time.time() + timeout
    started = time.time()
    last_report = 0.0
    seen = {}

    while time.time() < deadline:
        seen = census(harness, names, instance)
        done = [n for n in names if n in seen and predicate(*seen[n])]
        if len(done) == len(names):
            return time.time() - started, None

        now = time.time()
        if progress and now - last_report >= 15.0:
            last_report = now
            print(f"      {now - started:5.0f}s  {len(done):>2} of {len(names)} "
                  f"{description}", flush=True)
        time.sleep(POLL)

    missing = sorted(n for n in names if n not in seen or not predicate(*seen[n]))
    return None, (f"timed out after {timeout:.0f}s waiting for {description}; "
                  f"{len(missing)} still short, e.g. {missing[:5]}")


def run_raid(harness, instance, size):
    """Fill this raid, wipe it inside, and require all of it back in unaided."""
    print(f"{instance.name} (map {instance.map_id}), {size} bots plus {LEADER}",
          flush=True)

    # Regrouping happens out in the open. Recruiting where the raid is standing means
    # recruiting inside the instance it just cleared, and that instance's player cap turns
    # away everyone past it.
    harness.run(f"harness exec {LEADER} revive", allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d" % ((LEADER,) + STAGING),
                allow_failure=True)
    time.sleep(POLL)

    names, error = build_raid(harness, size)
    if error:
        return f"recruiting: {error}"

    attunement = ATTUNEMENTS.get(instance.map_id)
    if attunement:
        for who in names + [LEADER]:
            harness.run(attunement.format(who=who), allow_failure=True)

    harness.run("harness exec %s go xyz %f %f %f %d"
                % (LEADER, instance.x, instance.y, instance.z, instance.map_id),
                allow_failure=True)

    _, error = wait_for_raid(
        harness, names, instance,
        lambda state, map_id: map_id == instance.map_id and state == ALIVE,
        FOLLOW_TIMEOUT, "inside", progress=True)
    if error:
        return f"moving in: {error}"

    for who in names + [LEADER]:
        harness.run(f"harness exec {who} die", allow_failure=True)

    released, error = wait_for_raid(
        harness, names, instance,
        lambda state, map_id: state == GHOST or
        (state == ALIVE and map_id == instance.map_id),
        RELEASE_TIMEOUT, "released", progress=True)
    if error:
        return f"releasing: {error}"

    elapsed, error = wait_for_raid(
        harness, names, instance,
        lambda state, map_id: state == ALIVE and map_id == instance.map_id,
        RUN_TIMEOUT, "back inside", progress=True)

    # Anyone alive outside the instance was picked up by the spirit healer, which is the
    # deadlock breaker rather than a recovery, so it does not count.
    seen = census(harness, names, instance)
    rescued = sorted(n for n, (state, map_id) in seen.items()
                     if state == ALIVE and map_id != instance.map_id)
    if error:
        return f"running back: {error}"
    if rescued:
        return (f"{len(rescued)} were revived outside rather than running back, "
                f"e.g. {rescued[:5]}")

    print(f"  pass  all {len(names)} released in {released:.0f}s and were back inside "
          f"{elapsed:.0f}s later\n", flush=True)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", default=[],
                        help="substring of a raid name; repeatable")
    parser.add_argument("--size", type=int,
                        help="override the bot count instead of filling to the cap")
    args = parser.parse_args()

    raids = [r for r in load_raids().values()
             if not args.only or any(o.lower() in r.name.lower() for o in args.only)]
    if not raids:
        raise SystemExit("no raids matched")

    harness = Harness.from_env()
    harness.login(LEADER)
    harness.run(f"harness exec {LEADER} character level {LEADER} 60", allow_failure=True)

    restore_gates = False
    if any(r.map_id in AQ_MAPS for r in raids):
        listing = harness.run("event list", allow_failure=True)
        if any(l.strip().startswith(f"{AQ_GATE_EVENT} -") and "[active]" in l
               for l in listing.splitlines()):
            harness.run(f"event stop {AQ_GATE_EVENT}", allow_failure=True)
            restore_gates = True

    print(f"{len(raids)} raids, wiped at full strength\n", flush=True)

    failures = []
    started = time.time()
    try:
        for instance in raids:
            size = args.size or (instance.limit - 1)
            error = run_raid(harness, instance, size)
            if error:
                failures.append((instance.name, error))
                print(f"  FAIL  {error}\n", flush=True)
    finally:
        clear_roster(harness)
        harness.run(f"harness exec {LEADER} revive", allow_failure=True)
        if restore_gates:
            harness.run(f"event start {AQ_GATE_EVENT}", allow_failure=True)

    print(f"{len(raids) - len(failures)} of {len(raids)} raids recovered from a full wipe "
          f"in {(time.time() - started) / 60:.0f} minutes")
    for name, error in failures:
        print(f"FAIL  {name:<24} {error}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
