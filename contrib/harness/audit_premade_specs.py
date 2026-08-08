#!/usr/bin/env python3
"""Spawn a bot for every level 60 premade spec and report what the build actually is.

Counting a template's talent points by matching `player_premade_spell` rows against Talent.dbc
does not work: a template may store a rank id the DBC chain does not list, and the count comes
out low. The only trustworthy measure is to apply the spec to a real character and read the
build back, which is what this does.

The point is the summary line per spec: points spent against points the level allows, and any
talent standing on a tier it did not pay for. Specs are applied with LearnSpell rather than
LearnTalent, so an illegal build applies in silence and this is the only thing that says so.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM account:
python3 audit_premade_specs.py
"""

import argparse
import subprocess
import sys
import time

from vmangos_harness import Harness
from test_premade_specs import (LEVEL, POINTS_AT_60, add_bot, build_of, check_legal,
                                clear_roster, provision_leader)

# Bot classes the party bot command understands, keyed by the class id in the template table.
CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest",
               7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}


def templates(level):
    """Every spec template at a level, as (entry, class name, spec name)."""
    rows = subprocess.run(
        ["sudo", "mysql", "mangos", "-N", "-B", "-e",
         "SELECT `entry`, `class`, `name` FROM `player_premade_spell_template` "
         "WHERE `level` = %u ORDER BY `class`, `entry`;" % level],
        capture_output=True, text=True, check=True).stdout

    found = []
    for line in rows.splitlines():
        entry, class_id, name = line.split("\t")
        class_name = CLASS_NAMES.get(int(class_id))
        if class_name:
            found.append((int(entry), class_name, name))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", help="SOAP url, defaults to the harness default")
    args = parser.parse_args()

    harness = Harness.from_env(url=args.url)
    leader = provision_leader(harness)

    problems = []
    try:
        for entry, class_name, spec in templates(LEVEL):
            clear_roster(harness, leader)
            # Asked for by entry rather than name because names are not unique across classes:
            # both a paladin and a priest ship a spec called holy-pve.
            bot = add_bot(harness, leader, class_name, str(entry))
            if not bot:
                problems.append(f"{entry} {spec}: no bot joined the group")
                continue

            try:
                data = build_of(harness, bot)
                label = f"{entry} {class_name} {spec}"
                problems.extend(check_legal(label, data))

                summary = data.get("summary", {})
                trees = {k: v for k, v in data.get("trees", {}).items() if v}
                print("%-4s %-8s %-24s spent=%2s/%s illegal=%s  %s"
                      % (entry, class_name, spec, summary.get("spent", "?"), POINTS_AT_60,
                         summary.get("illegal", "?"),
                         ", ".join("%s %d" % kv for kv in sorted(trees.items(),
                                                                 key=lambda kv: -kv[1]))))
            finally:
                harness.run(f"harness exec {bot} partybot remove", allow_failure=True)
                time.sleep(2.0)
    finally:
        clear_roster(harness, leader)

    if problems:
        print("\nPROBLEMS")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print("\nevery template spends its budget and stands on legal tiers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
