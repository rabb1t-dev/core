#!/usr/bin/env python3
"""Perform a real corpse run for every instance, rather than measuring one.

The entrance survey checks a static property of the navigation mesh. It cannot see the gap
between a route existing and a bot actually walking it and getting through the door, and both
bugs behind the Blackwing Lair run lived in exactly that gap. This drives the whole thing: a
group is moved inside an instance, one bot is killed, and that bot has to release to its
graveyard, cross however much world lies in between, and let itself back in unaided.

Success is the route, not the destination. A bot that stalls is eventually revived at the
graveyard by the spirit healer and then follow-teleports to its leader, who is still inside,
so ending up alive in the instance proves nothing on its own and is treated as a failure.

Nearly all of the elapsed time is spent watching a ghost walk, and those waits are independent,
so the runs are spread over a pool of leaders that each drive their own group. One leader can
only be in one place, which is the only reason this needs more than one.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_corpse_runs_live.py
"""

import argparse
import collections
import os
import queue
import subprocess
import sys
import threading
import time

from vmangos_harness import Harness, CommandError

# Character names cannot contain digits, so the pool is named rather than numbered.
LEADER_NAMES = [
    "Harnessbot", "Leadbravo", "Leadcharlie", "Leaddelta", "Leadecho",
    "Leadfoxtrot", "Leadgolf", "Leadhotel", "Leadindia", "Leadjuliet",
]
LEADER_ACCOUNT_PREFIX = "harnesslead"
LEADER_PASSWORD = "harness"
LEADER_RACE, LEADER_CLASS = 2, 1        # Orc warrior, so the group releases to Horde graveyards

# Player::DeathState, as reported by .harness info.
ALIVE = 0
GHOST = 3

# A raid map refuses a party outright, and six is the smallest group that converts to a raid.
# A dungeon needs nothing of the sort, and keeping those groups to two avoids crowding a
# five player instance with a party of six and having the server turn someone away.
RAID_BOTS = 5
DUNGEON_BOTS = 1

# Open ground in the Barrens, where groups are formed before being moved anywhere.
STAGING = (-600.0, -2515.0, 92.0, 1)

EXCLUDED_MAPS = (
    29,     # CashTest
    44,     # <unused> Monastery
    169,    # Emerald Dream, unfinished and unreachable
    269,    # Caverns of Time, an empty shell in this expansion
)

# What a ghost needs before the way back in will admit it. The level and condition gates on an
# entrance portal are checked for the dead exactly as for the living; only a game master is
# waved through, and putting the bot in that state would defeat the whole test.
ATTUNEMENTS = {
    249: ["harness exec {bot} additem 16309"],      # Onyxia: the Drakefire Amulet, held not worn
    409: ["harness rewardquest {bot} 7848"],        # Molten Core: Attunement to the Core
    469: ["harness rewardquest {bot} 7761"],        # Blackwing Lair: Blackhand's Command
    533: ["harness rewardquest {bot} 9378"],        # Naxxramas: The Dread Citadel
}

# Ahn'Qiraj is sealed until the war effort finishes, and the seal is an entrance condition that
# no amount of attuning satisfies. Opening the gates is the only way to test the run, so the
# event is stopped for the duration and put back afterwards.
AQ_GATE_EVENT = 83
AQ_MAPS = (509, 531)

RELEASE_TIMEOUT = 150.0
RUN_TIMEOUT = 540.0
FOLLOW_TIMEOUT = 90.0
ENTRY_TIMEOUT = 30.0
GROUP_TIMEOUT = 150.0
POLL = 3.0
GATE_REFRESH = 20.0

Instance = collections.namedtuple("Instance", "map_id name raid x y z")

output_lock = threading.Lock()


def emit(lines):
    with output_lock:
        for line in lines:
            print(line, flush=True)


def query(sql):
    command = os.environ.get("VMANGOS_MYSQL", "sudo mysql mangos")
    result = subprocess.run(
        command.split() + ["-N", "-B", "-e", sql],
        capture_output=True, text=True, check=True,
    )
    return [line.split("\t") for line in result.stdout.splitlines() if line]


def load_instances():
    """Every instance, with a point inside it that a portal actually lands people on."""
    rows = query(f"""
        SELECT mt.entry, mt.map_name, mt.map_type, tp.id,
               tp.target_position_x, tp.target_position_y, tp.target_position_z
        FROM map_template mt
        JOIN areatrigger_teleport tp ON tp.target_map = mt.entry
        WHERE mt.map_type IN (1, 2)
          AND mt.entry NOT IN ({",".join(str(m) for m in EXCLUDED_MAPS)})
        ORDER BY mt.map_type, mt.entry, tp.id
    """)

    instances = collections.OrderedDict()
    for row in rows:
        map_id = int(row[0])
        if map_id in instances:
            continue
        instances[map_id] = Instance(map_id, row[1], int(row[2]) == 2,
                                     *(float(v) for v in row[4:7]))
    return instances


def info(harness, name):
    try:
        return harness.info(name)
    except CommandError:
        return None


def roster(harness, leader):
    """Group members other than the leader, keyed by name."""
    current = info(harness, leader)
    if not current:
        return {}
    return {m["name"]: m for m in current["members_detail"] if m["name"] != leader}


def clear_roster(harness, leader):
    deadline = time.time() + 90.0
    while time.time() < deadline:
        current = roster(harness, leader)
        if not current:
            return None
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)
    return f"roster would not clear; {sorted(roster(harness, leader))} remain"


def ensure_group(harness, leader, wanted):
    """Give the leader exactly the number of bots this kind of instance needs.

    Topped up in a loop rather than filled once. An add does not always take, and with several
    leaders recruiting at the same time it stopped taking often enough to fail three raids on
    setup alone. Asking again is a great deal simpler than working out why.
    """
    deadline = time.time() + GROUP_TIMEOUT
    while time.time() < deadline:
        current = roster(harness, leader)
        if len(current) == wanted and all(int(m["deathstate"]) == ALIVE
                                          for m in current.values()):
            return sorted(current)[0], None

        surplus = sorted(current)[wanted:]
        for name in surplus:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        for _ in range(wanted - len(current)):
            harness.run(f"harness exec {leader} partybot add dps", allow_failure=True)
        time.sleep(POLL)

    return None, f"only {len(roster(harness, leader))} of {wanted} bots joined"


def wait_for(harness, who, predicate, timeout, description, log=None, fail_if=None):
    """Poll a character until predicate holds, optionally narrating where it goes."""
    deadline = time.time() + timeout
    started = time.time()
    last = None

    while time.time() < deadline:
        current = info(harness, who)
        if current:
            if log is not None:
                where = (int(current["map"]), round(float(current["x"])),
                         round(float(current["y"])))
                if where != last:
                    log.append(
                        f"      {time.time() - started:5.0f}s  map {current['map']:>3}  "
                        f"zone {current['zone']:>4}  state {current['deathstate']}  "
                        f"{float(current['x']):8.0f} {float(current['y']):8.0f} "
                        f"{float(current['z']):7.0f}")
                    last = where
            if fail_if:
                reason = fail_if(current)
                if reason:
                    return None, reason
            if predicate(current):
                return time.time() - started, None
        time.sleep(POLL)

    return None, f"timed out after {timeout:.0f}s waiting for {description}"


def run_one(harness, leader, instance, trace):
    """Kill a bot inside the instance and require it to walk itself back in."""
    log = [] if trace else None

    # Recruiting happens out in the open. Doing it where the leader happens to be standing
    # means doing it inside the dungeon it has just finished, and that instance's five player
    # cap then quietly turns the last bot away: "only 4 of 5 bots joined" was never the group
    # being full, it was the room.
    harness.run(f"harness exec {leader} revive", allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d" % ((leader,) + STAGING),
                allow_failure=True)
    _, error = wait_for(harness, leader, lambda i: int(i["map"]) == STAGING[3],
                        ENTRY_TIMEOUT, "the leader to reach open ground")
    if error:
        return False, f"setup: {error}", log

    victim, error = ensure_group(
        harness, leader, RAID_BOTS if instance.raid else DUNGEON_BOTS)
    if error:
        return False, f"setup: {error}", log

    harness.run(f"harness exec {victim} revive", allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d"
                % (leader, instance.x, instance.y, instance.z, instance.map_id),
                allow_failure=True)

    # Asked separately, because the server refuses an entry by dropping the teleport and
    # telling a client that is not there. Rolled into the wait below it looks like the bots
    # failing to follow, which is what sent the first run of this suite chasing the wrong bug.
    _, error = wait_for(harness, leader, lambda i: int(i["map"]) == instance.map_id,
                        ENTRY_TIMEOUT, "the leader to get in")
    if error:
        return False, ("setup: the server refused to let the leader in "
                       "(instance cap, raid group, entry condition or the per hour limit)"), log

    _, error = wait_for(
        harness, victim,
        lambda i: int(i["map"]) == instance.map_id and int(i["deathstate"]) == ALIVE,
        FOLLOW_TIMEOUT, "the group to follow the leader inside")
    if error:
        return False, f"setup: {error}", log

    # An attunement that fails to apply looks exactly like a broken route: the ghost walks the
    # whole way and is then turned away at the door. Refuse to run rather than report that.
    for command in ATTUNEMENTS.get(instance.map_id, []):
        try:
            harness.run(command.format(bot=victim))
        except CommandError as exc:
            return False, f"setup: attunement failed: {exc}", log

    harness.run(f"harness exec {victim} die", allow_failure=True)

    released, error = wait_for(
        harness, victim,
        lambda i: int(i["deathstate"]) == GHOST and int(i["map"]) != instance.map_id,
        RELEASE_TIMEOUT, "the ghost to release out of the instance", log)
    if error:
        return False, f"release: {error}", log

    def gave_up(current):
        if int(current["deathstate"]) == ALIVE and int(current["map"]) != instance.map_id:
            return (f"spirit healer revived it at {float(current['x']):.0f} "
                    f"{float(current['y']):.0f} on map {current['map']}; the run stalled")
        return None

    elapsed, error = wait_for(
        harness, victim,
        lambda i: int(i["map"]) == instance.map_id and int(i["deathstate"]) == ALIVE,
        RUN_TIMEOUT, "the ghost to run back and re-enter", log, gave_up)
    if error:
        return False, error, log

    return True, f"released in {released:.0f}s, back inside {elapsed:.0f}s later", log


def worker(leader, work, results, trace):
    harness = Harness.from_env()
    while True:
        try:
            instance = work.get_nowait()
        except queue.Empty:
            harness.run(f"harness exec {leader} revive", allow_failure=True)
            clear_roster(harness, leader)
            return

        started = time.time()
        try:
            ok, detail, log = run_one(harness, leader, instance, trace)
        except CommandError as exc:
            ok, detail, log = False, f"harness error: {exc}", None

        lines = [f"{instance.name}  [{leader}]"] + (log or [])
        lines.append(f"  {'pass' if ok else 'FAIL'}  {instance.name:<24} {detail} "
                     f"[{time.time() - started:.0f}s]")
        lines.append("")
        emit(lines)
        results.append((ok, instance.name, detail))


def provision_leaders(harness, count):
    """Create and log in one headless leader per worker.

    Each gets an account to itself, because the bot manager refuses to open a second session
    for an account that already has one, so a shared account caps the pool at a single leader.
    The account needs game master rights of its own: .harness exec runs a command inside the
    target's session, so it is that account's level the command is checked against, not the
    level of whoever is driving over SOAP.
    """
    ready = []
    for index, name in enumerate(LEADER_NAMES[:count]):
        account = f"{LEADER_ACCOUNT_PREFIX}{index}"
        harness.run(f"account create {account} {LEADER_PASSWORD}", allow_failure=True)
        harness.run(f"account set gmlevel {account} 6", allow_failure=True)

        # An offline leader is erased and remade rather than reused, because the name says
        # nothing about which account owns it and a leader stranded on an account that already
        # has a session can never be logged in. Recreating is cheap; diagnosing that is not.
        if info(harness, name) is None:
            harness.run(f"character erase {name}", allow_failure=True)
            harness.run(f"harness createchar {account} {name} "
                        f"{LEADER_RACE} {LEADER_CLASS} 0", allow_failure=True)
        try:
            harness.login(name)
        except CommandError as exc:
            print(f"leaving {name} out of the pool: {exc}", flush=True)
            continue
        harness.run(f"harness exec {name} character level {name} 60", allow_failure=True)
        ready.append(name)
    return ready


def event_active(harness, event_id):
    listing = harness.run("event list", allow_failure=True)
    return any(line.strip().startswith(f"{event_id} -") and "[active]" in line
               for line in listing.splitlines())


def hold_gates_open(stop):
    """Keep the Ahn'Qiraj seal off for as long as the suite runs.

    Stopping the event once is not enough to keep it stopped. The game event manager re-reads
    its schedule periodically and puts 83 back on underneath the run, which shuts the gates on
    whichever ghost is walking back at the time. Both Ahn'Qiraj instances fail together when
    that happens, and they fail standing on the entrance being refused, which reads like a
    broken route rather than a door that closed.
    """
    harness = Harness.from_env()
    while not stop.wait(GATE_REFRESH):
        if event_active(harness, AQ_GATE_EVENT):
            harness.run(f"event stop {AQ_GATE_EVENT}", allow_failure=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", default=[],
                        help="substring of an instance name; repeatable")
    parser.add_argument("--raids", action="store_true")
    parser.add_argument("--dungeons", action="store_true")
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--trace", action="store_true", help="narrate every move")
    args = parser.parse_args()

    harness = Harness.from_env()

    instances = [
        i for i in load_instances().values()
        if (not args.only or any(w.lower() in i.name.lower() for w in args.only))
        and (not (args.raids or args.dungeons)
             or (args.raids and i.raid) or (args.dungeons and not i.raid))
    ]
    if not instances:
        print("nothing selected")
        return 1

    workers = max(1, min(args.workers, len(instances), len(LEADER_NAMES)))
    leaders = provision_leaders(harness, workers)
    if not leaders:
        print("FAIL: no leader could be logged in")
        return 1

    # Dungeons first so that each leader regroups at most once, when it crosses over to the
    # raids and needs four more bodies to make a raid group.
    work = queue.Queue()
    for instance in sorted(instances, key=lambda i: i.raid):
        work.put(instance)

    print(f"{len(instances)} instances over {len(leaders)} leaders: "
          f"{', '.join(leaders)}\n", flush=True)

    restore_gates = False
    gatekeeper = None
    stop_gatekeeper = threading.Event()
    if any(i.map_id in AQ_MAPS for i in instances):
        if event_active(harness, AQ_GATE_EVENT):
            harness.run(f"event stop {AQ_GATE_EVENT}", allow_failure=True)
            restore_gates = True
        gatekeeper = threading.Thread(target=hold_gates_open, args=(stop_gatekeeper,),
                                      daemon=True)
        gatekeeper.start()
        print(f"opened the Ahn'Qiraj gates by stopping event {AQ_GATE_EVENT}, "
              f"and holding them open\n", flush=True)

    results = []
    threads = [threading.Thread(target=worker, args=(leader, work, results, args.trace),
                                daemon=True)
               for leader in leaders]
    started = time.time()
    try:
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    finally:
        stop_gatekeeper.set()
        if gatekeeper:
            gatekeeper.join(timeout=GATE_REFRESH + 10.0)
        if restore_gates:
            harness.run(f"event start {AQ_GATE_EVENT}", allow_failure=True)

    failed = [(name, detail) for ok, name, detail in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} instances ran back unaided "
          f"in {(time.time() - started) / 60:.0f} minutes")
    for name, detail in sorted(failed):
        print(f"FAIL  {name:<24} {detail}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
