#!/usr/bin/env python3
"""Prove that a bot which dies during a fight can be brought back before the fight ends.

Wipe recovery already works, and it is the wrong tool for this. A corpse run is what happens
when an attempt has been lost; the interesting case is the one where it has not, and a single
death in the first minute quietly decides the rest of the pull. Until now every bot that died
mid-encounter stayed dead until the pull ended, however many druids were standing next to it.

Two mechanisms, tested apart so that one cannot be mistaken for the other:

  rebirth        a druid resurrects a dead group member while the fight is still going on.
  reincarnation  a shaman with no help available stands itself back up, and spends its Ankh.

Both need something to hold the group in combat for long enough to observe, which is what the
summoned boss is for. It is a temporary summon, so nothing is written to the creature table,
and it is a raid boss because the requirement is a health pool deep enough that the fight is
still running a minute later rather than anything the boss does.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_combat_resurrection.py
"""

import argparse
import sys
import time

from vmangos_harness import Harness, CommandError

LEADER = "Harnessbot"

# Player::DeathState, as reported by .harness info.
ALIVE = 0
CORPSE = 2

CLASS_SHAMAN = 7
CLASS_DRUID = 11

# Open ground in the Barrens, well away from anything that would join in.
STAGING = (-600.0, -2515.0, 92.0, 1)

# A level 60 with a hundred times the usual health and entirely ordinary damage, which is the
# combination this needs and which almost nothing in the game has: the fight lasts minutes and
# kills nobody who was not killed on purpose. A real raid boss was tried first and was worse in
# the way that matters, since it killed the druid under test and reported that as the druid
# declining to resurrect anyone.
PUNCHING_BAG = 11080

ANKH = "17030"

GROUP_TIMEOUT = 180.0
COMBAT_TIMEOUT = 60.0
RES_TIMEOUT = 90.0
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
    deadline = time.time() + 120.0
    while time.time() < deadline:
        current = roster(harness)
        if not current:
            return
        for name in current:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)


def recruit(harness, wanted):
    """Fill the group with these classes, in order, and hand back who turned up as what.

    An add does not always take, so this tops up rather than asking once. Classes are counted
    rather than matched by name, since the bot names itself and there is no way to ask for a
    particular one back.
    """
    deadline = time.time() + GROUP_TIMEOUT
    while time.time() < deadline:
        current = roster(harness)
        if len(current) >= len(wanted):
            return current

        harness.run(f"harness exec {LEADER} partybot add {wanted[len(current)]}",
                    allow_failure=True)
        time.sleep(POLL * 2)

    return roster(harness)


def of_class(members, class_id):
    return sorted(n for n, m in members.items() if int(m["class"]) == class_id)


def group_in_combat(harness):
    """Whether anyone in the group is still fighting, in one call rather than one per bot."""
    current = info(harness, LEADER)
    if not current:
        return False
    return any(int(m.get("incombat", 0)) for m in current["members_detail"])


def start_fight(harness):
    """Summon the boss onto the leader and wait for the group to be pulled into combat."""
    # Clear the field first. A bag from an earlier run is still standing and still in combat
    # with a leader who cannot die and so never drops it, and the group would assist against
    # that one instead of the one summoned here.
    harness.despawn(LEADER, PUNCHING_BAG)
    harness.run(f"harness exec {LEADER} npc summon {PUNCHING_BAG}", allow_failure=True)

    deadline = time.time() + COMBAT_TIMEOUT
    while time.time() < deadline:
        if group_in_combat(harness):
            return None
        time.sleep(POLL)
    return "nothing pulled the group into combat"


def keep_standing(harness, who):
    """Make this bot unkillable and keep its mana topped up.

    Only ever applied to the bot doing the resurrecting, never to the one being resurrected.
    Without it the test measures attrition instead: the first attempt at this used a real raid
    boss and reported that the druid declined to cast Rebirth, when what happened is that the
    druid healed itself to empty and then died.
    """
    harness.run(f"harness exec {who} modify hp 200000 200000", allow_failure=True)
    harness.run(f"harness exec {who} modify mana 100000 100000", allow_failure=True)


def wait_for_res(harness, who, helper=None):
    """Wait for this bot to be alive again, and say whether the fight was still on when it was.

    Standing back up after the fight ends is the old behaviour working normally, not this one,
    so the combat state is sampled alongside rather than afterwards: by the time a resurrection
    is visible the fight may already be over.
    """
    deadline = time.time() + RES_TIMEOUT
    started = time.time()
    while time.time() < deadline:
        if helper:
            keep_standing(harness, helper)

        fighting = group_in_combat(harness)
        current = info(harness, who)
        if current and int(current["deathstate"]) == ALIVE:
            return time.time() - started, fighting, None
        if not fighting:
            return None, False, "the fight ended before anyone was resurrected"
        time.sleep(POLL)

    # selfres says whether the engine is still holding a charge for this death. Still set means
    # nothing tried to spend it; cleared on a bot that is still dead would mean it was spent on
    # a cast that failed, and the two want opposite fixes.
    current = info(harness, who) or {}
    return None, False, (f"still dead {RES_TIMEOUT:.0f}s later "
                         f"(deathstate={current.get('deathstate')} "
                         f"selfres={current.get('selfres')})")


def ankh_count(harness, who):
    current = info(harness, who)
    if not current:
        return 0
    return sum(int(i.get("count", 0)) for i in current.get("items", [])
               if i.get("entry") == ANKH)


def stage(harness, wanted):
    """Put a fresh group of these classes on open ground, in a fight, and hand it back."""
    harness.run(f"harness exec {LEADER} revive", allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d" % ((LEADER,) + STAGING),
                allow_failure=True)
    time.sleep(POLL)
    clear_roster(harness)

    members = recruit(harness, wanted)
    if len(members) < len(wanted):
        return None, f"only {len(members)} of {len(wanted)} bots joined"

    return members, None


def test_rebirth(harness):
    """A druid resurrects a dead group member without waiting for the fight to end."""
    members, error = stage(harness, ["druid", "warrior", "warrior", "mage"])
    if error:
        return error

    druids = of_class(members, CLASS_DRUID)
    if not druids:
        return "no druid joined"

    # The druid's role is deliberately left as it rolled. Rebirth cannot be cast in any form,
    # and a feral or balance druid is the ordinary case in a raid rather than an awkward one,
    # so forcing a healer here would test only the arrangement that was never in doubt.
    victims = [n for n in sorted(members) if n not in druids]
    if not victims:
        return "nobody to kill who was not the druid"
    victim = victims[0]

    error = start_fight(harness)
    if error:
        return error

    keep_standing(harness, druids[0])
    harness.run(f"harness exec {victim} die", allow_failure=True)
    print(f"  {victim} killed mid-fight, {len(druids)} druid(s) present", flush=True)

    elapsed, fighting, error = wait_for_res(harness, victim, helper=druids[0])
    if error:
        # Say what the druid was doing instead. A druid that is dead, out of mana or stuck in
        # bear form has each failed for a different reason, and "still dead" describes all
        # three equally badly.
        for name in druids:
            current = info(harness, name) or {}
            print(f"  {name}: alive={current.get('alive')} "
                  f"form={current.get('form')} "
                  f"mana={current.get('power')}/{current.get('maxpower')} "
                  f"incombat={current.get('incombat')}", flush=True)
        return error
    if not fighting:
        return f"{victim} came back after {elapsed:.0f}s, but the fight was already over"

    print(f"  pass  {victim} was resurrected {elapsed:.0f}s later, still in combat\n",
          flush=True)
    return None


def test_reincarnation(harness):
    """A shaman with nobody able to help stands itself up, and the Ankh is gone afterwards."""
    # Three shamans for one test, because Reincarnation is a talent and the spec is rolled per
    # spawn. Asking one shaman and reporting what it happened to roll would be a test of the
    # spec generator wearing this test's name.
    members, error = stage(harness, ["shaman", "shaman", "shaman", "warrior"])
    if error:
        return error

    shamans = of_class(members, CLASS_SHAMAN)
    if not shamans:
        return "no shaman joined"

    # Whichever shaman is actually holding an Ankh. Reincarnation is a talent, so a spec that
    # did not take it produces a shaman that is expected not to come back, and failing on that
    # would be reporting the spec roll rather than anything about resurrection.
    ready = [n for n in shamans if ankh_count(harness, n) > 0]
    if not ready:
        return "no shaman spawned with an Ankh, so none of them took Reincarnation"
    victim = ready[0]

    error = start_fight(harness)
    if error:
        return error

    before = ankh_count(harness, victim)
    harness.run(f"harness exec {victim} die", allow_failure=True)
    print(f"  {victim} killed mid-fight, carrying {before} Ankh", flush=True)

    elapsed, fighting, error = wait_for_res(harness, victim)
    if error:
        return error
    if not fighting:
        return f"{victim} came back after {elapsed:.0f}s, but the fight was already over"

    after = ankh_count(harness, victim)
    if after >= before:
        return (f"{victim} reincarnated but still carries {after} Ankh, "
                f"so the reagent was never spent")

    print(f"  pass  {victim} reincarnated {elapsed:.0f}s later, still in combat, "
          f"Ankh {before} -> {after}\n", flush=True)
    return None


TESTS = {
    "rebirth": test_rebirth,
    "reincarnation": test_reincarnation,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", default=[], choices=sorted(TESTS),
                        help="run just this scenario; repeatable")
    args = parser.parse_args()

    chosen = args.only or sorted(TESTS)

    harness = Harness.from_env()
    harness.login(LEADER)
    harness.run(f"harness exec {LEADER} character level {LEADER} 60", allow_failure=True)

    failures = []
    try:
        for name in chosen:
            print(f"{name}", flush=True)
            error = TESTS[name](harness)
            if error:
                failures.append((name, error))
                print(f"  FAIL  {error}\n", flush=True)
    finally:
        clear_roster(harness)
        harness.run(f"harness exec {LEADER} revive", allow_failure=True)
        harness.run("harness exec %s go xyz %f %f %f %d" % ((LEADER,) + STAGING),
                    allow_failure=True)

    print(f"{len(chosen) - len(failures)} of {len(chosen)} scenarios resurrected mid-fight")
    for name, error in failures:
        print(f"FAIL  {name:<16} {error}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
