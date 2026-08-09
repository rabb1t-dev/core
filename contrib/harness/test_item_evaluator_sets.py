#!/usr/bin/env python3
"""Check that a bot keeps a set together, breaks it when that is right, and reads its bags.

`test_item_evaluator_loadout.py` proves the arithmetic: a set bonus reaches the score and a
capped stat stops paying. This drives the same two ideas through a live bot, where the answer
is observable as what it is wearing rather than as a number.

Thero-shan's Vestments is the set, for three reasons: six leather pieces, no level requirement,
and rogue-only, so a level 1 orc rogue can wear all of it. Its only measurable bonus needs all
six, which makes every case here a clean question about one threshold. For the combat rogue
weights the six pieces score 444.33 alone and 504.33 together, so the bonus is worth 60 and the
belt they compete over is worth 33.54.

That yields the pair of cases that matters. Sagebrush Girdle scores 75.71, better than the set
belt by 42 and so not worth 60, and the set has to survive it: this is the case the old per-slot
scoring got wrong every time, because it compared two belts and could not see what the second
one cost. Belt of Preserved Heads scores 157.58, better by 124, and the set has to break for it,
because a set bonus is worth a number and not a special status.

The last case is about where the bot looks rather than what it decides. Gear inside an equipped
bag was invisible to the pass until it walked containers as well as the backpack, and a bag is
the obvious place for a member handed one to keep what it earns.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_item_evaluator_sets.py
"""

import sys

from roster_fixture import cleanup, give, holds, inventory, strip, summon_roster
from vmangos_harness import Harness

LEADER = "Rgsetslead"
BOT = "Rgsetsone"
# Orc rogue: the set is rogue-only, and a rogue can wear leather from level 1.
MEMBERS = [(LEADER, 2, 4), (BOT, 2, 4)]
SPEC = "combat-swords-pve"

# Thero-shan's Vestments, by the equipment slot each piece fills.
SET_WAIST = 7948
SET_PIECES = {
    5: SET_WAIST,   # Girdle of Thero-shan, 33.54
    6: 7949,        # Leggings of Thero-shan
    4: 7950,        # Armor of Thero-shan
    9: 7951,        # Hands of Thero-shan
    7: 7952,        # Boots of Thero-shan
    0: 7953,        # Mask of Thero-shan
}

# Better than the set belt, but by less than the bonus is worth.
CHEAP_RIVAL = 17778     # Sagebrush Girdle, 75.71
# Better than the set belt by more than the bonus is worth.
EXPENSIVE_RIVAL = 20216  # Belt of Preserved Heads, 157.58

EQUIPMENT_SLOT_WAIST = 5
FIRST_CONTAINER_SLOT = 19
POUCH = 805  # Small Red Pouch, six slots, no level requirement


def main():
    harness = Harness.from_env()
    failures = []

    problem = summon_roster(harness, MEMBERS, LEADER, BOT, role="meleedps", spec=SPEC)
    if problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        cleanup(harness, MEMBERS)
        return 1

    def optimise():
        harness.run(f"harness equipnew {BOT}")

    def worn():
        equipped, bag, mailed = inventory(harness, BOT)
        return equipped, bag, mailed

    def dress_in_set():
        strip(harness, BOT)
        for entry in SET_PIECES.values():
            give(harness, BOT, entry)
        optimise()

    # Nothing below means anything if the spec did not reach the bot: the whole roster would be
    # scored against whatever row for the class was found first, and the numbers this file
    # asserts against are the combat rogue ones.
    info = harness.info(BOT)
    if not info or info.get("spec") != SPEC:
        failures.append(f"the roster's spec did not reach the bot; it reports "
                        f"{info.get('spec') if info else 'nothing'} rather than {SPEC}")
    print(f"spec={info.get('spec') if info else '?'}")

    # One: offered the six pieces and nothing else, the bot ends up wearing the set. A member
    # that cannot assemble a set at all cannot be asked whether it will keep one.
    dress_in_set()
    equipped, _, _ = worn()
    for slot, entry in sorted(SET_PIECES.items()):
        if equipped.get(slot) != entry:
            failures.append(f"set piece {entry} was not worn in slot {slot}; "
                            f"that slot holds {equipped.get(slot)}")
    print(f"assembled {sorted(equipped.items())}")

    # Two: a better belt that is not better than the bonus. The set stays, and the belt is put
    # away rather than consumed, so declining costs nothing.
    give(harness, BOT, CHEAP_RIVAL)
    optimise()
    equipped, bag, mailed = worn()
    if equipped.get(EQUIPMENT_SLOT_WAIST) != SET_WAIST:
        failures.append(f"the set was broken for a belt worth less than the bonus; the waist "
                        f"holds {equipped.get(EQUIPMENT_SLOT_WAIST)} rather than {SET_WAIST}")
    if not holds(equipped, bag, mailed, CHEAP_RIVAL):
        failures.append("the declined belt is gone; declining should not consume it")
    print(f"kept set waist={equipped.get(EQUIPMENT_SLOT_WAIST)} "
          f"bag={sorted(e for e, _ in bag)}")

    # Three: a belt better than the bonus. The set breaks, and the piece it displaces survives.
    # Without this the case above would be satisfied by an engine that never breaks a set.
    give(harness, BOT, EXPENSIVE_RIVAL)
    optimise()
    equipped, bag, mailed = worn()
    if equipped.get(EQUIPMENT_SLOT_WAIST) != EXPENSIVE_RIVAL:
        failures.append(f"a belt worth more than the whole set bonus was declined; the waist "
                        f"holds {equipped.get(EQUIPMENT_SLOT_WAIST)}")
    if not holds(equipped, bag, mailed, SET_WAIST):
        failures.append("the displaced set piece is gone; it was destroyed rather than put away")
    print(f"broke set waist={equipped.get(EQUIPMENT_SLOT_WAIST)} "
          f"bag={sorted(e for e, _ in bag)}")

    # Four: the upgrade is inside a bag rather than the backpack. Same question as the very
    # first behaviour case, asked from the one place the pass used not to look.
    strip(harness, BOT)
    harness.run(f"harness wear {BOT} {POUCH}")
    give(harness, BOT, SET_WAIST)
    optimise()

    equipped, _, _ = worn()
    if equipped.get(EQUIPMENT_SLOT_WAIST) != SET_WAIST:
        failures.append(f"the only belt available was not worn; the waist holds "
                        f"{equipped.get(EQUIPMENT_SLOT_WAIST)}")

    harness.run(f"harness stow {BOT} {EXPENSIVE_RIVAL} {FIRST_CONTAINER_SLOT}")
    optimise()

    equipped, bag, mailed = worn()
    if equipped.get(EQUIPMENT_SLOT_WAIST) != EXPENSIVE_RIVAL:
        failures.append(f"an upgrade sitting in a bag was not considered; the waist still holds "
                        f"{equipped.get(EQUIPMENT_SLOT_WAIST)}")
    if not holds(equipped, bag, mailed, SET_WAIST):
        failures.append("the belt replaced from a bag is gone rather than put away")
    print(f"from bag waist={equipped.get(EQUIPMENT_SLOT_WAIST)} "
          f"bag={sorted(e for e, _ in bag)}")

    cleanup(harness, MEMBERS)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
