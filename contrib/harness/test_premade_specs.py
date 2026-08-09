#!/usr/bin/env python3
"""Check that a bot's talent build is chosen deliberately and is legal.

Two separate failures live here. The first is that spec selection used to end in
`SelectRandomContainerElement`, so a paladin spawned holy, protection or retribution by luck and
the same command produced a different character each time. The second is quieter: specs are
applied with LearnSpell rather than LearnTalent, so nothing checks tier requirements, talent
prerequisites or the point budget. An authored build with a bad row produces a character with
deep talents it never paid for, or with points left unspent, and reports nothing at all.

So this asserts three things: a named spec is honoured exactly, an unnamed one is at least the
same every time, and every build actually applied to a bot is legal and spends all 51 points.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM account:
python3 test_premade_specs.py
"""

import argparse
import sys
import time

from vmangos_harness import Harness, CommandError

LEADER = {
    "name": "Speclead",
    "account": "harnessspec",
    "race": 5, "class": 5,                      # Undead priest, so every tested class is Horde
    "staging": (-600.0, -2515.0, 92.0, 1),
}
LEADER_PASSWORD = "harness"

LEVEL = 60
POINTS_AT_60 = 51

CLASS_IDS = {
    "warrior": 1, "paladin": 2, "hunter": 3, "rogue": 4, "priest": 5,
    "shaman": 7, "mage": 8, "warlock": 9, "druid": 11,
}

# The specs authored to fill the level 60 gaps, with the tree spend each should produce. The
# per-tree numbers are the real assertion: a total of 51 only says the points went somewhere,
# while "Fire 32" says the bot is a fire mage rather than an arcane one that happens to add up.
AUTHORED = [
    ("mage", "fire-pve", {"Fire": 31, "Arcane": 18, "Frost": 2}),
    ("warrior", "arms-pve", {"Arms": 31, "Fury": 20}),
    ("warlock", "sm-ruin-pve", {"Affliction": 30, "Destruction": 21}),
    ("rogue", "seal-fate-daggers-pve", {"Assassination": 30, "Combat": 16, "Subtlety": 5}),
    ("priest", "discipline-holy-pve", {"Discipline": 21, "Holy": 30}),
    ("druid", "feral-cat-pve", {"Feral Combat": 32, "Balance": 14, "Restoration": 5}),
]

# Classes to check for a stable build when no spec is named. Every one of these has more than
# one level 60 template, which is exactly the case that used to be settled by a dice roll.
DETERMINISM_CLASSES = ["warrior", "priest", "druid", "mage"]

# Levels to apply an ordered spec at. None of these is a level any template was authored for,
# apart from 60, which is the point: the old data had templates at 19, 29, 39, 49 and 60 and
# nothing sensible to say about anything between them.
SPEND_ORDER_LEVELS = [22, 45, 60]

# Classes with an ordered spec, checked below 60 without naming it, since a roster is not the
# only thing that spawns bots and `.partybot add mage 45` should not produce a twink either.
UNNAMED_BELOW_60 = ["mage", "druid"]

ADD_TIMEOUT = 60.0
SPEC_TIMEOUT = 45.0
POLL = 2.0


def info(harness, name):
    try:
        return harness.info(name)
    except CommandError:
        return None


def member_details(harness, leader):
    return (info(harness, leader) or {}).get("members_detail", [])


def members_of(harness, leader):
    return [m["name"] for m in member_details(harness, leader)]


def clear_roster(harness, leader):
    deadline = time.time() + 60.0
    while time.time() < deadline:
        others = [n for n in members_of(harness, leader) if n != leader]
        if not others:
            return
        for name in others:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)


def provision_leader(harness):
    harness.run(f"account create {LEADER['account']} {LEADER_PASSWORD}", allow_failure=True)
    harness.run(f"account set gmlevel {LEADER['account']} 6", allow_failure=True)

    if info(harness, LEADER["name"]) is None:
        harness.run(f"character erase {LEADER['name']}", allow_failure=True)
        harness.run(f"harness createchar {LEADER['account']} {LEADER['name']} "
                    f"{LEADER['race']} {LEADER['class']} 0", allow_failure=True)

    harness.login(LEADER["name"])
    harness.run(f"harness exec {LEADER['name']} character level {LEADER['name']} {LEVEL}",
                allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d"
                % ((LEADER["name"],) + LEADER["staging"]), allow_failure=True)

    clear_roster(harness, LEADER["name"])
    return LEADER["name"]


def add_bot(harness, leader, bot_class, spec=None, attempts=2, level=LEVEL):
    """Add one bot, optionally naming its spec, and return its name.

    The new member is matched on class and level rather than taken as whichever name appeared,
    because a bot dismissed at the end of one case can still be finishing its join when the next
    one starts. Trusting the first new name then hands back the previous case's bot, and the
    build read off it belongs to another class entirely -- which reads as a spec failure and is
    not one.
    """
    # The level goes on the command whenever it is not the default, spec or no spec, because
    # asking for a bot below 60 without naming a build is exactly the case worth testing.
    parts = [bot_class]
    if spec or level != LEVEL:
        parts.append(str(level))
    if spec:
        parts.append(spec)

    argument = " ".join(parts)
    wanted_class = str(CLASS_IDS[bot_class])

    for _ in range(attempts):
        before = set(members_of(harness, leader))
        harness.run(f"harness exec {leader} partybot add {argument}", allow_failure=True)

        deadline = time.time() + ADD_TIMEOUT
        while time.time() < deadline:
            for member in member_details(harness, leader):
                name = member.get("name")
                if name in before or name == leader:
                    continue
                if member.get("class") == wanted_class and member.get("level") == str(level):
                    return name
            time.sleep(POLL)

    return None


def build_of(harness, bot):
    """Wait for the spec to be applied, then read the build back."""
    deadline = time.time() + SPEC_TIMEOUT
    data = {}
    while time.time() < deadline:
        data = harness.talents(bot)
        if data.get("summary", {}).get("spent", 0) > 0:
            return data
        time.sleep(POLL)
    return data


def points_at(level):
    """Vanilla grants the first talent point at level 10 and one per level after."""
    return max(0, level - 9)


def check_legal(label, data, expected_level=LEVEL):
    """Every applied build must spend its whole budget on talents it is entitled to.

    The level is asserted too, not just the points. Applying a spec used to drag a character up
    to the template's level, so a level 45 bot asked for a level 60 build came back level 60,
    and a check that only looked at points would have called that a pass.
    """
    problems = []
    summary = data.get("summary", {})
    if not summary:
        return [f"{label}: no talent report came back"]

    spent = summary.get("spent", 0)
    available = summary.get("available", 0)
    free = summary.get("free", 0)
    illegal = summary.get("illegal", 0)
    level = summary.get("level")

    if level != expected_level:
        problems.append(f"{label}: is level {level}, expected {expected_level}")
    if available != points_at(expected_level):
        problems.append(f"{label}: level {level} offers {available} points, "
                        f"expected {points_at(expected_level)}")
    if spent != available:
        problems.append(f"{label}: spends {spent} of {available} points")
    if free:
        problems.append(f"{label}: {free} talent points left unspent")
    if illegal:
        detail = ", ".join("talent %s (%s)" % (row.get("talent"), row.get("reason"))
                           for row in data.get("illegal", []))
        problems.append(f"{label}: {illegal} illegal talents: {detail}")

    return problems


def check_authored(harness, leader):
    """A named spec must be applied exactly, tree for tree."""
    problems = []

    for bot_class, spec, expected in AUTHORED:
        clear_roster(harness, leader)
        bot = add_bot(harness, leader, bot_class, spec)
        if not bot:
            problems.append(f"{spec}: no bot joined the group")
            continue

        try:
            data = build_of(harness, bot)
            problems.extend(check_legal(spec, data))

            trees = data.get("trees", {})
            for tree, points in expected.items():
                if trees.get(tree, 0) != points:
                    problems.append(f"{spec}: {tree} has {trees.get(tree, 0)} points, "
                                    f"expected {points}")

            # A tree not named in the build must be empty, or the spec is not what it claims.
            for tree, points in trees.items():
                if points and tree not in expected:
                    problems.append(f"{spec}: {points} unexpected points in {tree}")

            print(f"  {spec:<26} {dict(sorted(trees.items()))}")
        finally:
            harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
            time.sleep(POLL)

    return problems


def check_levels(harness, leader):
    """A spec must fit whatever level it is applied to, not only the one it was authored for.

    This is the whole point of a spend order. Before it, a level 45 bot fell back to the level
    39 twink template and spent 30 of its 36 points, or was dragged up to 60 to fit the build.
    Either way nothing reported it, which is why the assertion is on the level and the point
    total together.
    """
    problems = []

    for bot_class, spec, expected in AUTHORED:
        for level in SPEND_ORDER_LEVELS:
            clear_roster(harness, leader)
            bot = add_bot(harness, leader, bot_class, spec, level=level)
            if not bot:
                problems.append(f"{spec} at {level}: no bot joined the group")
                continue

            try:
                data = build_of(harness, bot)
                problems.extend(check_legal(f"{spec} at {level}", data, expected_level=level))

                trees = {k: v for k, v in data.get("trees", {}).items() if v}
                # The order has to converge on the authored build, or a level 60 member of the
                # roster is no longer the spec it was asked for.
                if level == LEVEL and trees != expected:
                    problems.append(f"{spec} at {level}: build is {trees}, expected {expected}")

                print(f"  {spec + ' @ ' + str(level):<30} "
                      f"{data.get('summary', {}).get('spent', '?'):>2}/{points_at(level)} "
                      f"{dict(sorted(trees.items()))}")
            finally:
                harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
                time.sleep(POLL)

    # An unnamed bot below 60 has to reach an ordered spec too. Selection used to consider only
    # templates at or below the bot's level, so a level 45 mage took the level 39 twink build and
    # spent 30 of its 36 points however good the level 60 data was.
    for bot_class in UNNAMED_BELOW_60:
        clear_roster(harness, leader)
        bot = add_bot(harness, leader, bot_class, level=45)
        if not bot:
            problems.append(f"unnamed {bot_class} at 45: no bot joined the group")
            continue

        try:
            data = build_of(harness, bot)
            problems.extend(check_legal(f"unnamed {bot_class} at 45", data, expected_level=45))
            trees = {k: v for k, v in data.get("trees", {}).items() if v}
            print(f"  {'unnamed ' + bot_class + ' @ 45':<30} "
                  f"{data.get('summary', {}).get('spent', '?'):>2}/{points_at(45)} "
                  f"{dict(sorted(trees.items()))}")
        finally:
            harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
            time.sleep(POLL)

    return problems


def check_determinism(harness, leader, spawns=3):
    """The same request must produce the same build, with no spec named to force it."""
    problems = []

    for bot_class in DETERMINISM_CLASSES:
        seen = []
        for _ in range(spawns):
            clear_roster(harness, leader)
            bot = add_bot(harness, leader, bot_class)
            if not bot:
                problems.append(f"{bot_class}: no bot joined the group")
                continue
            try:
                data = build_of(harness, bot)
                problems.extend(check_legal(f"{bot_class} (unnamed spec)", data))
                seen.append(tuple(sorted(data.get("trees", {}).items())))
            finally:
                harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
                time.sleep(POLL)

        if len(set(seen)) > 1:
            problems.append(f"{bot_class}: spec varies between spawns: "
                            + " vs ".join(str(dict(s)) for s in sorted(set(seen))))
        elif seen:
            print(f"  {bot_class:<26} stable across {len(seen)} spawns: {dict(seen[0])}")

    return problems


def check_unknown_spec_falls_back(harness, leader):
    """A typo must not produce a talentless bot, and must not be silently accepted either."""
    clear_roster(harness, leader)
    bot = add_bot(harness, leader, "mage", "not-a-real-spec")
    if not bot:
        return ["unknown spec: no bot joined the group"]

    try:
        data = build_of(harness, bot)
        problems = check_legal("unknown spec", data)
        print(f"  {'unknown spec falls back':<26} {dict(sorted(data.get('trees', {}).items()))}")
        return problems
    finally:
        harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
        time.sleep(POLL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", help="SOAP url, defaults to the harness default")
    parser.add_argument("--only", choices=["authored", "levels", "determinism", "fallback"],
                        help="run a single section")
    args = parser.parse_args()

    harness = Harness.from_env(url=args.url)
    leader = provision_leader(harness)

    problems = []
    try:
        if args.only in (None, "authored"):
            print("authored specs:")
            problems.extend(check_authored(harness, leader))
        if args.only in (None, "levels"):
            print("ordered specs fit the level they are applied to:")
            problems.extend(check_levels(harness, leader))
        if args.only in (None, "determinism"):
            print("unnamed spec is stable:")
            problems.extend(check_determinism(harness, leader))
        if args.only in (None, "fallback"):
            print("unknown spec:")
            problems.extend(check_unknown_spec_falls_back(harness, leader))
    finally:
        clear_roster(harness, leader)

    if problems:
        print("\nFAILED")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print("\nPASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
