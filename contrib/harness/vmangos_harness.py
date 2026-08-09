#!/usr/bin/env python3
"""Drive a running mangosd over SOAP so tests can play without a game client.

Runs on the server host, since SOAP binds to loopback only.
"""

import argparse
import base64
import html
import os
import re
import sys
import time
import urllib.error
import urllib.request
from xml.sax.saxutils import escape

DEFAULT_URL = os.environ.get("VMANGOS_SOAP_URL", "http://127.0.0.1:7878/")

HORDE = 67
ALLIANCE = 469

ENVELOPE = (
    '<?xml version="1.0" encoding="utf-8"?>'
    '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/"'
    ' xmlns:ns1="urn:MaNGOS">'
    "<SOAP-ENV:Body><ns1:executeCommand><command>{command}</command>"
    "</ns1:executeCommand></SOAP-ENV:Body></SOAP-ENV:Envelope>"
)

# The server reports success even when it refuses to run a command, so the text has
# to be inspected as well. See the console-forbidden path in ChatHandler::ExecuteCommand.
FAILURE_MARKERS = (
    "There is no such command",
    "There is no such subcommand",
    "Command not available",
    "Harness: ",
)


class CommandError(RuntimeError):
    pass


class Harness:
    def __init__(self, user, password, url=DEFAULT_URL, timeout=30.0):
        self.url = url
        self.timeout = timeout
        token = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.headers = {
            "Content-Type": "text/xml; charset=utf-8",
            "Authorization": f"Basic {token}",
            "SOAPAction": "urn:MaNGOS#executeCommand",
        }

    @classmethod
    def from_env(cls, url=None, timeout=30.0):
        user = os.environ.get("VMANGOS_SOAP_USER")
        password = os.environ.get("VMANGOS_SOAP_PASSWORD")
        if not user or not password:
            raise CommandError(
                "Set VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD to a GM account."
            )
        return cls(user, password, url or DEFAULT_URL, timeout)

    def raw(self, command):
        """Run a command, returning (ok, output). A refused command still returns text."""
        body = ENVELOPE.format(command=escape(command)).encode()
        request = urllib.request.Request(self.url, data=body, headers=self.headers)
        ok = True
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                payload = response.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            payload = exc.read().decode("utf-8", "replace")
            ok = False

        match = re.search(r"<result>(.*?)</result>", payload, re.S)
        if not match:
            match = re.search(r"<faultstring>(.*?)</faultstring>", payload, re.S)
        text = html.unescape(match.group(1)).replace("\r", "").strip() if match else ""
        return ok, text

    def run(self, command, allow_failure=False):
        ok, text = self.raw(command)
        if not allow_failure:
            if not ok:
                raise CommandError(f"{command!r} failed: {text}")
            # The server also reports success when it refuses to run a command.
            for marker in FAILURE_MARKERS:
                if marker in text:
                    raise CommandError(f"{command!r} rejected: {text}")
        return text

    # -- character helpers -------------------------------------------------

    def info(self, character):
        """Parsed `.harness info` output, or None when the character is offline."""
        text = self.run(f"harness info {character}", allow_failure=True)
        if "is not in the world" in text:
            return None

        fields = {}
        members = []
        items = []
        # Repeated lines are collected rather than merged, since each one describes a different
        # thing and folding them into the same dict leaves only whichever came last.
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            pairs = dict(re.findall(r"(\w+)=(\S+)", line))

            # The last value on a line runs to the end of it, spaces and all. Names are put
            # last for exactly that reason, so that nothing has to be quoted, and the pair
            # pattern above would otherwise keep only the first word of one.
            trailing = re.search(r"(\w+)=([^=]*)$", line)
            if trailing:
                pairs[trailing.group(1)] = trailing.group(2)

            if line.startswith("member "):
                members.append(pairs)
            elif line.startswith("item "):
                items.append(pairs)
            else:
                fields.update(pairs)
        fields["members_detail"] = members
        fields["items"] = items
        return fields

    def login(self, character, timeout=60.0):
        """Log a character in headlessly and wait for it to reach the world."""
        if self.info(character) is not None:
            return

        self.run(f"harness login {character}")

        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.info(character) is not None:
                return
            time.sleep(1.0)

        raise CommandError(f"{character} did not reach the world within {timeout:.0f}s")

    def logout(self, character):
        self.run(f"bot delete {character}", allow_failure=True)

    def execute(self, character, command):
        """Run a command as the given in-world character."""
        return self.run(f"harness exec {character} {command}")

    def teleport(self, character, x, y, z, map_id):
        return self.execute(character, f"go xyz {x:.2f} {y:.2f} {z:.2f} {map_id}")

    def spells(self, character):
        """A combat bot's named spell slots and planted totems, plus a summary.

        Slots are parsed by position rather than by the usual key=value sweep, because a
        spell name contains spaces and so has to be the rest of the line.

        The totems are what is on the ground now, which is not the same question as the
        totem slots: those hold the choice the bot would make knowing nothing, and the real
        one is made afresh each time a totem is planted.
        """
        text = self.run(f"harness spells {character}")

        summary = {}
        slots = {}
        totems = {}
        for line in text.splitlines():
            line = line.strip()
            match = re.match(
                r"^slot (\w+) id=(\d+) rank=(\d+) level=(\d+) known=(\d+) name=(.*)$", line)
            if match:
                slots[match.group(1)] = {
                    "id": int(match.group(2)),
                    "rank": int(match.group(3)),
                    "level": int(match.group(4)),
                    "known": int(match.group(5)) == 1,
                    "name": match.group(6),
                }
                continue

            match = re.match(r"^totem (\w+) id=(\d+) name=(.*)$", line)
            if match:
                totems[match.group(1)] = {
                    "id": int(match.group(2)),
                    "name": match.group(3),
                }
            elif line.startswith("spells "):
                summary = {k: int(v) if v.isdigit() else v
                           for k, v in re.findall(r"(\w+)=(\S+)", line)}

        return {"summary": summary, "slots": slots, "totems": totems}

    def talents(self, character):
        """A character's talent build: totals, per tree spend, and every learned talent.

        The summary's `illegal` and `spent` versus `available` counts are the point of this.
        Premade specs are applied with LearnSpell rather than LearnTalent, so the server never
        checks tier requirements or the point budget, and a build that could not be made in the
        client applies without complaint. This is the only way to see that from a test.
        """
        text = self.run(f"harness talents {character}")

        summary = {}
        trees = {}
        talents = {}
        illegal = []
        for line in text.splitlines():
            line = line.strip()

            match = re.match(r"^tab id=(\d+) page=(\d+) spent=(\d+) name=(.*)$", line)
            if match:
                trees[match.group(4) or match.group(1)] = int(match.group(3))
                continue

            match = re.match(
                r"^talent id=(\d+) tab=(\d+) row=(\d+) rank=(\d+) max=(\d+) spell=(\d+) name=(.*)$",
                line)
            if match:
                talents[match.group(7)] = {
                    "id": int(match.group(1)),
                    "tab": int(match.group(2)),
                    "row": int(match.group(3)),
                    "rank": int(match.group(4)),
                    "max": int(match.group(5)),
                    "spell": int(match.group(6)),
                }
                continue

            if line.startswith("illegal "):
                illegal.append(dict(re.findall(r"(\w+)=(\S+)", line)))
            elif line.startswith("talents "):
                summary = {k: int(v) if v.isdigit() else v
                           for k, v in re.findall(r"(\w+)=(\S+)", line)}

        return {"summary": summary, "trees": trees, "talents": talents, "illegal": illegal}

    # -- world queries -----------------------------------------------------

    def despawn(self, character, entry, range_yards=500.0):
        """Remove every creature of one entry near a character, returning how many went.

        Summoned test targets outlive the suite that made them, and one still in combat with
        an unkillable harness character never resets. Clearing them is what makes a combat
        test repeatable rather than only correct the first time after a restart.
        """
        text = self.run(f"harness despawn {character} {entry} {range_yards:.0f}")
        match = re.search(r"removed=(\d+)", text)
        return int(match.group(1)) if match else 0

    def threat(self, character):
        """The threat list of whatever this character is fighting, highest first.

        None when the character is not in a fight, or is fighting something that keeps no
        threat list. Percentages are of the current victim's threat, since the rule that
        decides who a boss hits is written as a ratio rather than an amount.
        """
        text = self.run(f"harness threat {character}")
        if "threat none" in text:
            return None

        summary = {}
        hostiles = []
        for line in text.splitlines():
            line = line.strip()

            match = re.match(
                r"^hostile threat=(\S+) percent=(\S+) top=(\d+) melee=(\d+) dist=(\S+) "
                r"name=(.*)$", line)
            if match:
                hostiles.append({
                    "threat": float(match.group(1)),
                    "percent": float(match.group(2)),
                    "top": match.group(3) == "1",
                    # Which flip rule this one is actually subject to, 110 percent when the mob
                    # can swing at it and 130 when it cannot, rather than what its class implies.
                    "melee": match.group(4) == "1",
                    "distance": float(match.group(5)),
                    "name": match.group(6),
                })
                continue

            match = re.match(
                r"^threat entries=(\d+) topthreat=(\S+) combat=(\S+) victim=(.*)$", line)
            if match:
                summary = {
                    "entries": int(match.group(1)),
                    "topthreat": float(match.group(2)),
                    "combat": int(match.group(3)),
                    "victim": match.group(4),
                }

        return {"summary": summary,
                "hostiles": sorted(hostiles, key=lambda h: -h["threat"])}

    def graveyard(self, map_id, x, y, z, team=HORDE):
        """Where a ghost dying at this spot releases to, as (map, x, y, z)."""
        text = self.run(f"harness graveyard {map_id} {x:.2f} {y:.2f} {z:.2f} {team}")
        if "graveyard none" in text:
            return None
        f = dict(re.findall(r"(\w+)=(\S+)", text))
        return (int(f["map"]), float(f["x"]), float(f["y"]), float(f["z"]))

    def path(self, character, x, y, z, trigger=0):
        """What the navigation mesh answers for a route the character would walk.

        Passing an area trigger id also reports whether the route's end lands inside it,
        which is the only meaningful test of arrival at a portal.
        """
        text = self.run(f"harness path {character} {x:.2f} {y:.2f} {z:.2f} {trigger}")
        f = dict(re.findall(r"(\w+)=(\S+)", text))
        return {
            "type": f["type"].split("(")[0],
            "length": float(f["length"]),
            "shortfall": float(f["shortfall"]),
            "reached": tuple(float(v) for v in f["reached"].split(",")),
            "arrived": int(f["arrived"]) == 1,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--user", default=os.environ.get("VMANGOS_SOAP_USER"))
    parser.add_argument("--password", default=os.environ.get("VMANGOS_SOAP_PASSWORD"))
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--as-character", dest="character")
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()

    if not args.user or not args.password:
        parser.error(
            "pass --user and --password, or set VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD"
        )

    harness = Harness(args.user, args.password, args.url)
    command = " ".join(args.command)

    try:
        if args.character:
            harness.login(args.character)
            print(harness.execute(args.character, command))
        else:
            print(harness.run(command))
    except CommandError as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
