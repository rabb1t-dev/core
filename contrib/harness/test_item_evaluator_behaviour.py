#!/usr/bin/env python3
"""Check that a bot wears the better of what it has, and declines the worse.

`test_item_evaluator.py` proves the engine reads an item the same way the Classic Gear
Ranker does. That says nothing about whether a bot acts on it. This file drives the live
path: `.harness equipnew` runs the same re-optimisation pass that trade completion runs,
over everything the bot is wearing and carrying.

Every case is a neck, because there is exactly one neck slot. A ring would prove nothing:
with two finger slots a bot can wear both the good one and the bad one, so "declined" would
have no observable meaning.

The pair is chosen so the verdict does not rest on the weight table being well tuned.
Necklace of the Dawn is 15 stamina and nothing else; the Kodobone Necklace is 7 stamina and
4 spirit. For a protection warrior that is more than twice the stamina in exchange for a
stat the spec scores at zero, so no plausible retuning reverses it.

The last case is the one that would have caught the old behaviour. Before the evaluator the
pass equipped whatever fitted, in bag order, so which neck a bot ended up wearing depended
on which one it happened to look at last. Handing over the same two items in both orders and
demanding the same answer is what separates choosing from merely equipping.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_item_evaluator_behaviour.py
"""

import sys

from roster_fixture import cleanup, give, holds, inventory, strip, summon_roster
from vmangos_harness import Harness

# Distinct names from the gear preservation suite so the two can run back to back.
LEADER = "Rgevallead"
BOT = "Rgevalone"
MEMBERS = [(LEADER, 2, 1), (BOT, 2, 1)]

BETTER = 13811  # Necklace of the Dawn, 15 stamina
WORSE = 15690   # Kodobone Necklace, 7 stamina and 4 spirit

EQUIPMENT_SLOT_NECK = 1


def main():
    harness = Harness.from_env()
    failures = []

    problem = summon_roster(harness, MEMBERS, LEADER, BOT)
    if problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        cleanup(harness, MEMBERS)
        return 1

    def optimise():
        harness.run(f"harness equipnew {BOT}")

    def neck():
        equipped, bag, mailed = inventory(harness, BOT)
        return equipped.get(EQUIPMENT_SLOT_NECK), equipped, bag, mailed

    # One: the upgrade is taken, and the neck it replaces is put away rather than deleted.
    strip(harness, BOT)
    give(harness, BOT, WORSE)
    optimise()
    worn, _, _, _ = neck()
    if worn != WORSE:
        failures.append(f"the only neck available was not worn; the neck slot holds {worn}")

    give(harness, BOT, BETTER)
    optimise()
    worn, equipped, bag, mailed = neck()
    if worn != BETTER:
        failures.append(f"the better neck was not worn; the neck slot holds {worn}")
    if not holds(equipped, bag, mailed, WORSE):
        failures.append("the replaced neck is gone; it was destroyed rather than put away")

    print(f"upgrade neck={worn} bag={sorted(e for e, _ in bag)} mailed={sorted(mailed)}")

    # Two: the downgrade is declined, and declining does not consume it either. A pass that
    # refused an item by destroying it would satisfy the first case and fail here.
    strip(harness, BOT)
    give(harness, BOT, BETTER)
    optimise()
    give(harness, BOT, WORSE)
    optimise()

    worn, _, bag, _ = neck()
    if worn != BETTER:
        failures.append(f"a worse neck displaced a better one; the neck slot holds {worn}")
    if not any(e == WORSE for e, _ in bag):
        failures.append("the declined neck left the bags; declining should not consume it")

    print(f"downgrade neck={worn} bag={sorted(e for e, _ in bag)}")

    # Three: the same two items in either order give the same answer.
    for first, second in ((BETTER, WORSE), (WORSE, BETTER)):
        strip(harness, BOT)
        give(harness, BOT, first)
        give(harness, BOT, second)
        optimise()

        worn, _, _, _ = neck()
        if worn != BETTER:
            failures.append(f"offered {first} then {second}, the bot wore {worn} rather than "
                            f"the better {BETTER}; the choice still turns on bag order")

        print(f"order first={first} second={second} neck={worn}")

    cleanup(harness, MEMBERS)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
