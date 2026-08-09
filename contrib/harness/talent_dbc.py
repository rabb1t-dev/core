#!/usr/bin/env python3
"""Read Talent.dbc and TalentTab.dbc so talent builds can be authored against real data.

Talent trees are client data, not server data: there is no `talent` table in the world
database, so a build written by hand has nothing to check it. That matters more than usual
here, because `player_premade_spell` templates are applied with LearnSpell rather than
LearnTalent, which grants a talent without checking that the tree beneath it was paid for.
An illegal build applies silently.

This module parses the two DBCs the server itself reads and exposes enough structure to
generate a build and prove it legal before it ever reaches the database.

    python3 talent_dbc.py --class mage --tree Fire
    python3 talent_dbc.py --class warrior --json
"""

import argparse
import functools
import json
import os
import struct
import subprocess
import sys

DEFAULT_DBC_DIR = os.path.expanduser("~/server/data/5875/dbc")

# Field layouts, taken from DBCfmt.h so that this agrees with the server rather than with a
# wiki. Every field is a 4 byte little endian word; 'x' fields are ones the server skips.
#   TalentEntryfmt    "niiiiiiiixxxxixxixxxi"
#   TalentTabEntryfmt "nxxxxxxxxxxxiix"
TALENT_FIELDS = 21
TALENT_TAB_FIELDS = 15

CLASS_MASKS = {
    "warrior": 1,
    "paladin": 2,
    "hunter": 4,
    "rogue": 8,
    "priest": 16,
    "shaman": 64,
    "mage": 128,
    "warlock": 256,
    "druid": 1024,
}

CLASS_IDS = {
    "warrior": 1,
    "paladin": 2,
    "hunter": 3,
    "rogue": 4,
    "priest": 5,
    "shaman": 7,
    "mage": 8,
    "warlock": 9,
    "druid": 11,
}

# TalentTab.dbc drops the name column in the server's format string, so tree names are supplied
# here. Keyed by tab id rather than by the tabpage ordering column, because that column is wrong
# for mage: tabs 41 (Fire) and 81 (Arcane) both claim page 0. Every other class is consistent,
# which is exactly the kind of single-case data flaw that ordering-based naming hides.
TAB_NAMES = {
    161: "Arms", 164: "Fury", 163: "Protection",                    # warrior
    382: "Holy", 383: "Protection", 381: "Retribution",             # paladin
    361: "Beast Mastery", 363: "Marksmanship", 362: "Survival",     # hunter
    182: "Assassination", 181: "Combat", 183: "Subtlety",           # rogue
    201: "Discipline", 202: "Holy", 203: "Shadow",                  # priest
    261: "Elemental", 263: "Enhancement", 262: "Restoration",       # shaman
    81: "Arcane", 41: "Fire", 61: "Frost",                          # mage
    302: "Affliction", 303: "Demonology", 301: "Destruction",       # warlock
    283: "Balance", 281: "Feral Combat", 282: "Restoration",        # druid
}

# Tab ids in the order the client shows them, since tabpage cannot be trusted to supply it.
CLASS_TABS = {
    "warrior": [161, 164, 163],
    "paladin": [382, 383, 381],
    "hunter": [361, 363, 362],
    "rogue": [182, 181, 183],
    "priest": [201, 202, 203],
    "shaman": [261, 263, 262],
    "mage": [81, 41, 61],
    "warlock": [302, 303, 301],
    "druid": [283, 281, 282],
}


def read_dbc(path, expected_fields):
    """Return a list of records, each a tuple of uint32, plus the raw string block."""
    with open(path, "rb") as handle:
        blob = handle.read()

    magic, count, fields, record_size, string_size = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise ValueError("%s is not a DBC file" % path)
    if fields != expected_fields:
        raise ValueError("%s has %u fields, expected %u" % (path, fields, expected_fields))
    if record_size != fields * 4:
        raise ValueError("%s has %u byte records, expected %u" % (path, record_size, fields * 4))

    records = []
    offset = 20
    for _ in range(count):
        records.append(struct.unpack_from("<%uI" % fields, blob, offset))
        offset += record_size

    return records


class Talent(object):
    __slots__ = ("talent_id", "tab_id", "row", "col", "ranks", "depends_on",
                 "depends_on_rank", "depends_on_spell")

    def __init__(self, record):
        self.talent_id = record[0]
        self.tab_id = record[1]
        self.row = record[2]
        self.col = record[3]
        self.ranks = [spell for spell in record[4:9] if spell]
        self.depends_on = record[13]
        self.depends_on_rank = record[16]
        self.depends_on_spell = record[20]

    @property
    def max_rank(self):
        return len(self.ranks)


@functools.lru_cache(maxsize=4)
def load_talents(dbc_dir=DEFAULT_DBC_DIR):
    """Return (talents by id, tab id -> (class mask, tabpage)).

    Cached because validating a spend order re-reads this once per prefix, and a 51 point
    build has 51 prefixes.
    """
    talents = {}
    for record in read_dbc(os.path.join(dbc_dir, "Talent.dbc"), TALENT_FIELDS):
        talent = Talent(record)
        if talent.ranks:
            talents[talent.talent_id] = talent

    tabs = {}
    for record in read_dbc(os.path.join(dbc_dir, "TalentTab.dbc"), TALENT_TAB_FIELDS):
        tabs[record[0]] = (record[12], record[13])

    return talents, tabs


def spell_names(spell_ids, db="mangos", user="mangos", password="mangos", build=5875):
    """Map spell id to name using the world database, since DBC names are not loaded."""
    if not spell_ids:
        return {}

    # spell_template holds a row per client build, but only for builds where the spell actually
    # changed, so an exact build match finds almost nothing. Take the newest row at or below the
    # build we care about, which is how the server resolves it too.
    id_list = ",".join(str(int(spell)) for spell in sorted(spell_ids))
    query = ("SELECT s.entry, s.name FROM spell_template s"
             " JOIN (SELECT entry, MAX(build) AS build FROM spell_template"
             "       WHERE build <= %u AND entry IN (%s) GROUP BY entry) newest"
             "   ON newest.entry = s.entry AND newest.build = s.build"
             % (build, id_list))
    output = subprocess.check_output(
        ["mysql", "-N", "-B", "-u" + user, "-p" + password, db, "-e", query],
        stderr=subprocess.DEVNULL).decode("utf-8", "replace")

    names = {}
    for line in output.splitlines():
        if "\t" not in line:
            continue
        entry, name = line.split("\t", 1)
        names[int(entry)] = name
    return names


def talents_for_class(class_name, dbc_dir=DEFAULT_DBC_DIR):
    """Return (tab id, talent) for this class, in client tree order then row then column."""
    talents, tabs = load_talents(dbc_dir)
    class_mask = CLASS_MASKS[class_name]
    tab_order = CLASS_TABS[class_name]

    selected = []
    for talent in talents.values():
        tab = tabs.get(talent.tab_id)
        if not tab or not (tab[0] & class_mask):
            continue
        selected.append((talent.tab_id, talent))

    selected.sort(key=lambda pair: (tab_order.index(pair[0]), pair[1].row, pair[1].col))
    return selected


def points_for_level(level):
    """Vanilla grants the first talent point at level 10 and one per level after."""
    return max(0, level - 9)


def validate_build(class_name, spend, dbc_dir=DEFAULT_DBC_DIR, level=60):
    """Check a {talent_id: rank} build against the rules LearnSpell would not enforce.

    Returns a list of human readable problems, empty when the build is legal.
    """
    talents, tabs = load_talents(dbc_dir)
    class_mask = CLASS_MASKS[class_name]
    problems = []

    spent_by_tab_row = {}
    total = 0
    for talent_id, rank in spend.items():
        talent = talents.get(talent_id)
        if not talent:
            problems.append("talent %u does not exist" % talent_id)
            continue

        tab = tabs.get(talent.tab_id)
        if not tab or not (tab[0] & class_mask):
            problems.append("talent %u is not available to %s" % (talent_id, class_name))
            continue

        if rank < 1 or rank > talent.max_rank:
            problems.append("talent %u rank %u is outside 1..%u"
                            % (talent_id, rank, talent.max_rank))
            continue

        total += rank
        spent_by_tab_row.setdefault(talent.tab_id, {}).setdefault(talent.row, 0)
        spent_by_tab_row[talent.tab_id][talent.row] += rank

    available = points_for_level(level)
    if total > available:
        problems.append("spends %u points, only %u available at level %u"
                        % (total, available, level))
    elif total < available:
        problems.append("spends %u of %u points, leaving %u unspent"
                        % (total, available, available - total))

    for talent_id, rank in spend.items():
        talent = talents.get(talent_id)
        if not talent:
            continue

        rows = spent_by_tab_row.get(talent.tab_id, {})
        above = sum(points for row, points in rows.items() if row < talent.row)
        if above < talent.row * 5:
            problems.append("talent %u sits on row %u needing %u points above it, has %u"
                            % (talent_id, talent.row, talent.row * 5, above))

        if talent.depends_on:
            have = spend.get(talent.depends_on, 0)
            if have <= talent.depends_on_rank:
                problems.append("talent %u needs talent %u at rank above %u, has %u"
                                % (talent_id, talent.depends_on, talent.depends_on_rank, have))

    return problems


def build_spell_ids(spend, dbc_dir=DEFAULT_DBC_DIR):
    """Return the spell ids a build's ranks resolve to, which is what a template stores."""
    talents, _ = load_talents(dbc_dir)
    spells = []
    for talent_id, rank in sorted(spend.items()):
        talent = talents[talent_id]
        spells.append(talent.ranks[rank - 1])
    return spells


def spend_order(class_name, spend, tree_order, dbc_dir=DEFAULT_DBC_DIR):
    """Turn a finished build into the order its points are spent, one entry per point.

    Returns a list of (talent id, rank), where rank counts up from 1, so the list is exactly as
    long as the build's point total and its first N entries are what a character with N points
    should have. That is the whole reason this exists: a template is otherwise a finished set
    with no way to be worth anything below the level it was authored for.

    Trees are spent in the order given, and within a tree by row and then column, which is
    legal for free. A talent on row r needs 5r points above it in its own tree, and the
    finished build already satisfies that, so filling rows top down can never reach a row
    early. Prerequisites are the one thing row order does not settle by itself, so a talent
    whose prerequisite is not yet paid for is deferred and retried.

    What this does not model is a human's priorities. Row order inside a tree means a talent
    that a real character would rush is taken whenever its row comes up, and a later tree is
    untouched until the one before it is finished. Ordering the trees is therefore the author's
    lever, and the only one: at the full level the build is identical either way.
    """
    talents, _ = load_talents(dbc_dir)
    order_of_tree = {name: index for index, name in enumerate(tree_order)}

    remaining = []
    for tab_id, talent in talents_for_class(class_name, dbc_dir):
        if talent.talent_id in spend:
            tree = TAB_NAMES[tab_id]
            remaining.append((order_of_tree.get(tree, len(order_of_tree)),
                              talent.row, talent.col, talent))
    remaining.sort(key=lambda item: item[:3])

    ordered = []
    taken = {}
    while remaining:
        for index, item in enumerate(remaining):
            talent = item[3]
            # A prerequisite is satisfied only above the named rank, matching the check the
            # client makes and the one validate_build makes.
            if talent.depends_on and taken.get(talent.depends_on, 0) <= talent.depends_on_rank:
                continue

            for rank in range(1, spend[talent.talent_id] + 1):
                ordered.append((talent.talent_id, rank))
            taken[talent.talent_id] = spend[talent.talent_id]
            del remaining[index]
            break
        else:
            stuck = ", ".join(str(item[3].talent_id) for item in remaining)
            raise ValueError("%s: no legal order, stuck on talents %s" % (class_name, stuck))

    return ordered


def validate_spend_order(class_name, ordered, dbc_dir=DEFAULT_DBC_DIR):
    """Check that every prefix of a spend order is itself a legal build.

    Validating only the finished build is not enough once a template can be applied partially.
    A prefix is what a character below the authored level actually receives, so an order whose
    thirtieth point stands on a row it has not paid for produces an illegal level 39 character
    from a build that is perfectly legal at 60.
    """
    problems = []
    for length in range(1, len(ordered) + 1):
        spend = {}
        for talent_id, rank in ordered[:length]:
            spend[talent_id] = max(spend.get(talent_id, 0), rank)

        # A prefix of N points is exactly what the level granting N points must produce.
        for problem in validate_build(class_name, spend, dbc_dir, level=length + 9):
            problems.append("after %u points: %s" % (length, problem))

    return problems


def spend_order_spell_ids(ordered, dbc_dir=DEFAULT_DBC_DIR):
    """The spell id for each point of a spend order, which is what a template row stores."""
    talents, _ = load_talents(dbc_dir)
    return [talents[talent_id].ranks[rank - 1] for talent_id, rank in ordered]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dbc", default=DEFAULT_DBC_DIR, help="directory holding the DBC files")
    parser.add_argument("--class", dest="class_name", required=True,
                        choices=sorted(CLASS_MASKS), help="class to list talents for")
    parser.add_argument("--tree", help="only this tree, by name")
    parser.add_argument("--json", action="store_true", help="emit json instead of a table")
    parser.add_argument("--no-names", action="store_true",
                        help="skip the database lookup for spell names")
    args = parser.parse_args()

    selected = talents_for_class(args.class_name, args.dbc)

    if args.tree:
        wanted = args.tree.lower()
        selected = [pair for pair in selected if TAB_NAMES[pair[0]].lower() == wanted]
        if not selected:
            print("no tree named %r for %s" % (args.tree, args.class_name), file=sys.stderr)
            return 1

    names = {}
    if not args.no_names:
        every_spell = set()
        for _, talent in selected:
            every_spell.update(talent.ranks)
        try:
            names = spell_names(every_spell)
        except Exception as error:                                  # noqa: BLE001
            print("spell names unavailable (%s)" % error, file=sys.stderr)

    if args.json:
        payload = []
        for tab_id, talent in selected:
            payload.append({
                "tree": TAB_NAMES[tab_id],
                "talent_id": talent.talent_id,
                "row": talent.row,
                "col": talent.col,
                "max_rank": talent.max_rank,
                "ranks": talent.ranks,
                "depends_on": talent.depends_on,
                "depends_on_rank": talent.depends_on_rank,
                "name": names.get(talent.ranks[0], ""),
            })
        print(json.dumps(payload, indent=2))
        return 0

    current_tree = None
    for tab_id, talent in selected:
        if tab_id != current_tree:
            current_tree = tab_id
            print("\n== %s ==" % TAB_NAMES[tab_id])
        print("talent=%-5u row=%u col=%u max=%u ranks=%-30s %s"
              % (talent.talent_id, talent.row, talent.col, talent.max_rank,
                 ",".join(str(spell) for spell in talent.ranks),
                 names.get(talent.ranks[0], "")))

    return 0


if __name__ == "__main__":
    sys.exit(main())
