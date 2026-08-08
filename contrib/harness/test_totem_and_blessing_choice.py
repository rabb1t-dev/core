#!/usr/bin/env python3
"""Check that totems and blessings are chosen, not drawn at random.

Six spell slots used to be filled by `SelectRandomContainerElement` at the moment the bot
learned its spells and then never revisited: the four totem schools, the paladin's aura and
its one blessing. That is the wrong shape twice over. It is random, so Windfury Totem was one
ticket in a draw that also held Windwall; and it is decided once, when the thing being decided
depends on who is standing nearby and, for a blessing, on who is receiving it.

Randomness is why this survived so long. A shaman that drops a different air totem every spawn
looks alive rather than broken, and nobody reads the totem bar. So the first thing asserted
here is boring on purpose: spawn the same bot repeatedly and the same choice must come out
every time. The rest asserts that the choice responds to the group, and that the blessing a
member is holding matches what that member should be holding rather than what the paladin
happens to like.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM account:
python3 test_totem_and_blessing_choice.py
"""

import argparse
import sys
import time

from vmangos_harness import Harness, CommandError

# The shaman leader is a priest so the group can be made genuinely caster only. With a warrior
# leading, every group contains a melee weapon user and the composition half of this test
# cannot say anything.
LEADERS = {
    "totem": {
        "name": "Totemlead",
        "account": "harnesstotem",
        "race": 5, "class": 5,                  # Undead priest
        "staging": (-600.0, -2515.0, 92.0, 1),
    },
    "blessing": {
        "name": "Blesslead",
        "account": "harnessbless",
        "race": 1, "class": 1,                  # Human warrior
        "staging": (-8900.0, -130.0, 81.0, 0),
    },
}
LEADER_PASSWORD = "harness"

# Totems that must never be the resting choice for a slot. Each is worth dropping on a
# particular fight and worth nothing as a default, which is exactly what a random draw could
# not express: Disease Cleansing Totem became reachable when its matcher was fixed and went
# straight into the water pool with the same odds as Mana Spring.
NEVER_DEFAULT = {
    "water": ["Disease Cleansing Totem", "Poison Cleansing Totem", "Fire Resistance Totem"],
    "air": ["Windwall Totem", "Nature Resistance Totem", "Sentry Totem"],
    "earth": ["Earthbind Totem", "Stoneclaw Totem"],
    "fire": ["Frost Resistance Totem"],
}

# Same idea for the paladin. A resistance aura is a fight-specific call, and running one by
# default costs the group Devotion for the entire night.
NEVER_DEFAULT_AURA = ["Fire Resistance Aura", "Frost Resistance Aura", "Shadow Resistance Aura"]

# Slots where the *input* varies per spawn even though the choice does not. A premade talent
# spec is rolled for each bot and decides whether the talented option was ever learned, so
# Sanctuary and Sanctity are present for some builds and absent for others. Requiring the same
# answer every time would be testing the spec roller. These assert membership instead: every
# listed value is one the role should accept, and the point is that nothing else appears. A
# Greater Blessing appearing here is the specific regression being guarded, since those cost a
# reagent, buff a whole class at once, and reached this slot only because their name contains
# the single-target name.
TALENT_DEPENDENT = {
    ("paladin", "tank", "pBlessingBuff"):
        {"Blessing of Sanctuary", "Blessing of Kings", "Blessing of Might"},
    ("paladin", "meleedps", "pAura"):
        {"Sanctity Aura", "Retribution Aura"},
}

# The slots this test is about. Anything outside it is test_spell_population.py's business.
CHOICE_SLOTS = {
    "shaman": ["pAirTotem", "pEarthTotem", "pFireTotem", "pWaterTotem", "pWeaponBuff"],
    "paladin": ["pAura", "pBlessingBuff"],
}

ADD_TIMEOUT = 60.0
BUFF_TIMEOUT = 90.0
POLL = 2.0


def info(harness, name):
    try:
        return harness.info(name)
    except CommandError:
        return None


def members_of(harness, leader):
    return [m["name"] for m in (info(harness, leader) or {}).get("members_detail", [])]


def clear_roster(harness, leader):
    deadline = time.time() + 60.0
    while time.time() < deadline:
        others = [n for n in members_of(harness, leader) if n != leader]
        if not others:
            return
        for name in others:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)


def provision_leader(harness, key):
    spec = LEADERS[key]
    harness.run(f"account create {spec['account']} {LEADER_PASSWORD}", allow_failure=True)
    harness.run(f"account set gmlevel {spec['account']} 6", allow_failure=True)

    if info(harness, spec["name"]) is None:
        harness.run(f"character erase {spec['name']}", allow_failure=True)
        harness.run(f"harness createchar {spec['account']} {spec['name']} "
                    f"{spec['race']} {spec['class']} 0", allow_failure=True)

    harness.login(spec["name"])
    harness.run(f"harness exec {spec['name']} character level {spec['name']} 60",
                allow_failure=True)
    harness.run("harness exec %s go xyz %f %f %f %d"
                % ((spec["name"],) + spec["staging"]), allow_failure=True)

    clear_roster(harness, spec["name"])
    return spec["name"]


def add_bot(harness, leader, bot_class, role=None, attempts=2):
    """Add one bot, optionally pinning its role, and return its name."""
    bot = None
    # A spawn occasionally does not land, and a bot that never joins reads here as a bot that
    # made a strange choice, which is a confusing way to report a flaky spawn.
    for _ in range(attempts):
        before = set(members_of(harness, leader))
        harness.run(f"harness exec {leader} partybot add {bot_class}", allow_failure=True)

        deadline = time.time() + ADD_TIMEOUT
        while time.time() < deadline:
            new = set(members_of(harness, leader)) - before - {leader}
            if new:
                bot = sorted(new)[0]
                break
            time.sleep(POLL)

        if bot:
            break

    if bot and role:
        # setrole re-runs PopulateSpellData, which is what makes the role and the group
        # composition at this moment the inputs to the choice rather than whatever they were
        # when the bot spawned.
        harness.run(f"harness exec {bot} partybot setrole {role}", allow_failure=True)

    return bot


def repopulate(harness, bot, role):
    harness.run(f"harness exec {bot} partybot setrole {role}", allow_failure=True)


def choices_of(harness, bot, bot_class):
    data = harness.spells(bot)
    return {slot: data["slots"].get(slot, {}).get("name", "") for slot in CHOICE_SLOTS[bot_class]}


def check_determinism(harness, leader, bot_class, role, spawns=3, exempt=()):
    """The same bot in the same group must make the same choice every time."""
    problems = []
    seen = []

    # "The same group" has to be true for the comparison to mean anything, and a removal takes
    # a moment to land.
    clear_roster(harness, leader)

    for _ in range(spawns):
        bot = add_bot(harness, leader, bot_class, role)
        if not bot:
            problems.append(f"{bot_class} {role}: no bot joined the group")
            continue
        try:
            seen.append(choices_of(harness, bot, bot_class))
        finally:
            harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
            time.sleep(POLL)

    if not seen:
        return problems, {}

    for slot in CHOICE_SLOTS[bot_class]:
        picked = {c.get(slot, "") for c in seen}

        allowed = TALENT_DEPENDENT.get((bot_class, role, slot))
        if allowed:
            bad = sorted(p for p in picked if p not in allowed)
            if bad:
                problems.append(f"{bot_class} {role}: {slot} held {', '.join(bad)}, which is "
                                f"not something this role should be given")
            continue

        if len(picked) > 1:
            problems.append(f"{bot_class} {role}: {slot} varies between spawns: "
                            f"{', '.join(sorted(p or '<empty>' for p in picked))}")

    return problems, seen[0]


def check_no_junk_defaults(bot_class, choices):
    problems = []
    if bot_class == "shaman":
        for school, banned in NEVER_DEFAULT.items():
            slot = f"p{school.capitalize()}Totem"
            name = choices.get(slot, "")
            if name in banned:
                problems.append(f"shaman: {slot} rests on {name}, which is a call for a "
                                f"specific fight rather than a default")
    else:
        if choices.get("pAura", "") in NEVER_DEFAULT_AURA:
            problems.append(f"paladin: aura rests on {choices['pAura']}, a resistance aura")

    return problems


def check_totem_composition(harness, leader):
    """The air and earth totems must answer to who is actually standing in range."""
    problems = []

    # A bot left over from an earlier check is a melee weapon user standing in range, which is
    # the exact input this measures.
    clear_roster(harness, leader)

    shaman = add_bot(harness, leader, "shaman", "healer")
    if not shaman:
        return ["shaman did not join for the composition check"], None

    caster_only = choices_of(harness, shaman, "shaman")

    rogue = add_bot(harness, leader, "rogue", "meleedps")
    if not rogue:
        harness.run(f"harness exec {shaman} partybot remove", allow_failure=True)
        return ["rogue did not join for the composition check"], None

    # The shaman already made its choice, so ask it again now that the group has changed.
    repopulate(harness, shaman, "healer")
    time.sleep(POLL)
    with_melee = choices_of(harness, shaman, "shaman")

    if caster_only.get("pAirTotem") != "Grace of Air Totem":
        problems.append(f"a caster-only group should get Grace of Air Totem in the air slot, "
                        f"got {caster_only.get('pAirTotem') or '<empty>'}")
    if with_melee.get("pAirTotem") != "Windfury Totem":
        problems.append(f"a group with a melee weapon user should get Windfury Totem, "
                        f"got {with_melee.get('pAirTotem') or '<empty>'}")
    if caster_only.get("pEarthTotem") != "Stoneskin Totem":
        problems.append(f"a caster-only group should get Stoneskin Totem in the earth slot, "
                        f"got {caster_only.get('pEarthTotem') or '<empty>'}")
    if with_melee.get("pEarthTotem") != "Strength of Earth Totem":
        problems.append(f"a group with a melee weapon user should get Strength of Earth Totem, "
                        f"got {with_melee.get('pEarthTotem') or '<empty>'}")

    for name in (rogue, shaman):
        harness.run(f"harness exec {name} partybot remove", allow_failure=True)

    return problems, {"caster_only": caster_only, "with_melee": with_melee}


def blessing_on(harness, character):
    """Which blessing this character is currently holding, if any."""
    text = harness.execute(character, "list auras")
    for blessing in ("Blessing of Sanctuary", "Blessing of Kings", "Blessing of Wisdom",
                     "Blessing of Might", "Blessing of Light"):
        if blessing in text:
            return blessing
    return None


def check_blessings(harness, leader):
    """Every member should hold the blessing that suits it, not one the paladin picked."""
    problems = []

    paladin = add_bot(harness, leader, "paladin", "healer")
    rogue = add_bot(harness, leader, "rogue", "meleedps")
    mage = add_bot(harness, leader, "mage", "rangedps")
    spawned = [n for n in (paladin, rogue, mage) if n]
    if len(spawned) != 3:
        for name in spawned:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        return ["the blessing group did not fill"], None

    # A paladin blesses one target per tick and only out of combat, so the group converges
    # over several seconds rather than at once.
    expected = {leader: "Blessing of Might",     # warrior
                rogue: "Blessing of Might",
                mage: "Blessing of Wisdom",
                paladin: "Blessing of Wisdom"}   # healer

    held = {}
    deadline = time.time() + BUFF_TIMEOUT
    while time.time() < deadline:
        held = {name: blessing_on(harness, name) for name in expected}
        if all(held.values()):
            break
        time.sleep(POLL)

    for name, want in expected.items():
        got = held.get(name)
        if got is None:
            problems.append(f"{name} never received a blessing")
        elif got != want:
            problems.append(f"{name} is holding {got}, expected {want}")

    for name in spawned:
        harness.run(f"harness exec {name} partybot remove", allow_failure=True)

    return problems, held


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip", action="append", default=[],
                        help="a check name: determinism, composition, blessings")
    args = parser.parse_args()

    harness = Harness.from_env()
    problems = []

    if "determinism" not in args.skip or "composition" not in args.skip:
        leader = provision_leader(harness, "totem")
        print(f"totems under {leader}", flush=True)

        if "determinism" not in args.skip:
            for role in ("healer", "meleedps"):
                found, choices = check_determinism(harness, leader, "shaman", role)
                found += check_no_junk_defaults("shaman", choices)
                problems += found
                print(f"  {'FAIL' if found else 'pass'}  shaman {role:<9} "
                      f"{', '.join(f'{k}={v or chr(45)}' for k, v in sorted(choices.items()))}",
                      flush=True)
                for line in found:
                    print(f"          {line}", flush=True)

        if "composition" not in args.skip:
            found, detail = check_totem_composition(harness, leader)
            problems += found
            print(f"  {'FAIL' if found else 'pass'}  composition", flush=True)
            if detail:
                for label, choices in detail.items():
                    print(f"          {label}: air={choices.get('pAirTotem') or '-'}, "
                          f"earth={choices.get('pEarthTotem') or '-'}", flush=True)
            for line in found:
                print(f"          {line}", flush=True)

        clear_roster(harness, leader)
        print("", flush=True)

    leader = provision_leader(harness, "blessing")
    print(f"blessings under {leader}", flush=True)

    if "determinism" not in args.skip:
        for role in ("tank", "healer", "meleedps"):
            found, choices = check_determinism(harness, leader, "paladin", role)
            found += check_no_junk_defaults("paladin", choices)
            problems += found
            print(f"  {'FAIL' if found else 'pass'}  paladin {role:<9} "
                  f"aura={choices.get('pAura') or '-'}, "
                  f"own blessing={choices.get('pBlessingBuff') or '-'}", flush=True)
            for line in found:
                print(f"          {line}", flush=True)

    if "blessings" not in args.skip:
        found, held = check_blessings(harness, leader)
        problems += found
        print(f"  {'FAIL' if found else 'pass'}  per-target blessings", flush=True)
        if held:
            for name, blessing in sorted(held.items()):
                print(f"          {name}: {blessing or 'nothing'}", flush=True)
        for line in found:
            print(f"          {line}", flush=True)

    clear_roster(harness, leader)
    print("", flush=True)

    if problems:
        print(f"{len(problems)} problems")
        return 1

    print("totems and blessings are chosen deterministically and fit who is receiving them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
