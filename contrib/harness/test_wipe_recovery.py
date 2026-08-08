#!/usr/bin/env python3
"""Check that party bots recover from death on their own.

Two cases matter and they fail in opposite directions. A partial wipe should be
recovered by the surviving healer, with nobody releasing. A full wipe leaves nobody
to cast anything, so bots have to release and get themselves back on their feet,
which is the case that used to leave the whole group on the floor permanently.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a
GM account: python3 test_wipe_recovery.py
"""

import sys
import time

from vmangos_harness import Harness

LEADER = "Harnessbot"

# Player::DeathState, as reported by .harness info.
ALIVE = 0
CORPSE = 2
GHOST = 3

CLASS_PRIEST = 5

# Open ground in the Barrens, a short walk from its graveyard. Recovery now costs a corpse
# run, so where the group dies decides how long the test takes: left to inherit whatever
# position the leader was last driven to, this suite once attempted a seventeen hundred yard
# run across Silithus and timed out while the bots were still walking, correctly.
HOME = (-600, -2515, 92, 1)

# Must exceed PartyBot.DeathRecoveryTimeout plus a margin for the ten second cast.
RECOVERY_TIMEOUT = 180.0


def bots(harness):
    """Group members other than the leader, keyed by name."""
    info = harness.info(LEADER)
    if not info:
        return {}
    return {m["name"]: m for m in info["members_detail"] if m["name"] != LEADER}


def wait_until(harness, predicate, timeout, description, interval=2.0):
    """Poll the roster until predicate holds. Returns an error string, or None."""
    deadline = time.time() + timeout
    members = {}
    while time.time() < deadline:
        members = bots(harness)
        if members and predicate(members):
            return None
        time.sleep(interval)

    states = {n: m["deathstate"] for n, m in sorted(members.items())}
    return f"timed out waiting for {description}; deathstates were {states}"


def states(members):
    return [int(m["deathstate"]) for m in members.values()]


def reset(harness):
    """Clear the roster.

    partybot remove only removes the selected player, and a character with nothing
    selected counts as selecting itself, so each bot has to be told individually.
    """
    deadline = time.time() + 90.0
    while time.time() < deadline:
        current = bots(harness)
        if not current:
            return None
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(3.0)
    return f"roster would not clear; {sorted(bots(harness))} remain"


def setup(harness, composition):
    # The leader is an ordinary headless character with no recovery of its own, so a
    # previous full wipe would otherwise leave it dead for every later run.
    harness.execute(LEADER, "revive")
    harness.execute(LEADER, "go xyz %d %d %d %d" % HOME)
    time.sleep(2.0)

    error = reset(harness)
    if error:
        return {}, error

    for role in composition:
        harness.execute(LEADER, f"partybot add {role}")
        time.sleep(1.0)

    expected = len(composition)
    error = wait_until(
        harness,
        lambda m: len(m) == expected and all(s == ALIVE for s in states(m)),
        90.0,
        f"{expected} live bots",
    )
    return bots(harness), error


def kill(harness, names):
    """Kill the named bots and confirm they actually went down.

    Without the confirmation a failure to land the kill would look identical to a
    perfect recovery.
    """
    for name in names:
        harness.execute(name, "die")

    return wait_until(
        harness,
        lambda m: all(int(m[n]["deathstate"]) != ALIVE for n in names if n in m),
        30.0,
        "the kills to land",
        interval=0.5,
    )


def test_partial_wipe(harness):
    """A surviving healer should resurrect the dead, so nobody releases."""
    # A priest rather than "healer", which also rolls druids, whose only resurrection
    # is Rebirth on a thirty minute cooldown.
    members, error = setup(harness, ["priest", "dps", "dps"])
    if error:
        return [f"partial wipe setup: {error}"]

    healer = next(n for n, m in members.items() if m["class"] == str(CLASS_PRIEST))
    victims = [n for n in members if n != healer]

    error = kill(harness, victims)
    if error:
        return [f"partial wipe: {error}"]

    released = set()

    def recovered(current):
        released.update(
            n for n in victims if int(current[n]["deathstate"]) == GHOST
        )
        return all(int(current[n]["deathstate"]) == ALIVE for n in victims)

    error = wait_until(harness, recovered, RECOVERY_TIMEOUT, "a healer resurrection")

    failures = []
    if error:
        failures.append(f"partial wipe: {error}")
    if released:
        failures.append(
            f"partial wipe: {sorted(released)} released despite a live healer in range"
        )
    return failures


def test_full_wipe(harness):
    """With nobody left standing, bots must release and recover unaided.

    Either route counts. Walking back to the corpse is the good one and the spirit
    healer is the deadlock breaker behind it; what matters is that nobody is left on
    the floor.
    """
    members, error = setup(harness, ["priest", "dps", "dps"])
    if error:
        return [f"full wipe setup: {error}"]

    error = kill(harness, list(members))
    if error:
        return [f"full wipe: {error}"]
    harness.execute(LEADER, "die")

    # Releasing is the part that never used to happen, so assert it on the way through
    # rather than only checking that everyone ended up alive somehow.
    released = set()

    def all_released(current):
        released.update(n for n in current if int(current[n]["deathstate"]) == GHOST)
        return released >= set(members)

    error = wait_until(harness, all_released, 90.0, "every bot to release", interval=1.0)
    failures = []
    if error:
        failures.append(f"full wipe: {error}")
        return failures

    error = wait_until(
        harness,
        lambda m: all(s == ALIVE for s in states(m)),
        RECOVERY_TIMEOUT,
        "every bot to get back on its feet",
    )
    if error:
        failures.append(f"full wipe: {error}")
    return failures


def main():
    harness = Harness.from_env()
    harness.login(LEADER)
    harness.execute(LEADER, f"character level {LEADER} 60")

    failures = []
    for test in (test_partial_wipe, test_full_wipe):
        print(f"running {test.__name__}")
        results = test(harness)
        print(f"  {'ok' if not results else 'failed'}")
        failures.extend(results)

    reset(harness)
    harness.execute(LEADER, "revive")

    for failure in failures:
        print(f"FAIL: {failure}")
    print("PASS" if not failures else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
