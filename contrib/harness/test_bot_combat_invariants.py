#!/usr/bin/env python3
"""Fight one target and require that no bot fell into a state the tick log already describes.

The suites next to this one each assert a behaviour: the tank keeps the mob, the raid recovers
from a wipe, the rogue spends its combo points. This one asserts the absence of three failures
instead, and it exists because all three shipped, ran for a whole session, and were found by a
person watching the game rather than by anything here.

What makes them worth a suite of their own is that none is visible from outside. A bot in each of
these states is alive, in the group, in combat, and has a target; `harness info` reports nothing
unusual and the threat list looks ordinary. The evidence is in the tick log and only there, so
this reads the log rather than the world.

  * **A restarted autorepeat.** A wand or a bow is toggled on and fires on its own timer. Two
    rules disagreed about whether a caster with mana should be wanding -- one started a shot, the
    other cancelled it on the next tick -- and the result was a bot paying for a cast four times a
    second and never landing one. In the log it is a run of `started 'Shoot'` lines with
    `autorepeat=0` on every tick between them.

  * **A bot that goes quiet mid-fight.** Every early return in UpdateAI sits above the call that
    writes the per-tick line, so a bot held at one of them produces no output at all. It does not
    read as a stalled bot; it reads as a bot that is not there. One mage spent forty four seconds
    of a fight in this state and the fight is a blank in the file.

  * **A rotation that ignores its own talents.** The mage nuke order was a fixed chain for every
    mage alive, so a fire build spent its fight casting Frostbolt. Nothing failed, nothing errored,
    and the damage was simply lower than it should have been for six years.

Thresholds are set where a healthy bot is not near them rather than at the tightest value that
would still pass. Measured across one Maraudon run: the worst healthy bot reached ten seconds of
combat silence and the broken one forty four, so the bar is twenty. The same reasoning applies to
the others -- this suite is here to catch a rule that has stopped working, not to police tuning.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM account:
python3 test_bot_combat_invariants.py
"""

import argparse
import os
import re
import sys
import time
from collections import defaultdict

from vmangos_harness import Harness

LEADER = "Harnessbot"
POLL = 2.0
GROUP_TIMEOUT = 180.0

STAGING = (-600.0, -2515.0, 92.0, 1)

# The same target the threat suite uses: a level 60 with a hundred times the usual health, so the
# fight lasts long enough for a stall to be a stall rather than a gap between pulls.
PUNCHING_BAG = 11080

DEFAULT_LOG = "/srv/vmangos/server/logs/Server.log"

# Asked for by class so the group contains the two the school check needs. A mage and a warlock
# also happen to be the two casters whose rotations are most rule-driven.
DAMAGE_CLASSES = ["mage", "warlock", "hunter", "priest", "rogue"]

# A bot may restart an autorepeat legitimately -- it moved, the target died, something interrupted
# it. What is not legitimate is doing so repeatedly with no tick in between reporting the shot
# actually running.
MAX_AUTOREPEAT_RESTARTS = 4

# Seconds a bot may produce no log line at all while the group is demonstrably fighting.
MAX_COMBAT_SILENCE = 20

# A caster is allowed some off-school casting: an instant to finish something, a school it was
# forced into. Below this share of its own school's spells, the rotation is not following the
# build.
MIN_OWN_SCHOOL_SHARE = 0.60

# Which nukes belong to which mage tree, for the school check. Only the damage spells a rotation
# chooses between; utility and defensive spells say nothing about the build.
MAGE_SCHOOLS = {
    "Fire":  {"Fireball", "Fire Blast", "Scorch", "Pyroblast", "Blast Wave", "Flamestrike"},
    "Frost": {"Frostbolt", "Cone of Cold", "Blizzard", "Frost Nova"},
}

TS = re.compile(r"^\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d) ")
BOT = re.compile(r"bot='([A-Za-z0-9]+)'")
STARTED = re.compile(r"ranged bot='([A-Za-z0-9]+)'.*started '([^']+)'")
CAST = re.compile(r"cast bot='([A-Za-z0-9]+)'.*spell='([^']+)'\((\d+)\) result=ok")


def seconds(line):
    match = TS.match(line)
    if not match:
        return None
    return int(match.group(1)) * 3600 + int(match.group(2)) * 60 + int(match.group(3))


def roster(harness):
    text = harness.run(f"harness exec {LEADER} partybot list", allow_failure=True) or ""
    return dict(re.findall(r"^\s*(\w+)\s+.*?class=(\d+)", text, re.M))


def clear_roster(harness):
    deadline = time.time() + 180.0
    while time.time() < deadline:
        current = roster(harness)
        if not current:
            return
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)


def build_group(harness, size):
    """One tank, one healer, damage for the rest. Same shape as the threat suite's, and for the
    same reason: ask by how many have been requested rather than how many have arrived, or a slow
    spawn is re-requested and the group comes out with two tanks."""
    wanted = ["tank", "healer"] + [DAMAGE_CLASSES[i % len(DAMAGE_CLASSES)]
                                   for i in range(size - 2)]
    issued = 0
    deadline = time.time() + GROUP_TIMEOUT
    while time.time() < deadline:
        current = roster(harness)
        if len(current) >= size:
            return current, None
        if issued < size and len(current) >= issued:
            harness.run(f"harness exec {LEADER} partybot add {wanted[issued]}",
                        allow_failure=True)
            issued += 1
        time.sleep(POLL)
    return None, f"only {len(roster(harness))} of {size} bots joined"


def read_new_lines(path, offset):
    """Everything written since the offset, and the new offset."""
    with open(path, "r", errors="ignore") as handle:
        handle.seek(offset)
        return handle.read().split("\n"), handle.tell()


# -- the invariants ------------------------------------------------------------


def check_autorepeat_restarts(lines):
    """A shot started over and over with no tick reporting it running."""
    restarts = defaultdict(int)
    worst = {}
    for line in lines:
        match = STARTED.search(line)
        if match:
            key = (match.group(1), match.group(2))
            restarts[key] += 1
            worst[key] = max(worst.get(key, 0), restarts[key])
            continue
        # Any tick that reports the autorepeat actually running clears the count: the shot took.
        if " tick bot=" in line and "autorepeat=1" in line:
            bot = BOT.search(line)
            if bot:
                for key in list(restarts):
                    if key[0] == bot.group(1):
                        restarts[key] = 0

    failures = []
    for (bot, spell), count in sorted(worst.items(), key=lambda kv: -kv[1]):
        if count > MAX_AUTOREPEAT_RESTARTS:
            failures.append(f"{bot} restarted '{spell}' {count} times without it ever running")
    return failures


def check_combat_silence(lines):
    """A bot that says nothing while the group is fighting."""
    fighting = set()
    spoke = defaultdict(set)
    bots = set()
    for line in lines:
        when = seconds(line)
        if when is None:
            continue
        bot = BOT.search(line)
        if not bot:
            continue
        bots.add(bot.group(1))
        spoke[bot.group(1)].add(when)
        if " tick bot=" in line and "victim='" in line and "victim='none'" not in line:
            fighting.add(when)

    if not fighting:
        return ["no second of the run shows any bot in combat; the fight never happened"]

    failures = []
    for bot in sorted(bots):
        longest = current = 0
        start = worst_start = None
        for when in sorted(fighting):
            if when not in spoke[bot]:
                if current == 0:
                    start = when
                current += 1
                if current > longest:
                    longest, worst_start = current, start
            else:
                current = 0
        if longest > MAX_COMBAT_SILENCE:
            stamp = "%02d:%02d:%02d" % (worst_start // 3600, worst_start % 3600 // 60,
                                        worst_start % 60)
            failures.append(f"{bot} logged nothing for {longest}s of combat, from {stamp}")
    return failures


def check_school_matches_spec(harness, lines, members):
    """A mage casting the other tree's nukes.

    The build is read from the server rather than guessed from the log, because a rotation that
    has stopped consulting the talents is exactly the bug being looked for and the log alone
    cannot distinguish it from a mage that really is frost.
    """
    casts = defaultdict(lambda: defaultdict(int))
    for line in lines:
        match = CAST.search(line)
        if match:
            casts[match.group(1)][match.group(2)] += 1

    failures = []
    for name in sorted(members):
        spell_counts = casts.get(name)
        if not spell_counts:
            continue

        by_school = {school: sum(n for s, n in spell_counts.items() if s in spells)
                     for school, spells in MAGE_SCHOOLS.items()}
        if sum(by_school.values()) < 10:
            continue                      # not a mage, or too few casts to judge

        trees = harness.talents(name).get("trees", {})
        spent = {school: points for school, points in trees.items() if school in MAGE_SCHOOLS}
        if not spent or max(spent.values()) == 0:
            continue

        build = max(spent, key=spent.get)
        total = sum(by_school.values())
        share = by_school.get(build, 0) / float(total)
        if share < MIN_OWN_SCHOOL_SHARE:
            other = ", ".join(f"{s}={n}" for s, n in sorted(by_school.items()))
            failures.append(
                f"{name} is {build} ({spent[build]} points) but only {share:.0%} of its "
                f"{total} nukes were {build} ({other})")
    return failures


def check_rotation_ran(lines):
    """A bot that held a live target and never had its rotation asked.

    The server reports this one itself, which is the whole point of it: the condition is checked
    where it can be seen -- immediately after the dispatch, while the flags still describe that
    tick -- rather than inferred here from the absence of casts. Absence of casts is what the log
    showed for fifteen passes while the cause stayed invisible.

    `norotation` means no rotation ran at all, which is structural and always a bug. `stalled`
    means the rotation ran and declined everything for ten seconds, which is usually tuning. They
    are separated because the first is never acceptable and the second sometimes is.
    """
    failures = []

    seen = {}
    for line in lines:
        if "] norotation bot=" not in line:
            continue
        bot = BOT.search(line)
        gate = re.search(r"gate=(\w+)", line)
        if not bot:
            continue
        key = (bot.group(1), gate.group(1) if gate else "unknown")
        seen[key] = seen.get(key, 0) + 1

    for (bot, gate), count in sorted(seen.items(), key=lambda kv: -kv[1]):
        failures.append(
            f"{bot} held a target with no rotation running, {count} report(s), gate={gate}")

    stalls = {}
    for line in lines:
        if "] stalled bot=" not in line:
            continue
        bot = BOT.search(line)
        held = re.search(r"for=(\d+)ms", line)
        if not bot:
            continue
        ms = int(held.group(1)) if held else 0
        stalls[bot.group(1)] = max(stalls.get(bot.group(1), 0), ms)

    for bot, ms in sorted(stalls.items(), key=lambda kv: -kv[1]):
        failures.append(f"{bot} ran its rotation but chose nothing for {ms / 1000.0:.0f}s")

    return failures


# -- driver --------------------------------------------------------------------


def run(harness, size, duration, log_path):
    harness.run(f"harness exec {LEADER} revive", allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d" % ((LEADER,) + STAGING),
                allow_failure=True)
    time.sleep(POLL)
    clear_roster(harness)

    members, error = build_group(harness, size)
    if error:
        return [error]

    # The leader cannot be allowed to die: everything is addressed through its session, and its
    # death turns any failure here into a harness failure wearing this suite's name.
    harness.run(f"harness exec {LEADER} modify hp 500000 500000", allow_failure=True)

    left_over = harness.despawn(LEADER, PUNCHING_BAG)
    if left_over:
        print(f"  removed {left_over} target(s) left standing by an earlier run", flush=True)

    # Only what this run writes. The file holds every earlier session, and a suite that read the
    # whole of it would report the bug it was written for long after it was fixed.
    offset = os.path.getsize(log_path)

    harness.run(f"harness exec {LEADER} npc summon {PUNCHING_BAG}", allow_failure=True)
    print(f"  fighting for {duration:.0f}s", flush=True)
    time.sleep(duration)

    lines, _ = read_new_lines(log_path, offset)
    print(f"  read {len(lines)} new log lines\n", flush=True)

    failures = []
    for label, found in (
        ("rotation", check_rotation_ran(lines)),
        ("autorepeat", check_autorepeat_restarts(lines)),
        ("silence", check_combat_silence(lines)),
        ("school", check_school_matches_spec(harness, lines, members)),
    ):
        for item in found:
            failures.append(f"[{label}] {item}")
        if not found:
            print(f"  ok   {label}", flush=True)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=5, help="bots besides the leader")
    parser.add_argument("--duration", type=float, default=120.0,
                        help="seconds to watch the fight")
    parser.add_argument("--log", default=DEFAULT_LOG, help="server log to read")
    args = parser.parse_args()

    if not os.path.exists(args.log):
        print(f"FAIL  no log at {args.log}; run this on the server host")
        return 1

    harness = Harness.from_env()
    harness.login(LEADER)
    harness.run(f"harness exec {LEADER} character level {LEADER} 60", allow_failure=True)

    print(f"{args.size} bots on one target for {args.duration:.0f}s\n", flush=True)

    try:
        failures = run(harness, args.size, args.duration, args.log)
    finally:
        clear_roster(harness)
        harness.run(f"harness exec {LEADER} revive", allow_failure=True)

    if failures:
        print()
        for item in failures:
            print(f"FAIL  {item}")
        return 1

    print("\nPASS  every bot ran its rotation, kept its autorepeat, and fought in school")
    return 0


if __name__ == "__main__":
    sys.exit(main())
