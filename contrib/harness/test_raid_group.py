#!/usr/bin/env python3
"""Check that a party bot roster can grow past five and stops at the raid ceiling.

A normal party holds five, so this exercises the promotion to a raid group and the
40 member cap.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a
GM account: python3 test_raid_group.py
"""

import sys
import time

from vmangos_harness import CommandError, Harness

LEADER = "Harnessbot"
TARGET_MEMBERS = 40  # one leader plus 39 bots


def wait_for_members(harness, expected, timeout=120.0):
    """Bots spawn asynchronously, so poll until the count settles."""
    deadline = time.time() + timeout
    last = -1
    while time.time() < deadline:
        info = harness.info(LEADER)
        count = int(info["members"]) if info else 0
        if count == expected:
            return info
        last = count
        time.sleep(2.0)
    raise AssertionError(f"expected {expected} members, stalled at {last}")


def main():
    harness = Harness.from_env()
    failures = []

    harness.login(LEADER)
    harness.execute(LEADER, f"character level {LEADER} 60")

    # Start from a known state.
    harness.run(f"harness exec {LEADER} partybot remove", allow_failure=True)
    time.sleep(3.0)

    for i in range(TARGET_MEMBERS - 1):
        harness.execute(LEADER, "partybot add dps")
        # Spawning is asynchronous; a short gap keeps the login queue from bunching up.
        time.sleep(0.75)

    info = wait_for_members(harness, TARGET_MEMBERS)

    if info["raid"] != "1":
        failures.append("group did not convert to a raid")

    subgroups = {m["subgroup"] for m in info["members_detail"]}
    if len(subgroups) < 8:
        failures.append(f"expected members spread over 8 subgroups, saw {sorted(subgroups)}")

    # The ceiling should be a clean refusal rather than a silent no-op.
    try:
        text = harness.execute(LEADER, "partybot add dps")
        if "Group is full" not in text:
            failures.append(f"41st member was not refused: {text!r}")
    except CommandError as exc:
        if "Group is full" not in str(exc):
            failures.append(f"unexpected error at the cap: {exc}")

    after = harness.info(LEADER)
    if after["members"] != str(TARGET_MEMBERS):
        failures.append(f"roster changed after the refusal: {after['members']}")

    print(f"members={info['members']} raid={info['raid']} subgroups={sorted(subgroups)}")
    for failure in failures:
        print(f"FAIL: {failure}")
    print("PASS" if not failures else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
