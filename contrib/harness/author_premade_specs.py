#!/usr/bin/env python3
"""Author level 60 talent specs and emit them as a world database migration.

The builds live here rather than directly in SQL because SQL cannot check itself. Premade specs
are applied with LearnSpell rather than LearnTalent, so an illegal build -- a deep talent with
nothing paid for beneath it, or points spent past what the level allows -- applies silently and
produces a bot that is quietly wrong. Every build below is checked against Talent.dbc before a
single INSERT is printed, so the migration cannot be generated unless the builds are legal.

Talents are written as names, resolved to ids through the DBC, because a bare id is unreadable
and unreviewable. Run with the DBC files present:

    python3 author_premade_specs.py > /tmp/specs.sql
"""

import sys

import talent_dbc

LEVEL = 60

# CombatBotRoles from SharedDefines.h.
ROLE_MELEE_DPS = 1
ROLE_RANGE_DPS = 2
ROLE_TANK = 3
ROLE_HEALER = 4

# Entry ids continue past the 53 templates already shipped. Kept in one block so the whole set
# can be removed again by range if the migration ever needs undoing.
FIRST_ENTRY = 101

# Each spec is (name, class, role, {tree: {talent name: rank}}). The gaps these fill are the
# level 60 builds a raid roster actually asks for and the shipped data does not have: there was
# no fire mage, no arms warrior, no destruction warlock, no dagger rogue, no discipline priest
# and no cat druid, which between them are most of a raid's damage.
SPECS = [
    (
        "fire-pve", "mage", ROLE_RANGE_DPS,
        {
            "Fire": {
                "Improved Fireball": 5,
                "Impact": 5,
                "Ignite": 5,
                "Burning Soul": 2,
                "Improved Scorch": 3,
                "Master of Elements": 3,
                "Critical Mass": 3,
                "Fire Power": 5,
                "Combustion": 1,
            },
            "Arcane": {
                "Arcane Subtlety": 2,
                "Arcane Focus": 5,
                "Wand Specialization": 1,
                "Arcane Concentration": 5,
                "Magic Attunement": 2,
                "Improved Arcane Explosion": 1,
                "Arcane Meditation": 3,
            },
        },
    ),
    (
        "arms-pve", "warrior", ROLE_MELEE_DPS,
        {
            "Arms": {
                "Improved Heroic Strike": 3,
                "Deflection": 2,
                # Improved Rend and Sweeping Strikes are not chosen for their own sake: the DBC
                # makes them prerequisites of Deep Wounds and Mortal Strike respectively.
                "Improved Rend": 3,
                "Tactical Mastery": 5,
                "Anger Management": 1,
                "Deep Wounds": 3,
                "Two-Handed Weapon Specialization": 5,
                "Impale": 2,
                "Sword Specialization": 5,
                "Sweeping Strikes": 1,
                "Mortal Strike": 1,
            },
            "Fury": {
                "Cruelty": 5,
                "Unbridled Wrath": 5,
                "Improved Battle Shout": 5,
                "Enrage": 5,
            },
        },
    ),
    (
        "destruction-pve", "warlock", ROLE_RANGE_DPS,
        {
            "Destruction": {
                "Improved Shadow Bolt": 5,
                "Bane": 5,
                "Devastation": 5,
                "Shadowburn": 1,
                "Destructive Reach": 2,
                "Intensity": 2,
                "Improved Immolate": 5,
                "Ruin": 1,
                "Emberstorm": 5,
                "Conflagrate": 1,
            },
            "Affliction": {
                "Suppression": 5,
                "Improved Corruption": 5,
                "Improved Life Tap": 2,
                "Improved Drain Soul": 2,
                "Amplify Curse": 1,
                "Improved Curse of Agony": 3,
                "Grim Reach": 1,
            },
        },
    ),
    (
        "assassination-daggers-pve", "rogue", ROLE_MELEE_DPS,
        {
            "Assassination": {
                "Malice": 5,
                "Ruthlessness": 3,
                "Murder": 2,
                "Relentless Strikes": 1,
                "Lethality": 5,
                "Improved Poisons": 5,
                "Cold Blood": 1,
                "Improved Kidney Shot": 3,
                "Seal Fate": 5,
                "Vigor": 1,
            },
            "Subtlety": {
                "Opportunity": 5,
                "Camouflage": 5,
                "Elusiveness": 1,
                "Initiative": 3,
                "Improved Ambush": 3,
                "Setup": 3,
            },
        },
    ),
    (
        "discipline-holy-pve", "priest", ROLE_HEALER,
        {
            "Discipline": {
                "Unbreakable Will": 5,
                "Improved Power Word: Shield": 3,
                "Martyrdom": 2,
                "Improved Power Word: Fortitude": 1,
                "Meditation": 3,
                "Inner Focus": 1,
                "Mental Agility": 5,
                "Divine Spirit": 1,
            },
            "Holy": {
                "Holy Specialization": 5,
                "Improved Renew": 3,
                "Divine Fury": 5,
                "Inspiration": 3,
                "Blessed Recovery": 1,
                "Improved Healing": 3,
                "Spiritual Guidance": 5,
                "Spiritual Healing": 5,
            },
        },
    ),
    (
        "feral-cat-pve", "druid", ROLE_MELEE_DPS,
        {
            "Feral Combat": {
                "Ferocity": 5,
                "Feral Instinct": 5,
                "Sharpened Claws": 3,
                "Feline Swiftness": 2,
                "Improved Shred": 2,
                "Predatory Strikes": 3,
                "Primal Fury": 2,
                "Blood Frenzy": 2,
                "Savage Fury": 2,
                "Faerie Fire (Feral)": 1,
                "Heart of the Wild": 5,
                "Leader of the Pack": 1,
            },
            "Balance": {
                "Nature's Grasp": 1,
                "Improved Nature's Grasp": 4,
                "Natural Weapons": 5,
                "Natural Shapeshifter": 3,
                "Omen of Clarity": 1,
                "Improved Thorns": 3,
                "Nature's Reach": 1,
            },
        },
    ),
]


def resolve(class_name, trees, names_by_spell):
    """Turn {tree: {talent name: rank}} into {talent id: rank}, failing loudly on a typo."""
    catalogue = {}
    for tab_id, talent in talent_dbc.talents_for_class(class_name):
        tree = talent_dbc.TAB_NAMES[tab_id]
        name = names_by_spell.get(talent.ranks[0])
        if name:
            catalogue[(tree, name)] = talent

    spend = {}
    for tree, wanted in trees.items():
        for name, rank in wanted.items():
            talent = catalogue.get((tree, name))
            if not talent:
                raise KeyError("%s has no talent %r in %s" % (class_name, name, tree))
            if rank > talent.max_rank:
                raise ValueError("%s %s caps at rank %u, asked for %u"
                                 % (class_name, name, talent.max_rank, rank))
            spend[talent.talent_id] = rank

    return spend


def sql_escape(text):
    return text.replace("\\", "\\\\").replace("'", "\\'")


def main():
    every_spell = set()
    for _, class_name, _, _ in SPECS:
        for _, talent in talent_dbc.talents_for_class(class_name):
            every_spell.update(talent.ranks)
    names_by_spell = talent_dbc.spell_names(every_spell)
    if not names_by_spell:
        print("could not read spell names from the database", file=sys.stderr)
        return 1

    failed = False
    lines = []
    for offset, (spec_name, class_name, role, trees) in enumerate(SPECS):
        entry = FIRST_ENTRY + offset
        spend = resolve(class_name, trees, names_by_spell)

        problems = talent_dbc.validate_build(class_name, spend, level=LEVEL)
        if problems:
            failed = True
            print("%s (%s) is not a legal build:" % (spec_name, class_name), file=sys.stderr)
            for problem in problems:
                print("    %s" % problem, file=sys.stderr)
            continue

        spells = talent_dbc.build_spell_ids(spend)
        spent = sum(spend.values())
        summary = ", ".join("%s %u" % (tree, sum(ranks.values()))
                            for tree, ranks in trees.items())

        lines.append("-- %s: %s, %u points (%s)" % (spec_name, class_name, spent, summary))
        lines.append("INSERT INTO `player_premade_spell_template`"
                     " (`entry`, `class`, `level`, `role`, `name`) VALUES (%u, %u, %u, %u, '%s');"
                     % (entry, talent_dbc.CLASS_IDS[class_name], LEVEL, role,
                        sql_escape(spec_name)))
        values = ", ".join("(%u, %u)" % (entry, spell) for spell in spells)
        lines.append("INSERT INTO `player_premade_spell` (`entry`, `spell`) VALUES %s;" % values)
        lines.append("")

    if failed:
        return 1

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
