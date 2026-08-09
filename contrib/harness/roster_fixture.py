#!/usr/bin/env python3
"""A roster of one leader and one bot, and the inventory helpers that go with it.

Both gear suites need the same thing: a character standing in the world so there is
something to summon a roster to, and a summoned member running a party bot AI, which is
what carries the equip pass. The leader comes in through the plain login path and has no
AI at all, so it cannot stand in for the bot even though it is the same kind of character.
"""

import re
import time

from vmangos_harness import CommandError


def inventory(harness, name):
    """Where everything on a character is: worn, carried, or waiting in the mail.

    The mail is not an afterthought. It is the difference between an item moved out of the
    way and one destroyed, which is what both suites are ultimately asking about: gear the
    bags will not take is mailed, and a test that looked only at bags and equipment could
    not tell that from destruction.
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
    """Take everything off a character and out of its bags.

    Each case needs the bot to be offered exactly a known set, because starting kit alone is
    enough to make the result turn on gear the case never mentioned.
    """
    equipped, bag, _ = inventory(harness, name)

    counts = {}
    for entry in equipped.values():
        counts[entry] = counts.get(entry, 0) + 1
    for entry, count in bag:
        counts[entry] = counts.get(entry, 0) + count

    for entry, count in counts.items():
        harness.run(f"harness exec {name} additem {entry} -{count}", allow_failure=True)


def cleanup(harness, members):
    harness.run("raidguild resetbinds", allow_failure=True)

    for name, _, _ in members:
        harness.run(f"raidguild remove {name}", allow_failure=True)
        harness.logout(name)

    for name, _, _ in members:
        # Erasing resolves the name through the player cache, so erasing one still on its way
        # out of the world can miss and leave the row behind for the next run to trip over.
        deadline = time.time() + 30.0
        while time.time() < deadline and harness.info(name) is not None:
            time.sleep(1.0)

        harness.run(f"character erase {name}", allow_failure=True)


def summon_roster(harness, members, leader, bot, spec="tank", level=1):
    """Build the roster, bring the leader in, and summon the bot to it.

    Returns None once the bot is in the world, or a message describing what went wrong.
    """
    cleanup(harness, members)
    harness.run("raidguild reload")

    for name, race, class_id in members:
        harness.run(f"raidguild add {name} {race} {class_id} 0 {spec} {level}")

    harness.run("raidguild provision")
    harness.login(leader, timeout=60)
    harness.run(f"raidguild summon {leader}")

    deadline = time.time() + 90.0
    while time.time() < deadline and harness.info(bot) is None:
        time.sleep(2.0)

    if harness.info(bot) is None:
        return f"{bot} never reached the world"

    try:
        harness.run(f"harness equipnew {bot}")
    except CommandError as exc:
        return f"cannot drive the equip pass: {exc}"

    return None
