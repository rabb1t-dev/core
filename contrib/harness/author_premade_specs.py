#!/usr/bin/env python3
"""Author level 60 talent specs and emit them as a world database migration.

The builds live here rather than directly in SQL because SQL cannot check itself. Premade specs
are applied with LearnSpell rather than LearnTalent, so an illegal build -- a deep talent with
nothing paid for beneath it, or points spent past what the level allows -- applies silently and
produces a bot that is quietly wrong. Every build below is checked against Talent.dbc before a
single INSERT is printed, so the migration cannot be generated unless the builds are legal.

Each spec is emitted as an ordered spend list, one row per talent point, rather than as a set
of finished talents. That is what lets one row set serve every level: a character with 36
points takes the first 36 rows. The order is checked at every prefix, not just at the end,
because a prefix is a real character and an order can be legal at 60 while illegal at 39.

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
# no fire mage, no arms warrior, no SM/Ruin warlock, no dagger rogue, no full-budget priest
# healer and no cat druid, which between them are most of a raid's damage.
#
# The builds are the ones vanilla theorycraft settled on, not invented here. Sources, all for
# 1.12: Warcraft Tavern and vanillawowaddons for the mage; Icy Veins and Warcraft Tavern for the
# warlock; Icy Veins, Warcraft Tavern and legacy-wow for the rogue; IGN and Odealo for the
# priest; Icy Veins and Warcraft Tavern for the druid. Where a published point split is not
# actually legal under the tier rules, the comment on that spec says so and says what gives.
#
# The rule applied to every filler point is that a raid boss cannot be stunned, feared or
# disarmed and does not attack the caster, so talents that only matter against players are dead
# weight. An earlier version of this file spent points on Impact, Deflection, Martyrdom,
# Improved Nature's Grasp and Improved Thorns for no reason beyond reaching the next row.
SPECS = [
    (
        # Standard fire raid build. Published as 17/31/3, which cannot be built: Arcane
        # Meditation sits on row 3 and so needs 15 points above it in the tree, and 15 + 3
        # is 18. The extra point comes out of Frost, whose only purpose here is spell hit.
        # Fire is exactly 31 because Combustion needs 30 beneath it, so there is no slack.
        "fire-pve", "mage", ROLE_RANGE_DPS,
        {
            "Fire": {
                "Improved Fireball": 5,
                "Ignite": 5,
                "Flame Throwing": 2,
                "Pyroblast": 1,
                "Burning Soul": 2,
                # Improved Scorch is the reason a fire mage is in the raid at all: it is the
                # Fire Vulnerability debuff every other fire caster benefits from.
                "Improved Scorch": 3,
                "Master of Elements": 3,
                "Critical Mass": 3,
                "Blast Wave": 1,
                "Fire Power": 5,
                "Combustion": 1,
            },
            "Arcane": {
                "Arcane Subtlety": 2,
                "Arcane Focus": 5,
                "Magic Absorption": 3,
                "Arcane Concentration": 5,
                "Arcane Meditation": 3,
            },
            "Frost": {
                "Elemental Precision": 2,
            },
        },
    ),
    (
        # Arms 31/20, the two-handed raid build. Complements the shipped fury-dw-pve rather
        # than competing with it: this is the Mortal Strike healing debuff and Sweeping
        # Strikes cleave, not the higher personal damage of dual wield fury.
        "arms-pve", "warrior", ROLE_MELEE_DPS,
        {
            "Arms": {
                "Improved Heroic Strike": 3,
                # Improved Rend and Sweeping Strikes are not chosen for their own sake: the DBC
                # makes them prerequisites of Deep Wounds and Mortal Strike respectively.
                "Improved Rend": 3,
                "Tactical Mastery": 5,
                "Improved Overpower": 2,
                "Anger Management": 1,
                "Deep Wounds": 3,
                "Two-Handed Weapon Specialization": 5,
                "Impale": 2,
                "Sweeping Strikes": 1,
                "Axe Specialization": 5,
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
        # SM/Ruin 30/0/21. The shipped ds-ruin-pve is the other half of the pair, so this is
        # the build that keeps a pet: the imp's Blood Pact stamina buff is why every raid runs
        # at least one of these. Ruin needs 20 points beneath it, which fixes Destruction at
        # 21 and leaves exactly 30 for Shadow Mastery.
        "sm-ruin-pve", "warlock", ROLE_RANGE_DPS,
        {
            "Affliction": {
                "Suppression": 5,
                "Improved Corruption": 5,
                "Improved Drain Soul": 2,
                "Improved Life Tap": 2,
                "Improved Curse of Agony": 3,
                "Fel Concentration": 1,
                "Amplify Curse": 1,
                "Grim Reach": 2,
                "Nightfall": 2,
                "Siphon Life": 1,
                "Curse of Exhaustion": 1,
                "Shadow Mastery": 5,
            },
            "Destruction": {
                "Improved Shadow Bolt": 5,
                "Bane": 5,
                # Worth taking only because this build keeps the imp out.
                "Improved Firebolt": 2,
                "Devastation": 5,
                "Shadowburn": 1,
                "Destructive Reach": 2,
                "Ruin": 1,
            },
        },
    ),
    (
        # Seal Fate daggers 30/16/5. Combat stops at 16 on purpose: Dagger Specialization
        # needs 15 points beneath it and there are not 20 to spare, so the sixteenth point
        # goes to Dual Wield Specialization and the damage comes from Opportunity instead.
        # Vigor is deliberately skipped -- 10 energy is worth less than Opportunity's 20% on
        # every Backstab and Ambush.
        "seal-fate-daggers-pve", "rogue", ROLE_MELEE_DPS,
        {
            "Assassination": {
                "Malice": 5,
                "Improved Eviscerate": 3,
                "Ruthlessness": 3,
                "Murder": 2,
                "Relentless Strikes": 1,
                "Lethality": 5,
                "Improved Poisons": 5,
                "Cold Blood": 1,
                "Seal Fate": 5,
            },
            "Combat": {
                "Lightning Reflexes": 5,
                "Improved Backstab": 3,
                "Precision": 5,
                "Endurance": 2,
                "Dual Wield Specialization": 1,
            },
            "Subtlety": {
                "Opportunity": 5,
            },
        },
    ),
    (
        # Deep holy raid healing, 21/30/0. The shipped priest holy-pve is 20/29 and leaves two
        # points unspent, so this is the same role built to the full budget. Unbreakable Will
        # and Silent Resolve are the accepted filler for the first two Discipline rows: a
        # healer has nothing else to buy there, and threat is not what kills healers.
        "discipline-holy-pve", "priest", ROLE_HEALER,
        {
            # Holy is spent before Discipline even though Discipline is the shallower tree,
            # because a partly levelled healer wants Holy Specialization and Divine Fury long
            # before it wants a stronger Fortitude buff.
            "Holy": {
                "Improved Renew": 3,
                "Holy Specialization": 5,
                "Divine Fury": 5,
                "Holy Nova": 1,
                "Inspiration": 3,
                "Improved Healing": 3,
                "Spiritual Guidance": 5,
                "Spiritual Healing": 5,
            },
            "Discipline": {
                "Unbreakable Will": 5,
                "Silent Resolve": 1,
                "Improved Power Word: Fortitude": 2,
                "Improved Power Word: Shield": 3,
                "Inner Focus": 1,
                "Meditation": 3,
                "Mental Agility": 5,
                "Divine Spirit": 1,
            },
        },
    ),
    (
        # Feral cat 14/32/5, the powershifting build. The five Restoration points are the
        # whole point: Furor returns 40 energy on every shift into cat form, and without it
        # the rotation this spec exists for does not work. Natural Shapeshifter pays for it by
        # cutting the mana cost of all that shifting.
        #
        # Primal Fury is skipped even though most published lists include it, because those
        # lists are for a druid who also off-tanks: the tooltip grants rage on a critical
        # strike in Bear and Dire Bear Form only, and this template is cat damage. Bear is
        # already covered by the shipped feral-bear-pve.
        "feral-cat-pve", "druid", ROLE_MELEE_DPS,
        {
            "Feral Combat": {
                "Ferocity": 5,
                "Feral Aggression": 5,
                "Thick Hide": 1,
                "Sharpened Claws": 3,
                "Feline Swiftness": 2,
                "Improved Shred": 2,
                "Predatory Strikes": 3,
                "Blood Frenzy": 2,
                "Savage Fury": 2,
                "Faerie Fire (Feral)": 1,
                "Heart of the Wild": 5,
                "Leader of the Pack": 1,
            },
            # Restoration is spent before Balance so that Furor arrives as early as the build
            # can afford it. Balance is last because its first five points are dead weight.
            "Restoration": {
                "Furor": 5,
            },
            "Balance": {
                # Five dead points to reach row 1. Every option on the first Balance row is
                # useless to a cat, so this is the cheapest way through rather than a choice.
                "Nature's Grasp": 1,
                "Improved Nature's Grasp": 4,
                "Natural Weapons": 5,
                "Natural Shapeshifter": 3,
                "Omen of Clarity": 1,
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
    # Replace rather than insert. These entries have shipped once already, so a migration that
    # only inserted would collide, and one that only updated would leave the old spell rows
    # behind and produce a build that is the union of two specs.
    last_entry = FIRST_ENTRY + len(SPECS) - 1
    lines = [
        "DELETE FROM `player_premade_spell` WHERE `entry` BETWEEN %u AND %u;"
        % (FIRST_ENTRY, last_entry),
        "DELETE FROM `player_premade_spell_template` WHERE `entry` BETWEEN %u AND %u;"
        % (FIRST_ENTRY, last_entry),
        "",
    ]
    for offset, (spec_name, class_name, role, trees) in enumerate(SPECS):
        entry = FIRST_ENTRY + offset
        spend = resolve(class_name, trees, names_by_spell)

        problems = talent_dbc.validate_build(class_name, spend, level=LEVEL)

        # Ordering is what makes a template worth anything below the level it was authored
        # for, and every prefix of that order has to be a legal build in its own right.
        ordered = talent_dbc.spend_order(class_name, spend, list(trees.keys()))
        problems.extend(talent_dbc.validate_spend_order(class_name, ordered))

        if problems:
            failed = True
            print("%s (%s) is not a legal build:" % (spec_name, class_name), file=sys.stderr)
            for problem in problems:
                print("    %s" % problem, file=sys.stderr)
            continue

        spells = talent_dbc.spend_order_spell_ids(ordered)
        spent = sum(spend.values())
        summary = ", ".join("%s %u" % (tree, sum(ranks.values()))
                            for tree, ranks in trees.items())

        lines.append("-- %s: %s, %u points spent in order (%s)"
                     % (spec_name, class_name, spent, summary))
        lines.append("INSERT INTO `player_premade_spell_template`"
                     " (`entry`, `class`, `level`, `role`, `name`) VALUES (%u, %u, %u, %u, '%s');"
                     % (entry, talent_dbc.CLASS_IDS[class_name], LEVEL, role,
                        sql_escape(spec_name)))
        # One row per talent point, numbered from 1, so applying the first N rows is what a
        # character holding N points gets.
        values = ", ".join("(%u, %u, %u)" % (entry, spell, position)
                           for position, spell in enumerate(spells, start=1))
        lines.append("INSERT INTO `player_premade_spell` (`entry`, `spell`, `spend_order`)"
                     " VALUES %s;" % values)
        lines.append("")

    if failed:
        return 1

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
