#!/usr/bin/env python3
"""Check that giving a bot better gear does not destroy the gear it was already wearing.

The path under test decides what happens to a bot's current equipment when something new
turns up in its bags, and until recently it freed the slot by destroying whatever was in it.
That was survivable while bots were conjured seconds before use and carried nothing but
generated reagents. A roster that keeps what it earns cannot afford it, and the trigger makes
it worse than it sounds: the pass runs on trade completion and bots accept every trade, so
the natural way to hand a bot an upgrade was also the way to delete the item it replaced.

There is no way to conduct a trade over SOAP, which is why this path had no test at all.
`.harness equipnew` exists to call it directly and `.harness items` to see where everything
ended up, including the mail, since gear the bags will not take is mailed and a test that
looked only at bags and equipment could not tell that from destruction.

This file asks only whether gear survives. Which of two usable items a bot picks is the
evaluator's question and `test_item_evaluator_behaviour.py` asks it. The two are not fully
separable, though, since the pass now chooses rather than equipping whatever it sees last:
every case below is built so the item meant to be worn is also the one the evaluator
prefers, and the scores are quoted where that is not obvious.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_bot_gear_preservation.py
"""

import sys

from roster_fixture import cleanup, give, holds, inventory, strip, summon_roster
from vmangos_harness import Harness

# Two orc warriors: one to stand in the world so there is something to summon a roster to,
# and one to be the bot. Warrior because the last case is about the weapon and shield rules,
# and a class that can hold all of them keeps the setup honest.
LEADER = "Rggearlead"
BOT = "Rggearone"
MEMBERS = [(LEADER, 2, 1), (BOT, 2, 1)]

# One-handed swords, a shield and a two-handed sword, none of which ask for a level or a
# class, so the only thing between the bot and any of them is the code being tested.
SWORD = 3455            # Deathstalker Shortsword, 5.0 dps, scores 35 for a tank
OTHER_SWORD = 15335     # Briarsteel Shortsword, 7.2 dps, scores 60
SHIELD = 4911           # Thick Bark Buckler, 55 armour, scores 5

# Warblade of Caer Darrow, 57 dps and no level requirement, scoring 610 against the 40 of
# the sword and buckler together. It has to beat both of them at once, since for a tank a
# shield and a fast one-hander are worth more than a merely adequate two-hander, and one the
# evaluator declines proves nothing about the off-hand rule.
TWO_HANDER = 13982

# Holy War Sword, which asks for level 60. The bot is level 1, so equipping it would mean the
# restriction was never checked. The old code asked FindEquipSlot, which only answers where a
# thing of that shape goes and has no opinion on whether the character may wear it.
TOO_HIGH = 15221

# Minor Healing Potion, which used to be drunk the moment it arrived.
POTION = 118

EQUIPMENT_SLOT_MAINHAND = 15
EQUIPMENT_SLOT_OFFHAND = 16


def main():
    harness = Harness.from_env()
    failures = []

    problem = summon_roster(harness, MEMBERS, LEADER, BOT)
    if problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        cleanup(harness, MEMBERS)
        return 1

    # One: the displaced item survives. This is the case that used to delete the sword a
    # human traded in to replace.
    strip(harness, BOT)
    give(harness, BOT, SWORD)
    harness.run(f"harness equipnew {BOT}")

    equipped, bag, mailed = inventory(harness, BOT)
    if equipped.get(EQUIPMENT_SLOT_MAINHAND) != SWORD:
        failures.append(f"the first sword was not equipped; the main hand holds "
                        f"{equipped.get(EQUIPMENT_SLOT_MAINHAND)}")

    give(harness, BOT, OTHER_SWORD)
    harness.run(f"harness equipnew {BOT}")

    equipped, bag, mailed = inventory(harness, BOT)
    if equipped.get(EQUIPMENT_SLOT_MAINHAND) != OTHER_SWORD:
        failures.append(f"the second sword was not equipped; the main hand holds "
                        f"{equipped.get(EQUIPMENT_SLOT_MAINHAND)}")
    if not holds(equipped, bag, mailed, SWORD):
        failures.append("the displaced sword is gone; it was destroyed rather than put away")

    print(f"swap mainhand={equipped.get(EQUIPMENT_SLOT_MAINHAND)} "
          f"bag={sorted(e for e, _ in bag)} mailed={sorted(mailed)}")

    # Two: a restriction the bot does not meet is respected. FindEquipSlot would have handed
    # back a main hand for this and the sword would have gone straight on.
    strip(harness, BOT)
    give(harness, BOT, TOO_HIGH)
    harness.run(f"harness equipnew {BOT}")

    equipped, bag, mailed = inventory(harness, BOT)
    level = (harness.info(BOT) or {}).get("level")
    if TOO_HIGH in equipped.values():
        failures.append(f"a sword requiring level 60 was equipped by a level {level} bot")
    if not any(e == TOO_HIGH for e, _ in bag):
        failures.append("the level 60 sword left the bags without being equipped")

    print(f"restricted level={level} equipped={TOO_HIGH in equipped.values()}")

    # Three: a two-hander clears the off hand rather than being worn alongside a shield.
    strip(harness, BOT)
    give(harness, BOT, SWORD)
    give(harness, BOT, SHIELD)
    harness.run(f"harness equipnew {BOT}")

    equipped, _, _ = inventory(harness, BOT)
    if equipped.get(EQUIPMENT_SLOT_OFFHAND) != SHIELD:
        failures.append(f"the shield would not go on, so the two hander case proves nothing; "
                        f"the off hand holds {equipped.get(EQUIPMENT_SLOT_OFFHAND)}")

    give(harness, BOT, TWO_HANDER)
    harness.run(f"harness equipnew {BOT}")

    equipped, bag, mailed = inventory(harness, BOT)
    if equipped.get(EQUIPMENT_SLOT_MAINHAND) != TWO_HANDER:
        failures.append(f"the two hander was not equipped; the main hand holds "
                        f"{equipped.get(EQUIPMENT_SLOT_MAINHAND)}")
    if EQUIPMENT_SLOT_OFFHAND in equipped:
        failures.append(f"a shield is still worn alongside a two hander; the off hand holds "
                        f"{equipped[EQUIPMENT_SLOT_OFFHAND]}")
    if not holds(equipped, bag, mailed, SHIELD):
        failures.append("the shield is gone; it was destroyed rather than put away")

    print(f"twohander mainhand={equipped.get(EQUIPMENT_SLOT_MAINHAND)} "
          f"offhand={equipped.get(EQUIPMENT_SLOT_OFFHAND)}")

    # Four: nothing is used merely because it arrived. Consumables were drunk on receipt,
    # which for a raid handed forty flasks meant drinking them in the trade window.
    strip(harness, BOT)
    give(harness, BOT, POTION)
    harness.run(f"harness equipnew {BOT}")

    _, bag, _ = inventory(harness, BOT)
    kept = any(e == POTION for e, _ in bag)
    if not kept:
        failures.append("the potion was drunk on receipt rather than kept")

    print(f"consumable kept={kept}")

    cleanup(harness, MEMBERS)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
