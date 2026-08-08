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

Worth being clear about what is *not* claimed. This pass has no notion of better: it equips
whatever it can, in bag order, so the last thing it looks at is what ends up worn. Choosing
between two usable items is the evaluator's job and the evaluator does not exist yet. Each
case here therefore strips the bot first, so that what it is being offered is unambiguous.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_bot_gear_preservation.py
"""

import re
import sys
import time

from vmangos_harness import CommandError, Harness

# Two orc warriors: one to stand in the world so there is something to summon a roster to,
# and one to be the bot. Warrior because the last case is about the weapon and shield rules,
# and a class that can hold all of them keeps the setup honest.
LEADER = "Rggearlead"
BOT = "Rggearone"
MEMBERS = [(LEADER, 2, 1), (BOT, 2, 1)]

# One-handed swords, a shield and a two-handed sword, none of which ask for a level or a
# class, so the only thing between the bot and any of them is the code being tested.
SWORD = 3455            # Deathstalker Shortsword
OTHER_SWORD = 15335     # Briarsteel Shortsword
SHIELD = 4911           # Thick Bark Buckler
TWO_HANDER = 5779       # Forsaken Bastard Sword

# Holy War Sword, which asks for level 60. The bot is level 1, so equipping it would mean the
# restriction was never checked. The old code asked FindEquipSlot, which only answers where a
# thing of that shape goes and has no opinion on whether the character may wear it.
TOO_HIGH = 15221

# Minor Healing Potion, which used to be drunk the moment it arrived.
POTION = 118

EQUIPMENT_SLOT_MAINHAND = 15
EQUIPMENT_SLOT_OFFHAND = 16


def inventory(harness, name):
    """Where everything on a character is: worn, carried, or waiting in the mail.

    The mail is not an afterthought. It is the difference between an item moved out of the
    way and one destroyed, which is the whole question this file asks.
    """
    text = harness.run(f"harness items {name}")

    equipped, bag, mailed = {}, [], []
    for line in text.splitlines():
        line = line.strip()
        fields = dict(re.findall(r"(\w+)=(\S+)", line))
        if line.startswith("equipped "):
            equipped[int(fields["slot"])] = int(fields["entry"])
        elif line.startswith("bag "):
            bag.append((int(fields["entry"]), int(fields["count"])))
        elif line.startswith("mailed "):
            mailed.append(int(fields["entry"]))

    return equipped, bag, mailed


def holds(equipped, bag, mailed, entry):
    """Whether the character still has the item at all, wherever it ended up."""
    return (entry in equipped.values()
            or any(e == entry for e, _ in bag)
            or entry in mailed)


def give(harness, name, entry):
    harness.run(f"harness exec {name} additem {entry}")


def strip(harness, name):
    """Take everything off the bot and out of its bags.

    Each case needs the bot to be offered exactly one thing, because the pass equips
    everything it can rather than choosing, and starting kit alone is enough to make the
    result depend on which bag slot a sword happened to land in.
    """
    equipped, bag, _ = inventory(harness, name)

    counts = {}
    for entry in equipped.values():
        counts[entry] = counts.get(entry, 0) + 1
    for entry, count in bag:
        counts[entry] = counts.get(entry, 0) + count

    for entry, count in counts.items():
        harness.run(f"harness exec {name} additem {entry} -{count}", allow_failure=True)


def cleanup(harness):
    harness.run("raidguild resetbinds", allow_failure=True)

    for name, _, _ in MEMBERS:
        harness.run(f"raidguild remove {name}", allow_failure=True)
        harness.logout(name)

    for name, _, _ in MEMBERS:
        # Erasing resolves the name through the player cache, so erasing one still on its way
        # out of the world can miss and leave the row behind for the next run to trip over.
        deadline = time.time() + 30.0
        while time.time() < deadline and harness.info(name) is not None:
            time.sleep(1.0)

        harness.run(f"character erase {name}", allow_failure=True)


def main():
    harness = Harness.from_env()
    failures = []

    cleanup(harness)
    harness.run("raidguild reload")

    for name, race, class_id in MEMBERS:
        harness.run(f"raidguild add {name} {race} {class_id} 0 tank 1")

    harness.run("raidguild provision")
    harness.login(LEADER, timeout=60)
    harness.run(f"raidguild summon {LEADER}")

    deadline = time.time() + 90.0
    while time.time() < deadline and harness.info(BOT) is None:
        time.sleep(2.0)

    if harness.info(BOT) is None:
        print(f"FAIL: {BOT} never reached the world", file=sys.stderr)
        cleanup(harness)
        return 1

    # A summoned member runs a party bot AI, which is what carries the pass under test. The
    # leader came in through the plain login path and has no AI at all, so it cannot be used
    # for this even though it is the same kind of character.
    try:
        harness.run(f"harness equipnew {BOT}")
    except CommandError as exc:
        print(f"FAIL: cannot drive the equip pass: {exc}", file=sys.stderr)
        cleanup(harness)
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

    cleanup(harness)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
