#!/usr/bin/env python3
"""Regenerate commands.json, the console page's command reference.

The command tree is parsed out of src/game/Chat/Chat.cpp, which is the only complete
statement of what the core accepts: name, required security level and whether the
handler tolerates having no player session. Usage and descriptions are not in the
source at all -- the Help field there is empty for every command, because the core
fills it from the world `command` table, which carries 25 rows on this realm -- so they
come from the overlay in command_help.py.

Run from anywhere:  python3 contrib/admin/gen_commands.py
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CHAT_CPP = os.path.normpath(os.path.join(HERE, "..", "..", "src", "game", "Chat", "Chat.cpp"))
OUT = os.path.join(HERE, "commands.json")

sys.path.insert(0, HERE)
import command_help  # noqa: E402

SEC_NAMES = {
    "SEC_PLAYER": 0, "SEC_MODERATOR": 1, "SEC_TICKETMASTER": 2, "SEC_GAMEMASTER": 3,
    "SEC_BASIC_ADMIN": 4, "SEC_DEVELOPER": 5, "SEC_ADMINISTRATOR": 6, "SEC_CONSOLE": 7,
}

TABLE_RE = re.compile(r"static ChatCommand (\w+)\[\]\s*=\s*\{(.*?)\n    \};", re.S)
ROW_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*(\w+)\s*,\s*(true|false)\s*,'
    r'\s*(?:&ChatHandler::(\w+)|nullptr)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\w+)\s*\}')


def parse_tables(src):
    return {name: ROW_RE.findall(body) for name, body in TABLE_RE.findall(src)}


def walk(tables, table_name, prefix, out, seen):
    """Flatten the tree into leaves.

    A row with a child table is a group, not a command, unless the child table has an
    empty-named row -- that is the core's way of spelling "the bare command does
    something too", and it is reached by typing the parent on its own.
    """
    for name, sec, console, handler, inline_help, child in tables.get(table_name, []):
        full = (prefix + " " + name).strip()
        if handler and full not in seen:
            seen.add(full)
            entry = {
                "name": full,
                "group": full.split()[0],
                "security": SEC_NAMES.get(sec, 0),
                "console": console == "true",
            }
            if inline_help:
                entry["desc"] = inline_help
            out.append(entry)
        if child != "nullptr":
            walk(tables, child, full, out, seen)


def main():
    with open(CHAT_CPP, encoding="utf-8", errors="replace") as f:
        tables = parse_tables(f.read())
    if "commandTable" not in tables:
        raise SystemExit("could not find commandTable in %s" % CHAT_CPP)

    leaves = []
    walk(tables, "commandTable", "", leaves, set())
    leaves.sort(key=lambda c: c["name"])

    for c in leaves:
        note = command_help.NOTES.get(c["name"])
        if note:
            c["args"], c["desc"] = note

    unknown = set(command_help.NOTES) - {c["name"] for c in leaves}
    if unknown:
        raise SystemExit("command_help.NOTES has entries no longer in the core: %s"
                         % ", ".join(sorted(unknown)))

    payload = {
        "groups": {g: command_help.GROUPS.get(g, "") for g in
                   sorted({c["group"] for c in leaves})},
        "commands": leaves,
    }
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=1, sort_keys=False)
        f.write("\n")

    documented = sum(1 for c in leaves if "desc" in c)
    print("%s: %d commands in %d groups, %d documented"
          % (os.path.basename(OUT), len(leaves), len(payload["groups"]), documented))


if __name__ == "__main__":
    main()
