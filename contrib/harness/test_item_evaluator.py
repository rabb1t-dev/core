#!/usr/bin/env python3
"""Differential test for the item evaluation engine against Classic Gear Ranker.

The oracle is `fixtures/classic_gear_ranker_items.tsv` and
`fixtures/classic_gear_ranker_spells.tsv`, converted once from the spreadsheet so this
test has no dependency on Excel. For every item entry the harness dumps what
`ItemEvaluator::ResolveItem` thinks the item contributes, and every disagreement is a
real finding rather than a flaky assertion: the engine and the sheet are supposed to be
looking at the same game data.

Where the two disagree, the server data wins. The engine has to predict what the server
will actually apply to the character; the sheet is a cross-check, not the authority. Every
divergence below was traced back to item_template or spell_template first, and each is
explained precisely rather than waved through, so a real regression in one of these fields
still fails the run.

Two columns are not compared at all:

  ranged_ap  The sheet has no ranged attack power anywhere. Its "Ranged" column is ranged
             weapon skill: all twenty of its Ranged spell rows are Increased Bow, Increased
             Gun or Increased Crossbow, and items whose only ranged contribution is attack
             power read zero. The engine keeps the two apart, since a point of bow skill
             and a point of ranged attack power are not remotely the same thing, and folds
             ranged weapon skill into weapon_skill where it belongs. So the sheet's Ranged
             column is checked against the engine's weapon_skill, and the engine's
             ranged_ap has no oracle.

  dps        Compared with a tolerance, since the sheet rounds.

Run on the server host: python3 test_item_evaluator.py
"""

import os
import re
import sys

from vmangos_harness import Harness

HERE = os.path.dirname(os.path.abspath(__file__))
ITEMS = os.path.join(HERE, "fixtures", "classic_gear_ranker_items.tsv")
SPELLS = os.path.join(HERE, "fixtures", "classic_gear_ranker_spells.tsv")
FAILURE_LOG = os.environ.get(
    "ITEM_EVALUATOR_FAILURES", "/tmp/item_evaluator_failures.txt")

ITEM_CLASS_WEAPON = 2
AXE_SUBCLASSES = {0, 1}  # one- and two-handed, both covered by the racial

# Fields present on both the oracle and the harness dump. See the docstring for the two
# that are deliberately absent.
INT_FIELDS = [
    "armor", "stam", "spi", "int", "str", "agi",
    "ap", "hit", "crit", "weapon_skill", "defense", "dodge", "parry",
    "block", "block_value",
    "spdmg", "sppen", "sphit", "spcrit", "spheal", "mp5",
    "fire_res", "nat_res", "frost_res",
]

# The sheet signs spell penetration as the resistance debuff it is, so a 10 point
# penetration enchant reads -10. The engine stores the magnitude, because the weight schema
# treats every column as "more is better". Compared on magnitude alone.
SIGNLESS_FIELDS = {"sppen"}

# The sheet is built for an orc warrior, so every axe row carries the +5 weapon skill the
# Axe Specialization racial grants. That belongs to the wearer's race, not to the item, and
# has no place in a race-neutral item vector.
ORC_AXE_SPECIALIZATION = 5

# Spell rows the sheet has outright wrong, keyed by (spell, the sheet's own stat label).
SPELL_SHEET_ERRORS = {
    (14249, "Weapon"):
        "Increased Defense filed under Weapon; the sheet carries the correct Defense row too",
    (25113, "SpDmg"):
        "sheet says 61, but the spell is named 'Increase Spell Dam 36 and 1% Crit' and grants 36",
    (21539, "Block Value"):
        "amount read off the name 'Block Value 02'; the effect itself grants 1",
}

# Sheet stat label -> harness key. "Ranged" lands on weapon_skill: see the docstring.
SPELL_STAT_MAP = {
    "Crit": "crit",
    "Hit": "hit",
    "AP": "ap",
    "Defense": "defense",
    "Dodge": "dodge",
    "Parry": "parry",
    "Weapon": "weapon_skill",
    "Ranged": "weapon_skill",
    "Block Value": "block_value",
    "Block": "block",
    "SpDmg": "spdmg",
    "SpPen": "sppen",
    "SpHit": "sphit",
    "SpCrit": "spcrit",
    "SpHeal": "spheal",
    "Mp5": "mp5",
}


def parse_kv(text):
    return dict(re.findall(r"(\w+)=(\S+)", text))


def load_items():
    with open(ITEMS) as f:
        header = f.readline().rstrip("\n").split("\t")
        return [dict(zip(header, line.rstrip("\n").split("\t"))) for line in f]


def load_spells():
    with open(SPELLS) as f:
        header = f.readline().rstrip("\n").split("\t")
        return [dict(zip(header, line.rstrip("\n").split("\t"))) for line in f]


class Differ:
    def __init__(self, harness, sheet_spells):
        self.harness = harness
        self.sheet_spells = sheet_spells
        self.spell_cache = {}

    def spell_stats(self, spell_id):
        if spell_id not in self.spell_cache:
            text = self.harness.run(
                f"harness spellstats {spell_id}", allow_failure=True)
            self.spell_cache[spell_id] = parse_kv(text)
        return self.spell_cache[spell_id]

    def spell_contributions(self, got, field):
        """Each equip spell's contribution to one field, in the order they are applied."""
        raw = got.get("equip_spells", "-")
        if raw == "-":
            return []
        return [int(float(self.spell_stats(int(part)).get(field, "0")))
                for part in raw.split(",")]

    def unseen_spell_contribution(self, got, field):
        """What the item's equip spells that the sheet never recorded add to one field.

        The sheet's 498-row spell table is incomplete: mana regen, ranged attack power and a
        handful of others are missing entirely for some spell ids, so an item whose only
        contribution to a column comes from one of those reads zero on the sheet. Rather
        than wave the whole column through, attribute the difference to the exact spells the
        sheet has no row for, and only accept it if the arithmetic comes out.
        """
        raw = got.get("equip_spells", "-")
        if raw == "-":
            return 0, []

        total = 0
        unseen = []
        for part in raw.split(","):
            spell_id = int(part)
            if spell_id in self.sheet_spells:
                continue
            amount = int(float(self.spell_stats(spell_id).get(field, "0")))
            if amount:
                total += amount
                unseen.append(spell_id)
        return total, unseen

    def artifact(self, entry, field, want, have, got):
        """Explain a mismatch as a known sheet artifact, or return None if it is real."""
        if field == "weapon_skill":
            if want - have == ORC_AXE_SPECIALIZATION \
                    and int(got.get("class", -1)) == ITEM_CLASS_WEAPON \
                    and int(got.get("subclass", -1)) in AXE_SUBCLASSES:
                return "orc Axe Specialization baked into the sheet's axe rows"

            # Gear that raises bow, gun and crossbow skill together raises whichever the
            # character is actually shooting with, so it is worth the one amount. The sheet
            # adds the three up in its Ranged column.
            grants = [a for a in self.spell_contributions(got, field) if a]
            if len(grants) > 1 and want == sum(grants) and have == max(grants):
                return f"sheet sums {grants} across weapon skills only one of which applies"

        # The sheet's Block Value column counts only what equip effects add, never the base
        # block printed on the shield itself, which is the larger half of the stat.
        base_block = int(got.get("base_block", 0))
        if field == "block_value" and base_block > 0 and have - want == base_block:
            return f"shield base block of {base_block} that the sheet omits"

        if want == 0 and have < 0:
            return "sheet zeroes negative stats"

        if want == 0 and have > 0:
            contribution, unseen = self.unseen_spell_contribution(got, field)
            if unseen and contribution == have:
                return f"granted by {unseen}, absent from the sheet's spell table"

        return None


def main():
    harness = Harness.from_env()
    sheet_spell_rows = load_spells()
    differ = Differ(harness, {int(r["spell_id"]) for r in sheet_spell_rows})

    failures = []
    artifacts = []
    checked = 0
    skipped_bias = 0
    skipped_missing = 0

    print("item differential ...")
    for row in load_items():
        entry = int(row["entry"])
        if row.get("bias"):
            skipped_bias += 1
            continue

        text = harness.run(f"harness itemstats {entry}", allow_failure=True)
        if "does not exist" in text:
            skipped_missing += 1
            continue

        got = parse_kv(text)
        checked += 1
        for field in INT_FIELDS:
            want = int(float(row.get(field, "0") or 0))
            if field == "weapon_skill":
                # The sheet splits weapon skill across two columns, melee in weapon_skill
                # and ranged in the one labelled Ranged. The engine has a single column.
                want += int(float(row.get("ranged_ap", "0") or 0))

            have = int(float(got.get(field, "0")))
            if field in SIGNLESS_FIELDS:
                want, have = abs(want), abs(have)
            if want == have:
                continue

            reason = differ.artifact(entry, field, want, have, got)
            if reason:
                artifacts.append(f"item {entry} {field}: {reason}")
            else:
                failures.append(f"item {entry} {field}: sheet={want} engine={have}")

        want_dps = float(row.get("dps") or 0)
        if want_dps:
            have_dps = float(got.get("dps", "0"))
            drift = abs(want_dps - have_dps)
            if drift > 0.05 and drift / max(want_dps, 0.01) > 0.02:
                failures.append(f"item {entry} dps: sheet={want_dps} engine={have_dps}")

    item_fail = len(failures)
    item_artifacts = len(artifacts)
    print(f"items checked={checked} bias={skipped_bias} missing={skipped_missing} "
          f"artifacts={item_artifacts} mismatches={item_fail}")

    print("spell differential ...")
    spell_checked = 0
    for row in sheet_spell_rows:
        spell_id = int(row["spell_id"])
        sheet_stat = row["stat"]
        sheet_amount = int(float(row["amount"]))
        key = SPELL_STAT_MAP.get(sheet_stat)
        if not key:
            failures.append(f"spell {spell_id}: unknown sheet stat {sheet_stat!r}")
            continue

        have = int(float(differ.spell_stats(spell_id).get(key, "0")))
        if key in SIGNLESS_FIELDS:
            sheet_amount, have = abs(sheet_amount), abs(have)
        spell_checked += 1
        if have == sheet_amount:
            continue

        reason = SPELL_SHEET_ERRORS.get((spell_id, sheet_stat))
        if reason:
            artifacts.append(f"spell {spell_id} {sheet_stat}: {reason}")
        else:
            failures.append(
                f"spell {spell_id} {sheet_stat}: sheet={sheet_amount} engine={have}")

    print(f"spells checked={spell_checked} "
          f"artifacts={len(artifacts) - item_artifacts} "
          f"mismatches={len(failures) - item_fail}")

    with open(FAILURE_LOG, "w") as f:
        for entry in artifacts:
            f.write(f"ARTIFACT: {entry}\n")
        for failure in failures:
            f.write(f"FAIL: {failure}\n")

    # Cap the printed failures so a catastrophic resolution bug stays readable; the file
    # written above always holds the complete list.
    for failure in failures[:40]:
        print(f"FAIL: {failure}", file=sys.stderr)
    if len(failures) > 40:
        print(f"FAIL: ... and {len(failures) - 40} more", file=sys.stderr)
    print(f"full list in {FAILURE_LOG}")

    if failures:
        print(f"FAILED ({len(failures)} disagreements)")
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
