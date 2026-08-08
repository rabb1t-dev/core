#!/usr/bin/env python3
"""Check that a provisioned roster can be brought into the world as a raid and sent home.

Summoning is the half of the roster work where the persistence claim is actually made. A
generated `.partybot add` bot has m_saveDisabled set for its whole life and cannot keep
anything; a roster member is spawned through the database load path instead, so what it
carries at dismissal is what it will have next time. The test therefore cares less about
the commands returning success than about three things that are easy to get wrong: that
the group is a raid before anyone is placed in it, that members end up in the subgroups the
roster asks for, and that a dismissed member really leaves rather than merely being told to.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_raid_guild_summon.py
"""

import re
import sys
import time

from vmangos_harness import CommandError, Harness

# Seven, which is the smallest roster that says anything about raids: a party holds five,
# so a group this size has to have been promoted, and there is a second subgroup to be
# wrong about. The first is the leader, and stands in for the human player a real raid
# would form around.
MEMBERS = [
    # name, race, class, role, subgroup, and what the pair is, for the failure messages.
    ("Rgsumlead", 2, 1, "tank", 1, "orc warrior"),
    ("Rgsumtwo", 2, 1, "tank", 1, "orc warrior"),
    ("Rgsumthree", 5, 5, "healer", 1, "undead priest"),
    ("Rgsumfour", 5, 5, "healer", 2, "undead priest"),
    ("Rgsumfive", 2, 3, "rangedps", 2, "orc hunter"),
    ("Rgsumsix", 2, 4, "meleedps", 2, "orc rogue"),
    ("Rgsumseven", 5, 8, "rangedps", 2, "undead mage"),
]

LEADER = MEMBERS[0][0]
GUILD = "Rgsumguild"


def parse_status(harness):
    """`.raidguild status` as ({name: {...}}, summary)."""
    text = harness.run("raidguild status")

    summoned = {}
    summary = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("summoned "):
            fields = dict(re.findall(r"(\w+)=(\S+)", line))
            summoned[fields["name"]] = fields
        elif line.startswith("status "):
            summary = dict(re.findall(r"(\w+)=(\S+)", line))

    return summoned, summary


def wait_for(harness, predicate, timeout=90.0):
    """Poll the status until it satisfies the predicate, returning the last one seen.

    Nothing here happens on the tick the command returns. A summoned member takes a
    session load and then a couple of seconds of bot update before it joins the group,
    and a dismissed one takes a logout, so every assertion about the world has to be
    made against a settled state rather than the one right after the command.
    """
    deadline = time.time() + timeout
    while True:
        summoned, summary = parse_status(harness)
        if predicate(summoned, summary) or time.time() >= deadline:
            return summoned, summary
        time.sleep(2.0)


def cleanup(harness):
    """Leave nothing behind, in either the roster or the character table.

    The wait matters. Erasing a character resolves its name through the player cache, so
    erasing one that is still on its way out of the world can miss and leave the row
    behind, and the next run then provisions a second character under the same name.
    """
    # Disbanded first, and before the characters go. A guild outliving the roster would
    # make the next run's "added" count meaningless, since the members would already be in.
    harness.run(f'guild delete "{GUILD}"', allow_failure=True)

    for name, *_ in MEMBERS:
        harness.run(f"raidguild remove {name}", allow_failure=True)
        harness.logout(name)

    for name, *_ in MEMBERS:
        deadline = time.time() + 30.0
        while time.time() < deadline and harness.info(name) is not None:
            time.sleep(1.0)

        harness.run(f"character erase {name}", allow_failure=True)


def main():
    harness = Harness.from_env()
    failures = []

    cleanup(harness)
    harness.run("raidguild reload")

    for name, race, class_id, role, subgroup, _ in MEMBERS:
        harness.run(f"raidguild add {name} {race} {class_id} 0 {role} {subgroup}")

    harness.run("raidguild provision")

    # The leader arrives through the plain login path rather than as a summoned member,
    # which is what a human player is to a real raid: the thing the group is formed on.
    harness.login(LEADER, timeout=60)

    # Guilding happens with only the founder in the world, which is the whole point of
    # doing it through the guild tables: a roster of forty should not have to be summoned
    # to be guilded. Whether the other six really got in is not settled here, only claimed;
    # it is checked against the characters themselves once they are summoned below.
    guild = dict(re.findall(r"(\w+)=(\S+)", harness.run(f"raidguild guild {GUILD} {LEADER}")))

    # Everyone but the founder, who joined by founding it. What matters is the total.
    if guild.get("added") != str(len(MEMBERS) - 1):
        failures.append(f"the guild took {guild.get('added')} of {len(MEMBERS) - 1} offline members")
    if guild.get("failed") != "0":
        failures.append(f"{guild.get('failed')} members were refused by the guild")
    if guild.get("members") != str(len(MEMBERS)):
        failures.append(f"the new guild holds {guild.get('members')} members, not {len(MEMBERS)}")

    # Running it again must add nobody rather than fail or duplicate, since this is how a
    # guild is brought up to date after the roster grows.
    again = dict(re.findall(r"(\w+)=(\S+)", harness.run(f"raidguild guild {GUILD}")))
    if again.get("added") != "0":
        failures.append(f"re-forming the guild added {again.get('added')} members again")
    if again.get("members") != str(len(MEMBERS)):
        failures.append(f"the guild holds {again.get('members')} members, not {len(MEMBERS)}")

    harness.run(f"raidguild summon {LEADER}")

    expected = len(MEMBERS)
    summoned, summary = wait_for(
        harness, lambda s, summary: summary.get("grouped") == str(expected))

    if summary.get("online") != str(expected):
        failures.append(f"{summary.get('online')} of {expected} members reached the world")
    if summary.get("grouped") != str(expected):
        failures.append(f"{summary.get('grouped')} of {expected} members joined the group")

    # A party would have held five of them and then started refusing people, so this is
    # also the check that the group was promoted before it filled rather than after.
    for name, *_, description in MEMBERS:
        fields = summoned.get(name)
        if not fields:
            failures.append(f"{name} ({description}) is not in the world")
            continue

        if fields.get("raid") != "1":
            failures.append(f"{name} is in a party rather than a raid")

        # Read off the character, not the guild tables. A guild_member row that the
        # character never picks up on login would be a guild on paper only.
        if fields.get("guild", "0") == "0":
            failures.append(f"{name} came into the world with no guild")

    # Placement is reconciled on a timer rather than commanded at summon time, because a
    # member is not in the group yet when it is summoned. So it is worth waiting for
    # separately: getting the raid formed and getting the seating right are two events.
    def seated(summoned, _summary):
        return all(summoned.get(name, {}).get("subgroup") == str(subgroup)
                   for name, _, _, _, subgroup, _ in MEMBERS)

    summoned, summary = wait_for(harness, seated, timeout=60.0)
    for name, _, _, _, subgroup, _ in MEMBERS:
        placed = summoned.get(name, {}).get("subgroup")
        if placed != str(subgroup):
            failures.append(f"{name} is in subgroup {placed}, not the rostered {subgroup}")

    for name, fields in sorted(summoned.items()):
        print(f"summoned name={name} raid={fields.get('raid')} subgroup={fields.get('subgroup')}")

    # The claim the whole design rests on, made falsifiable. Player::Create sets
    # m_saveDisabled and nothing clears it, so a generated bot changed in the world is
    # unchanged the moment it leaves; a member spawned through the load path should not
    # be. A level is used because it is the cheapest thing to set that .harness info
    # already reports back.
    witness = MEMBERS[1][0]
    harness.run(f"character level {witness} 20")
    if (harness.info(witness) or {}).get("level") != "20":
        failures.append(f"{witness} would not take a level change while in the world")

    harness.run("raidguild dismiss")

    # Dismissal is a logout, and a logout is what saves the character, so a member that
    # has not actually gone has not actually been saved. Reporting the command as done
    # before that would make every later claim about persistence unfalsifiable.
    summoned, summary = wait_for(harness, lambda s, summary: summary.get("online") == "0")
    if summary.get("online") != "0":
        failures.append(f"{summary.get('online')} members were still in the world after dismissal")

    # And the characters survive it. A member that dismissal deleted, or that came back as
    # a fresh character, would pass everything above and still be useless for a roster
    # whose entire point is gearing up across sessions.
    harness.login(LEADER, timeout=60)
    harness.run(f"raidguild summon {LEADER}")
    summoned, summary = wait_for(
        harness, lambda s, summary: summary.get("grouped") == str(expected))
    if summary.get("online") != str(expected):
        failures.append(
            f"only {summary.get('online')} of {expected} members could be summoned a second time")

    level = (harness.info(witness) or {}).get("level")
    if level != "20":
        failures.append(f"{witness} came back at level {level}, so the session was not saved")

    print("status " + " ".join(f"{k}={v}" for k, v in sorted(summary.items())) + f" witnesslevel={level}")

    harness.run("raidguild dismiss")
    wait_for(harness, lambda s, summary: summary.get("online") == "0")
    cleanup(harness)

    for failure in failures:
        print(f"FAIL: {failure}")
    print("PASS" if not failures else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
