#!/usr/bin/env python3
"""Check that the bot spell struct is actually filled in, class by class.

`CombatBotBaseAI::PopulateSpellData` assigns spells into named struct slots by matching
their names, and a slot whose name never matches stays null for the life of the bot. From
outside the process that is indistinguishable from a rotation that simply declines to cast,
which is how the shaman cure slots went unfilled long enough to read as "Horde bots do not
dispel". Every fix in that area is therefore worth a test that can see the slots themselves.

Two things are asserted. The named slots below must hold something, because a level 60 bot
that has learned everything a trainer offers has no excuse for an empty Frostbolt. And no
slot anywhere may point at a spell the bot does not know, which is the other failure this
kind of name matching produces.

The empty-slot counts printed per class are not failures. They are the baseline the rotation
work is measured against, and most of them are talent spells the bot did not take.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_spell_population.py
"""

import argparse
import sys
import time

from vmangos_harness import Harness, CommandError

# One leader per faction, because three classes are faction locked: `.partybot add paladin`
# is refused for a Horde leader and `shaman` for an Alliance one.
LEADERS = {
    "Horde": {
        "name": "Spellhorde",
        "account": "harnessspellh",
        "race": 2, "class": 1,                  # Orc warrior
        "classes": ["warrior", "hunter", "rogue", "priest", "shaman", "mage",
                    "warlock", "druid"],
    },
    "Alliance": {
        "name": "Spellally",
        "account": "harnessspella",
        "race": 1, "class": 1,                  # Human warrior
        "classes": ["paladin"],
    },
}
LEADER_PASSWORD = "harness"

# Open ground, so the bots are not added inside anything with a player cap.
STAGING = {"Horde": (-600.0, -2515.0, 92.0, 1), "Alliance": (-8900.0, -130.0, 81.0, 0)}

# Slots that a level 60 bot with a full trainer spell book must have filled. Kept to spells
# every member of the class learns from a trainer: nothing talented, nothing from a quest,
# nothing a rank check could argue about. The dispel and armor slots are the ones this test
# was written for; the rest are here to catch a population regression that breaks the file
# wholesale rather than one slot.
REQUIRED = {
    "warrior": ["pBattleStance", "pDefensiveStance", "pBerserkerStance", "pSunderArmor",
                "pHeroicStrike", "pBattleShout", "pCharge", "pExecute"],
    "paladin": ["pSeal", "pAura", "pJudgement", "pCleanse", "pHammerOfJustice",
                "pDivineShield", "pLayOnHands", "pConsecration"],
    "hunter": ["pAspectOfTheHawk", "pAspectOfTheCheetah", "pSerpentSting", "pArcaneShot",
               "pMultiShot", "pHuntersMark", "pWingClip", "pRaptorStrike"],
    "rogue": ["pSinisterStrike", "pEviscerate", "pStealth", "pSliceAndDice", "pGouge",
              "pKick", "pSprint", "pEvasion"],
    "priest": ["pPowerWordFortitude", "pPowerWordShield", "pMindBlast", "pShadowWordPain",
               "pDispelMagic", "pAbolishDisease", "pPsychicScream", "pSmite"],
    # pCureDisease and pCurePoison had no matcher at all until this was fixed, and
    # pWaterTotem was unreachable through a matcher naming a spell that does not exist.
    "shaman": ["pLightningBolt", "pChainLightning", "pEarthShock", "pPurge", "pGhostWolf",
               "pCureDisease", "pCurePoison", "pAirTotem", "pEarthTotem", "pFireTotem",
               "pWaterTotem", "pLightningShield"],
    "mage": ["pFrostbolt", "pFireball", "pArcaneIntellect", "pPolymorph", "pFrostNova",
             "pBlink", "pCounterspell", "pRemoveLesserCurse", "pEvocation"],
    "warlock": ["pDemonArmor", "pShadowBolt", "pCorruption", "pImmolate", "pFear",
                "pBanish", "pCurseofAgony", "pLifeTap", "pDrainLife"],
    "druid": ["pBearForm", "pCatForm", "pWrath", "pMoonfire", "pMarkoftheWild",
              "pRemoveCurse", "pAbolishPoison", "pRebirth", "pFaerieFire", "pEntanglingRoots"],
}

# The top rank of each class's resurrection line. The slot took whichever resurrect effect
# the spell map happened to iterate last, and that map is unordered, so this is the
# assertion that the rank now decides it rather than a hash bucket.
RESURRECTION = {
    "priest": 20770,        # Resurrection Rank 5
    "paladin": 20773,       # Redemption Rank 5
    "shaman": 20777,        # Ancestral Spirit Rank 5
    "druid": 20748,         # Rebirth Rank 5
}

# The only slots allowed to hold a spell the bot does not know. A rogue learns the spell
# that crafts a poison, never the enchant that applies it, and the two are separate spells
# that happen to share a name; the enchant is cast directly rather than through the item.
UNKNOWN_ALLOWED = {"pMainHandPoison", "pOffHandPoison"}

# A slot can be filled and still be wrong, by holding a rank far below the bot. Poisons are
# where this bites: their ranks are written into the name rather than into the rank field,
# so the lookup used to settle on rank 1 forever. A level 60 rogue should be applying a
# poison from the last stretch of the level range whichever kind it picked, and the lowest
# top rank among the five kinds is Crippling Poison II at 38.
MIN_SLOT_LEVEL = {"pMainHandPoison": 38, "pOffHandPoison": 38}

ADD_TIMEOUT = 60.0
POLL = 2.0


def info(harness, name):
    try:
        return harness.info(name)
    except CommandError:
        return None


def clear_roster(harness, leader):
    deadline = time.time() + 60.0
    while time.time() < deadline:
        current = info(harness, leader)
        others = [m["name"] for m in (current or {}).get("members_detail", [])
                  if m["name"] != leader]
        if not others:
            return
        for name in others:
            harness.run(f"harness exec {name} partybot remove", allow_failure=True)
        time.sleep(POLL)


def provision_leader(harness, faction):
    spec = LEADERS[faction]
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
                % ((spec["name"],) + STAGING[faction]), allow_failure=True)

    clear_roster(harness, spec["name"])
    return spec["name"]


def add_bot(harness, leader, bot_class):
    """Add one bot of a given class and return its name once it is in the group."""
    before = {m["name"] for m in (info(harness, leader) or {}).get("members_detail", [])}
    harness.run(f"harness exec {leader} partybot add {bot_class}", allow_failure=True)

    deadline = time.time() + ADD_TIMEOUT
    while time.time() < deadline:
        current = info(harness, leader)
        names = {m["name"] for m in (current or {}).get("members_detail", [])}
        new = names - before - {leader}
        if new:
            return sorted(new)[0]
        time.sleep(POLL)

    return None


def check_class(harness, leader, bot_class):
    """Add a bot of this class, read its slots, and report what is wrong with them."""
    bot = add_bot(harness, leader, bot_class)
    if not bot:
        return [f"no {bot_class} bot joined the group"], None

    try:
        data = harness.spells(bot)
    except CommandError as exc:
        return [f"{bot_class}: {exc}"], None
    finally:
        harness.run(f"harness exec {bot} partybot remove", allow_failure=True)

    slots = data["slots"]
    summary = data["summary"]
    problems = []

    for name in REQUIRED.get(bot_class, []):
        if name not in slots:
            problems.append(f"{bot_class}: {name} is not a slot of this class")
        elif slots[name]["id"] == 0:
            problems.append(f"{bot_class}: {name} is empty")

    # A slot naming a spell the bot never learned is the other way name matching goes
    # wrong, and it fails silently because CanTryToCastSpell does not check HasSpell.
    for name, slot in sorted(slots.items()):
        if slot["id"] and not slot["known"] and name not in UNKNOWN_ALLOWED:
            problems.append(f"{bot_class}: {name} holds {slot['name']} "
                            f"({slot['id']}), which the bot does not know")

        floor = MIN_SLOT_LEVEL.get(name)
        if floor and slot["id"] and slot["level"] < floor:
            problems.append(f"{bot_class}: {name} holds {slot['name']} at spell level "
                            f"{slot['level']}, below the rank a level 60 bot should have")

    expected_res = RESURRECTION.get(bot_class)
    if expected_res and summary.get("resurrection") != expected_res:
        problems.append(f"{bot_class}: resurrection is {summary.get('resurrection')}, "
                        f"expected the top rank {expected_res}")

    empty = sorted(n for n, s in slots.items() if not s["id"])
    return problems, {"bot": bot, "filled": summary.get("filled", 0),
                      "slots": summary.get("slots", 0), "empty": empty}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", action="append", default=[],
                        help="a class name; repeatable")
    parser.add_argument("--empty", action="store_true",
                        help="also list every empty slot, not just the count")
    args = parser.parse_args()

    harness = Harness.from_env()
    problems = []

    for faction, spec in LEADERS.items():
        classes = [c for c in spec["classes"] if not args.only or c in args.only]
        if not classes:
            continue

        try:
            leader = provision_leader(harness, faction)
        except CommandError as exc:
            problems.append(f"{faction} leader could not be provisioned: {exc}")
            continue

        print(f"{faction} under {leader}", flush=True)
        for bot_class in classes:
            found, detail = check_class(harness, leader, bot_class)
            problems.extend(found)

            if detail:
                print(f"  {'FAIL' if found else 'pass'}  {bot_class:<8} "
                      f"{detail['filled']}/{detail['slots']} slots filled "
                      f"[{detail['bot']}]", flush=True)
                if args.empty and detail["empty"]:
                    print(f"          empty: {', '.join(detail['empty'])}", flush=True)
            for line in found:
                print(f"          {line}", flush=True)

        clear_roster(harness, leader)
        print("", flush=True)

    if problems:
        print(f"{len(problems)} problems")
        return 1

    print("every required slot is populated and every populated slot is known")
    return 0


if __name__ == "__main__":
    sys.exit(main())
