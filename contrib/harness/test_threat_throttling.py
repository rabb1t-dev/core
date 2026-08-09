#!/usr/bin/env python3
"""Watch a group fight and require that nobody takes the target off the tank.

Threat is the quantity that decides who a boss hits, and until now nothing in the bot code
consulted it except a single check against area spells. A damage dealer cast whatever came next
in its if-chain and the mob went to whoever generated the most, which in a raid loses the
attempt for a reason that has nothing to do with the encounter being tested.

Vanilla flips the target at 110 percent of the current victim's threat for an attacker in melee
range and 130 percent for one at range. This asserts two things over a long fight: that no
group member crosses its own threshold, and that the mob is still on the same tank at the end.

The second assertion is the one that would notice a throttle that works by never attacking. So
the peak ratio reached by each bot is reported too: bots that sat far below the ceiling prove
nothing either way, and bots parked just under it are the throttle doing its job.

A third assertion covers the opening, which the ratio cannot. A share of the tank's threat is
meaningless while the tank has almost none, so damage dealers hold for the first few seconds
exactly as a real raid does. What that has to buy is a tank in front when they start, so the
list is read at the moment the hold expires and the tank is required to be leading it.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_threat_throttling.py
"""

import argparse
import sys
import time

from vmangos_harness import Harness, CommandError

LEADER = "Harnessbot"

STAGING = (-600.0, -2515.0, 92.0, 1)

# A level 60 with a hundred times the usual health and ordinary damage, so the fight runs long
# enough for threat to separate without killing the group while it does.
PUNCHING_BAG = 11080

# Where the mob changes its mind, as a share of the current victim's threat. Melee is the
# stricter of the two and is what an unknown class is measured against.
PULL_RATIO_MELEE = 1.10
PULL_RATIO_RANGED = 1.30

CLASS_WARRIOR = 1
MELEE_CLASSES = {1, 4, 7, 11}   # warrior, rogue, shaman, druid: assume the harder threshold

# Must match PB_THREAT_PULL_HOLD_SECONDS in PartyBotAI.cpp.
PULL_HOLD = 8

CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest",
               7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}

# How long the pull is given to settle, in the mob's own seconds of combat rather than the
# suite's. The mob starts on whoever it noticed first, which need not be the tank, and the tank
# pulling it back is the thing working rather than breaking.
SETTLE = 45

GROUP_TIMEOUT = 240.0
COMBAT_TIMEOUT = 60.0
POLL = 3.0


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
            return
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)


# Damage is asked for by class rather than by role so that the tank stays the only warrior in
# the group. Nothing reports a bot's role from outside, and `dps` rolls a warrior often enough
# that "the tank" would otherwise be a guess between two of them.
DAMAGE_CLASSES = ["mage", "hunter", "warlock", "rogue", "priest"]


def build_group(harness, size):
    """One tank, one healer, and damage for the rest, topping up rather than asking once."""
    wanted = ["tank", "healer"] + [DAMAGE_CLASSES[i % len(DAMAGE_CLASSES)]
                                   for i in range(size - 2)]

    deadline = time.time() + GROUP_TIMEOUT
    while time.time() < deadline:
        current = roster(harness)
        if len(current) >= size:
            return current, None
        harness.run(f"harness exec {LEADER} partybot add {wanted[len(current)]}",
                    allow_failure=True)
        time.sleep(POLL)

    return None, f"only {len(roster(harness))} of {size} bots joined"


def read_threat(harness, who):
    try:
        return harness.threat(who)
    except CommandError:
        return None


def find_probe(harness, names):
    """A bot whose target's threat list can be read, which is any bot currently attacking.

    Polled tightly rather than at the suite's usual interval, because the opening hold is only
    eight seconds long and a first reading taken after it has expired cannot see it happen.
    """
    deadline = time.time() + COMBAT_TIMEOUT
    while time.time() < deadline:
        for name in names:
            if read_threat(harness, name):
                return name, None
        time.sleep(0.5)
    return None, "nothing pulled the group into combat"


def run(harness, size, duration):
    harness.run(f"harness exec {LEADER} revive", allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d" % ((LEADER,) + STAGING),
                allow_failure=True)
    time.sleep(POLL)
    clear_roster(harness)

    members, error = build_group(harness, size)
    if error:
        return error

    # The leader has to be unkillable rather than merely tough. It is the one character the
    # suite cannot lose: everything is addressed through its session, and a dead leader turns
    # a threat failure into a harness failure that looks nothing like one.
    harness.run(f"harness exec {LEADER} modify hp 500000 500000", allow_failure=True)

    # Clear the field first. The bag from a previous run is still standing and still in combat
    # with the leader, who cannot die and so never drops it, and bots assist against whatever
    # is already attacking the group. Left alone, this run would measure a fight that started
    # several minutes ago and the opening it is here to check would never happen.
    left_over = harness.despawn(LEADER, PUNCHING_BAG)
    if left_over:
        print(f"  removed {left_over} target(s) left standing by an earlier run", flush=True)

    harness.run(f"harness exec {LEADER} npc summon {PUNCHING_BAG}", allow_failure=True)

    # Read the list through whichever bot is fighting rather than through the leader. The
    # leader is a character with nobody driving it, so it never attacks anything and has no
    # victim whose threat list could be asked for, which reads as a group that never engaged.
    probe, error = find_probe(harness, sorted(members))
    if error:
        return error

    classes = {n: int(m["class"]) for n, m in members.items()}
    warriors = [n for n, c in classes.items() if c == CLASS_WARRIOR]
    if len(warriors) != 1:
        return f"expected exactly one warrior to be the tank, got {warriors}"
    tank = warriors[0]

    peak = {}
    stolen = []
    breaches = []
    opening_done = False
    lead = {}
    first_seen = None

    started = time.time()
    while time.time() - started < duration:
        reading = read_threat(harness, probe)
        if not reading:
            # The bot doing the looking may have died or stopped attacking, which says nothing
            # about the fight. Find another pair of eyes before concluding it is over.
            probe, error = find_probe(harness, sorted(members))
            if error:
                break
            continue

        # The mob's own seconds of combat, which is what the bots hold against. Wall clock
        # would do for a fight that starts when the suite says so, but this one starts when
        # the summoned mob picks someone, which is a moment the suite does not choose.
        fight = reading["summary"].get("combat", -1)
        victim = reading["summary"].get("victim")
        if first_seen is None:
            first_seen = fight

        # Read the list once, on the way past the hold, and only once: a mob that evades and
        # re-engages restarts its own clock, and by then a full list is the fight in progress
        # rather than the state the ramp handed over.
        if not opening_done and fight >= PULL_HOLD:
            opening_done = True
            lead = {h["name"]: h["threat"] for h in reading["hostiles"]
                    if h["name"] in classes}

        # The opening seconds belong to whoever the mob happened to see first, and the tank
        # taking the target off them is the system working rather than failing. Only what
        # happens after the pull has settled is evidence either way.
        settled = fight >= SETTLE if fight >= 0 else time.time() - started >= SETTLE

        if settled and victim and victim != tank and victim not in stolen:
            stolen.append(victim)

        for hostile in reading["hostiles"]:
            if not settled:
                continue

            # Recorded only once the pull has settled, for the same reason the assertions are.
            # In the opening seconds the tank's threat is near nothing and every ratio against
            # it is enormous, so peaks taken from there describe arithmetic rather than
            # behaviour.
            name = hostile["name"]
            ratio = hostile["percent"] / 100.0
            peak[name] = max(peak.get(name, 0.0), ratio)

            if name == victim or name not in classes:
                continue

            limit = (PULL_RATIO_MELEE if classes[name] in MELEE_CLASSES
                     else PULL_RATIO_RANGED)
            if ratio >= limit and name not in breaches:
                breaches.append(name)

        # Watch the opening closely and the rest of the fight cheaply.
        time.sleep(0.5 if not opening_done else POLL)

    for name in sorted(peak, key=lambda n: -peak[n]):
        marker = " <- the tank" if name == tank else ""
        what = CLASS_NAMES.get(classes.get(name), "not in the group")
        print(f"    {name:<22} {what:<8} peaked at {peak[name] * 100:5.0f}%{marker}",
              flush=True)

    # Said before the opening verdict rather than after it, because arriving late makes that
    # verdict vacuous rather than favourable: the ramp cannot be caught failing in a window
    # nobody watched.
    if first_seen is None or first_seen >= PULL_HOLD:
        return (f"never saw the pull: the group was fighting something that had been in "
                f"combat for {first_seen}s by the first reading")

    if not lead:
        return f"nobody was on the threat list {PULL_HOLD}s into the fight"

    ahead = [f"{n} on {lead[n]:.0f}" for n in lead if lead[n] > lead.get(tank, 0.0)]
    if ahead:
        return (f"the hold did not put {tank} in front: it had {lead.get(tank, 0.0):.0f} "
                f"threat when damage was released, behind {ahead}")
    if stolen:
        return f"the target was taken off the tank by {stolen}"
    if breaches:
        return f"{len(breaches)} member(s) crossed their pull threshold: {breaches}"

    # A throttle that works by never attacking would pass everything above, so say how close
    # anyone actually got. Nobody near the ceiling means the fight never tested it.
    contenders = [n for n, r in peak.items() if n != tank and r >= 0.5]
    runner_up = max((t for n, t in lead.items() if n != tank), default=0.0)
    print(f"  pass  {PULL_HOLD}s of holding left {tank} on {lead.get(tank, 0.0):.0f} threat "
          f"against a "
          f"next best of {runner_up:.0f}, and it kept the target for {duration:.0f}s while "
          f"{len(contenders)} member(s) got within half of it\n", flush=True)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=10, help="bots besides the leader")
    parser.add_argument("--duration", type=float, default=180.0,
                        help="seconds to watch the threat list")
    args = parser.parse_args()

    harness = Harness.from_env()
    harness.login(LEADER)
    harness.run(f"harness exec {LEADER} character level {LEADER} 60", allow_failure=True)

    print(f"{args.size} bots on one target for {args.duration:.0f}s\n", flush=True)

    try:
        error = run(harness, args.size, args.duration)
    finally:
        clear_roster(harness)
        harness.run(f"harness exec {LEADER} revive", allow_failure=True)

    if error:
        print(f"FAIL  {error}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
