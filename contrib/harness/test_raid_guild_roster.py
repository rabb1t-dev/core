#!/usr/bin/env python3
"""Check that an authored roster turns into real characters that can be logged in.

The roster is the half of the raid guild that has to survive a restart, so what matters
here is not that the commands return success but that a `characters` row exists afterwards
and that the bot login path accepts it. Those are different claims: a roster member is
provisioned with an account that has no `realmd` row at all, which works only because
nothing in the bot login path consults one.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 test_raid_guild_roster.py
"""

import re
import sys
import time

from vmangos_harness import CommandError, Harness

# Distinct from anything a real roster would be called, since the cleanup at either end of
# this test erases the characters it names.
MEMBERS = [
    # name, race, class, role, and what the pair is, for the failure messages.
    ("Rgtestone", 2, 1, "tank", "orc warrior"),
    ("Rgtesttwo", 5, 5, "healer", "undead priest"),
    ("Rgtestthree", 2, 3, "rangedps", "orc hunter"),
]


def parse_list(harness):
    """`.raidguild list` as {name: {...}}, plus the summary line."""
    text = harness.run("raidguild list")

    members = {}
    summary = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("member "):
            fields = dict(re.findall(r"(\w+)=(\S+)", line))
            members[fields["name"]] = fields
        elif line.startswith("roster "):
            summary = dict(re.findall(r"(\w+)=(\S+)", line))

    return members, summary


def cleanup(harness):
    """Leave nothing behind, in either the roster or the character table.

    The wait matters. Erasing a character resolves its name through the player cache, so
    erasing one that is still on its way out of the world can miss and leave the row
    behind, and the next run then provisions a second character under the same name.
    """
    for name, _, _, _, _ in MEMBERS:
        harness.run(f"raidguild remove {name}", allow_failure=True)
        harness.logout(name)

    for name, _, _, _, _ in MEMBERS:
        deadline = time.time() + 30.0
        while time.time() < deadline and harness.info(name) is not None:
            time.sleep(1.0)

        harness.run(f"character erase {name}", allow_failure=True)


def main():
    harness = Harness.from_env()
    failures = []

    cleanup(harness)
    harness.run("raidguild reload")

    for name, race, class_id, role, _ in MEMBERS:
        harness.run(f"raidguild add {name} {race} {class_id} 0 {role}")

    # A bad race and class pair has to be refused when the row is authored rather than
    # discovered at provisioning time, when there is a half built roster to unpick.
    try:
        harness.run("raidguild add Rgtestbad 5 2 0 tank")  # undead paladin
        failures.append("an undead paladin was accepted onto the roster")
        harness.run("raidguild remove Rgtestbad", allow_failure=True)
    except CommandError:
        pass

    members, summary = parse_list(harness)
    for name, _, _, _, description in MEMBERS:
        if name not in members:
            failures.append(f"{name} ({description}) is missing from the roster")
        elif members[name]["guid"] != "0":
            failures.append(f"{name} claims character {members[name]['guid']} before provisioning")

    if summary.get("provisioned") != "0":
        failures.append(f"roster reports {summary.get('provisioned')} provisioned before provisioning")

    harness.run("raidguild provision")

    members, summary = parse_list(harness)
    accounts = {}
    for name, race, class_id, _, description in MEMBERS:
        fields = members.get(name)
        if not fields:
            failures.append(f"{name} vanished from the roster during provisioning")
            continue

        if fields["guid"] == "0":
            failures.append(f"{name} ({description}) was not provisioned")
            continue

        if fields["account"] == "0":
            failures.append(f"{name} was provisioned without an account")

        accounts.setdefault(fields["account"], []).append(name)

    # Two members sharing an account is not a cosmetic problem. PlayerBotMgr refuses a bot
    # whose account already has a session, so the second one to spawn would simply never
    # arrive, and the roster would settle one member short with nothing in the log.
    for account, names in accounts.items():
        if len(names) > 1:
            failures.append(f"account {account} is shared by {', '.join(names)}")

    # Provisioning twice must be a no-op rather than a second set of characters, since it
    # is the normal way to bring a roster up to date after adding to it.
    before = {name: fields["guid"] for name, fields in members.items()}
    harness.run("raidguild provision")
    members, _ = parse_list(harness)
    for name, guid in before.items():
        if members.get(name, {}).get("guid") != guid:
            failures.append(f"{name} was reprovisioned as a different character")

    # Taking a member off the roster and putting it back must find the character it already
    # has rather than build a second one under the same name. This is the state a rebuilt
    # roster table arrives in, and getting it wrong is quiet: two characters answering to
    # one name, with everything that addresses a player by name reaching either of them.
    adopt_name, adopt_race, adopt_class, adopt_role, _ = MEMBERS[0]
    adopt_guid = members.get(adopt_name, {}).get("guid")
    harness.run(f"raidguild remove {adopt_name}")
    harness.run(f"raidguild add {adopt_name} {adopt_race} {adopt_class} 0 {adopt_role}")
    harness.run(f"raidguild provision {adopt_name}")

    members, summary = parse_list(harness)
    readopted = members.get(adopt_name, {}).get("guid")
    if readopted != adopt_guid:
        failures.append(
            f"re-adding {adopt_name} made character {readopted} instead of adopting {adopt_guid}")

    # The claim the rest of the roster work rests on: these are real characters, and the
    # bot login path takes them despite their accounts existing nowhere else.
    for name, race, class_id, _, description in MEMBERS:
        if members.get(name, {}).get("guid", "0") == "0":
            continue

        try:
            harness.login(name, timeout=45)
        except CommandError as exc:
            failures.append(f"{name} ({description}) would not log in: {exc}")
            continue

        info = harness.info(name)
        if not info:
            failures.append(f"{name} logged in but never reached the world")

    print("roster " + " ".join(f"{k}={v}" for k, v in sorted(summary.items())))
    for name, fields in sorted(members.items()):
        print(f"member name={name} guid={fields['guid']} account={fields['account']} role={fields['role']}")

    cleanup(harness)

    for failure in failures:
        print(f"FAIL: {failure}")
    print("PASS" if not failures else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
