#!/usr/bin/env python3
"""Check the two things about an item's value that no single item can express.

`test_item_evaluator.py` proves the engine reads one item the way the Classic Gear Ranker
does, and `test_item_evaluator_behaviour.py` proves a bot acts on the answer. Both work a
piece at a time, which is exactly the blind spot here.

A stat cap is a property of a total. Whether the tenth point of hit is worth anything depends
on the other nine, so no per-item score can see it, and a linear model that cannot see it
keeps paying 55 a point for hit long after the miss chance has reached zero.

A set bonus is a property of a combination. Three pieces of a set are worth more than three
pieces, so an individually worse piece can be the correct choice, and per-slot scoring cannot
reach that conclusion no matter how well the weights are tuned.

`.harness loadout` scores a hypothetical set of gear rather than a bot's, which is what makes
these assertions exact: naming the same shoulders ten times is a legitimate way to ask what
twenty points of hit is worth, and the answer is arithmetic with no bot, bags or slots in it.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_item_evaluator_loadout.py
"""

import sys

from vmangos_harness import Harness

# Truestrike Shoulders is +2% hit and nothing else that interacts with a cap, so a count of
# them is a dial on exactly one stat. Edgemaster's Handguards is +7 weapon skill, which is
# over the cap on its own: the famous part of the item is the first 5 points.
HIT_SHOULDERS = 12927
WEAPON_SKILL_GLOVES = 14551

# Warrior fury weights hit at 55 a point and weapon skill at 36.84, capped at 9 and 5.
FURY_HIT_WEIGHT = 55.0
FURY_WEAPON_SKILL_WEIGHT = 36.84

# Embrace of the Viper, whose three-piece bonus is the one measured here.
VIPER = (6473, 10410, 10411, 10412, 10413)

# Thero-shan's Vestments is the control, because its only threshold is all six pieces: five of
# them trigger no set spell at all, which is a stronger statement than five that trigger one
# worth nothing. The live test in test_item_evaluator_sets.py turns on the same threshold.
THERO_SHAN = (7948, 7949, 7950, 7951, 7952, 7953)

TOLERANCE = 0.01


def loadout(harness, class_id, spec, entries):
    """The `loadout` line as floats and ints, and the per-set lines beside it."""
    text = harness.run(f"harness loadout {class_id} {spec} " + " ".join(str(e) for e in entries))

    fields, sets = {}, []
    for line in text.splitlines():
        line = line.strip()
        pairs = dict(pair.split("=", 1) for pair in line.split() if "=" in pair)
        if line.startswith("loadout "):
            fields = pairs
        elif line.startswith("set "):
            sets.append(pairs)

    return fields, sets


def main():
    harness = Harness.from_env()
    failures = []

    def check(name, got, want):
        if abs(got - want) > TOLERANCE:
            failures.append(f"{name}: expected {want:.4f}, got {got:.4f}")

    # One: under the cap, capping changes nothing. Without this the next case could pass with
    # an engine that simply scored hit at zero.
    fields, _ = loadout(harness, 1, "fury", [HIT_SHOULDERS])
    hit = int(fields["hit"])
    if hit != 2:
        failures.append(f"expected 2 hit from one pair of shoulders, got {hit}")
    check("one pair of shoulders, capped score", float(fields["score"]), float(fields["linear"]))
    print(f"under cap hit={hit} score={fields['score']} linear={fields['linear']}")

    # Two: ten pairs is 20 hit against a cap of 9, so the eleven points past it are worth
    # nothing and the difference between the linear and capped scores is exactly what they
    # would have been paid.
    fields, _ = loadout(harness, 1, "fury", [HIT_SHOULDERS] * 10)
    hit = int(fields["hit"])
    cap = float(fields["hit_cap"])
    if hit != 20:
        failures.append(f"expected 20 hit from ten pairs of shoulders, got {hit}")
    if cap != 9.0:
        failures.append(f"expected a hit cap of 9 for fury, got {cap}")
    check("hit past the cap",
          float(fields["linear"]) - float(fields["score"]),
          (hit - cap) * FURY_HIT_WEIGHT)
    print(f"over cap hit={hit} cap={cap} score={fields['score']} linear={fields['linear']}")

    # Three: the same for weapon skill, and from a single item rather than a contrived stack.
    # An item can be over a cap on its own, which is why the cap cannot live in the item.
    fields, _ = loadout(harness, 1, "fury", [WEAPON_SKILL_GLOVES])
    skill = int(fields["weapon_skill"])
    cap = float(fields["weapon_skill_cap"])
    if skill != 7:
        failures.append(f"expected 7 weapon skill from the gloves, got {skill}")
    check("weapon skill past the cap",
          float(fields["linear"]) - float(fields["score"]),
          (skill - cap) * FURY_WEAPON_SKILL_WEIGHT)
    print(f"weapon skill={skill} cap={cap} score={fields['score']} linear={fields['linear']}")

    # Four: one piece short of the only threshold, the set is worth exactly its pieces and no
    # spell has fired. The control for the case below: without it an engine that credited a
    # bonus for any two matching items would pass everything else here.
    fields, sets = loadout(harness, 4, "combat-swords-pve", THERO_SHAN[:5])
    check("five pieces of a six-piece set", float(fields["score"]), float(fields["pieces"]))
    if not sets or int(sets[0]["count"]) != 5:
        failures.append(f"expected the five pieces to be counted as one set, got {sets}")
    elif sets[0]["spells"] != "-":
        failures.append(f"a bonus fired one piece short of the threshold: {sets[0]['spells']}")
    print(f"below threshold score={fields['score']} pieces={fields['pieces']} "
          f"spells={sets[0]['spells'] if sets else '?'}")

    # Five: the sixth piece is worth more than the sixth piece. The margin is what an
    # individually better off-set piece has to beat before breaking the set is the right
    # answer, which is the number test_item_evaluator_sets.py builds its cases around.
    fields, sets = loadout(harness, 4, "combat-swords-pve", THERO_SHAN)
    at_threshold = float(fields["score"]) - float(fields["pieces"])
    if at_threshold <= 0.0:
        failures.append("the complete set scored no more than its pieces; the set bonus is not "
                        "reaching the score")
    if not sets or sets[0]["spells"] == "-":
        failures.append(f"no set spell fired with the set complete: {sets}")
    print(f"at threshold bonus={at_threshold:.4f} score={fields['score']} "
          f"pieces={fields['pieces']} spells={sets[0]['spells'] if sets else '?'}")

    # Six: the same for a set whose threshold is three, so the behaviour is not an artefact of
    # one set's data.
    fields, sets = loadout(harness, 4, "combat-swords-pve", VIPER[:3])
    bonus = float(fields["score"]) - float(fields["pieces"])
    if bonus <= 0.0:
        failures.append("three pieces of the set scored no more than the three pieces alone; "
                        "the set bonus is not reaching the score")
    if not sets or sets[0]["spells"] == "-":
        failures.append(f"no set spell fired three pieces in: {sets}")
    print(f"three-piece bonus={bonus:.4f} score={fields['score']} pieces={fields['pieces']} "
          f"spells={sets[0]['spells'] if sets else '?'}")

    # Seven: adding pieces never loses the bonus already earned. A threshold implemented as a
    # window rather than a floor would pass every case above and fail here.
    fields_five, _ = loadout(harness, 4, "combat-swords-pve", VIPER)
    if float(fields_five["score"]) - float(fields_five["pieces"]) < bonus - TOLERANCE:
        failures.append("five pieces of the set are worth a smaller bonus than three; "
                        "a threshold is being treated as a window")
    print(f"five pieces score={fields_five['score']} pieces={fields_five['pieces']}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
