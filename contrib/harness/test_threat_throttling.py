#!/usr/bin/env python3
"""Watch a group fight and require that the tank keeps control of the target.

Threat is the quantity that decides who a boss hits, and until now nothing in the bot code
consulted it except a single check against area spells. A damage dealer cast whatever came next
in its if-chain and the mob went to whoever generated the most, which in a raid loses the
attempt for a reason that has nothing to do with the encounter being tested.

What is asserted is control, not perfection. A damage dealer that clips the tank and is
immediately overtaken again costs the raid nothing, and a throttle tuned so that never happens
is a throttle that has thrown away damage to buy something worthless. What loses raids is a
damage dealer that goes past the tank and stays past it, so the measurement is how long the mob
spends off the tank: a little is fine, a lot is the failure. Peaks per bot are reported for the
opposite reason, since a throttle that works by never attacking would satisfy every assertion
here and show up as a raid idling at half the tank's threat.

The opening is asserted separately because the ratio cannot govern it. A share of the tank's
threat is meaningless while the tank has almost none, so damage dealers hold for the first few
seconds exactly as a real raid does, and the list is read as the hold expires to require that
this bought a tank in front.

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

# Where the mob changes its mind, as a share of the current victim's threat: 110 percent for an
# attacker it can reach with a melee swing, 130 for one at range. Which of the two applies is
# read off the mob per sample rather than guessed from the class, because the rule follows
# position and not role: a caster that has wandered into reach is judged at 110 like everyone
# else, and calling it ranged declares it safe at 129 when it is already taking the mob.
# Reported against rather than asserted against, because crossing the line briefly is allowed
# and holding the mob is what is actually required.
PULL_RATIO_MELEE = 1.10
PULL_RATIO_RANGED = 1.30

CLASS_WARRIOR = 1

# Must match PB_THREAT_PULL_HOLD_SECONDS in PartyBotAI.cpp.
PULL_HOLD = 8

# How much of the fight the tank is allowed to not be holding the target. A brief loss is a
# damage dealer clipping past and being overtaken again, which is what a throttle tuned for
# damage rather than for tidiness looks like. A long one is the raid being eaten.
MAX_LOSS_STREAK = 15.0
MAX_LOSS_SHARE = 0.15

# Below this there is not enough settled fight to call anything a share of it. A full raid kills
# the bag in about ninety seconds, so at large sizes the window is short whatever `--duration`
# says, and a run that measured ten seconds should say so rather than pass.
MIN_WATCHED = 30.0

CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest",
               7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}

# How long the pull is given to settle, in the mob's own seconds of combat rather than the
# suite's. The mob starts on whoever it noticed first, which need not be the tank, and the tank
# pulling it back is the thing working rather than breaking. Forty-five was the figure before
# the opening hold existed and the pull was genuinely chaotic; it now costs more than it buys,
# since a full raid kills the bag in about ninety seconds and half the fight was being skipped.
SETTLE = 20

GROUP_TIMEOUT = 240.0
COMBAT_TIMEOUT = 60.0
POLL = 2.0


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
    """One tank, one healer, and damage for the rest, topping up rather than asking once.

    Asks by how many have been *requested*, not by how many have arrived. A bot takes a moment
    to appear in the roster, so indexing by roster size re-requests whatever is still in flight
    and the group ends up with two tanks, which reads later as a suite that cannot tell which
    warrior it is measuring.
    """
    wanted = ["tank", "healer"] + [DAMAGE_CLASSES[i % len(DAMAGE_CLASSES)]
                                   for i in range(size - 2)]

    issued = 0
    deadline = time.time() + GROUP_TIMEOUT
    while time.time() < deadline:
        current = roster(harness)
        if len(current) >= size:
            return current, None
        # Only ask for the next one once everything already asked for has turned up, so a slow
        # spawn delays the group rather than duplicating it.
        if issued < size and len(current) >= issued:
            harness.run(f"harness exec {LEADER} partybot add {wanted[issued]}",
                        allow_failure=True)
            issued += 1
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
    in_reach = {}
    seen_at = {}
    opening_done = False
    lead = {}
    first_seen = None

    # Loss of control, measured in seconds off the tank rather than counted in incidents: who
    # clipped past matters much less than for how long they stayed there.
    watched = 0.0
    lost = 0.0
    streak = 0.0
    worst_streak = 0.0
    culprits = {}
    last = None
    ended_early = None

    started = time.time()
    while time.time() - started < duration:
        reading = read_threat(harness, probe)
        if not reading:
            # The bot doing the looking may have died or stopped attacking, which says nothing
            # about the fight. Find another pair of eyes before concluding it is over.
            probe, error = find_probe(harness, sorted(members))
            if error:
                # Nobody is fighting anything, which at this size means the bag is dead: a
                # full raid gets through it in about a minute and a half. Worth saying, since
                # otherwise a short measurement looks like a long one.
                ended_early = time.time() - started
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

        now = time.time()
        if settled:
            # Charge the gap since the previous reading to whatever the mob was doing during
            # it, so the figures are seconds of fight rather than a count of samples.
            elapsed = now - last if last is not None else 0.0
            watched += elapsed
            if victim and victim != tank:
                lost += elapsed
                streak += elapsed
                worst_streak = max(worst_streak, streak)
                culprits[victim] = culprits.get(victim, 0.0) + elapsed
            else:
                streak = 0.0
        last = now

        for hostile in reading["hostiles"]:
            if not settled:
                continue

            # Recorded only once the pull has settled, for the same reason the assertions are.
            # In the opening seconds the tank's threat is near nothing and every ratio against
            # it is enormous, so peaks taken from there describe arithmetic rather than
            # behaviour.
            peak[hostile["name"]] = max(peak.get(hostile["name"], 0.0),
                                        hostile["percent"] / 100.0)

            # Which rule the mob is judging this one by, taken from the mob rather than guessed
            # from the class. A caster that has drifted inside melee reach flips at 110 percent
            # like anything else, and reading that off the class calls it safe at 129.
            if hostile["melee"]:
                in_reach[hostile["name"]] = in_reach.get(hostile["name"], 0) + 1
            seen_at[hostile["name"]] = seen_at.get(hostile["name"], 0) + 1

        # Watch the opening closely, then closely enough that a brief loss of the target is a
        # measurement rather than a rounding error.
        time.sleep(0.5 if not opening_done else POLL)

    for name in sorted(peak, key=lambda n: -peak[n]):
        what = CLASS_NAMES.get(classes.get(name), "not in the group")
        if name == tank:
            marker = " <- the tank"
        elif name in classes:
            # The strict rule applies if the mob was ever able to swing at it, since one
            # reading inside reach at the wrong moment is all a handover needs.
            flip = PULL_RATIO_MELEE if in_reach.get(name) else PULL_RATIO_RANGED
            marker = f" <- past its {flip * 100:.0f}% flip" if peak[name] >= flip else ""
        else:
            marker = ""
        share = in_reach.get(name, 0) / max(seen_at.get(name, 1), 1)
        reach = f"  in reach {share * 100:3.0f}% of the time" if share else ""
        print(f"    {name:<22} {what:<8} peaked at {peak[name] * 100:5.0f}%{reach}{marker}",
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

    share = lost / watched if watched else 0.0
    took = ", ".join(f"{n} for {t:.0f}s" for n, t in
                     sorted(culprits.items(), key=lambda kv: -kv[1]))
    if worst_streak > MAX_LOSS_STREAK:
        return (f"{tank} lost the target for {worst_streak:.0f}s at a stretch, which is not a "
                f"clip past it but a handover: {took}")
    if share > MAX_LOSS_SHARE:
        return (f"{tank} held the target for only {(1 - share) * 100:.0f} percent of the "
                f"fight, losing {lost:.0f}s of {watched:.0f}s to {took}")

    # A throttle that works by never attacking would pass everything above, so say how close
    # anyone actually got. Nobody near the ceiling means the fight never tested it.
    if watched < MIN_WATCHED:
        return (f"only {watched:.0f}s of settled fight to judge, which is not enough to say "
                f"anything about control" +
                (f"; the target died {ended_early:.0f}s in" if ended_early else ""))

    contenders = [n for n, r in peak.items() if n != tank and r >= 0.9]
    runner_up = max((t for n, t in lead.items() if n != tank), default=0.0)
    control = "never lost it" if not lost else (
        f"lost it for {lost:.0f}s, longest {worst_streak:.0f}s at a stretch, to {took}")
    death = f", the target dying {ended_early:.0f}s in" if ended_early else ""
    print(f"  pass  {PULL_HOLD}s of holding left {tank} on {lead.get(tank, 0.0):.0f} threat "
          f"against a next best of {runner_up:.0f}; over {watched:.0f}s of settled fight{death} "
          f"it {control}, with {len(contenders)} member(s) pushing to within a tenth of its "
          f"threat\n", flush=True)
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
