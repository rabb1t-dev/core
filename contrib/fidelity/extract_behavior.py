#!/usr/bin/env python3
"""Extract a creature's complete behavioural model from the world database.

Why this exists
---------------
A boss is not a spell list. What makes a fight *the* fight is sequencing:
phase thresholds, add waves, targeting rules, transitions, enrage. In vmangos
that behaviour is spread across four places, and an audit that reads fewer
than all four will report a mechanic as missing when it is merely somewhere
else.

Jammal'an the Prophet is the worked example. His signature Hex of Jammal'an
and both of his Healing Wave rules live in `creature_ai_events`, so an audit
that reads `creature_spells` alone concludes his namesake ability was never
implemented. It was; it is just in the other table.

    creature_template          stats, rank, immunity masks, which sources apply
    creature_spells            timed rotation -- NOTE: timers stored in SECONDS
    creature_ai_events         phases, HP thresholds, summons, aggro/death
      + creature_ai_scripts    the actions those events fire
    src/scripts/**/boss_*.cpp  arbitrary logic, NOT visible from here

The last one is the hole, and it is the dangerous one, because a creature
driven by C++ looks *empty* from the database rather than looking unknown.
When `script_name` is set this tool says so loudly instead of letting an
empty rotation read as a finding.

Usage
-----
    extract_behavior.py --map 109              # every rank>0 creature on a map
    extract_behavior.py --entry 5710 5709      # specific creatures
    extract_behavior.py --map 409 --json       # machine-readable, for the diff

Runs on the host that owns the world database (the Pi), because credentials
are parsed out of mangosd.conf and never leave it.
"""

import argparse
import json
import os
import re
import subprocess
import sys

CONF_DEFAULT = os.path.expanduser("~/server/etc/mangosd.conf")

# ---------------------------------------------------------------------------
# Enums, transcribed from source rather than from memory. Keep these in sync
# with the headers named beside them; a wrong decode here is invisible and
# produces a confidently wrong audit.
# ---------------------------------------------------------------------------

# src/game/AI/CreatureEventAI.h :: enum EventType
EVENT_TYPES = {
    0:  ("TIMER_IN_COMBAT",       ("InitialMin", "InitialMax", "RepeatMin", "RepeatMax")),
    1:  ("TIMER_OOC",             ("InitialMin", "InitialMax", "RepeatMin", "RepeatMax")),
    2:  ("HP",                    ("HPMax%", "HPMin%", "RepeatMin", "RepeatMax")),
    3:  ("MANA",                  ("ManaMax%", "ManaMin%", "RepeatMin", "RepeatMax")),
    4:  ("AGGRO",                 ()),
    5:  ("KILL",                  ("RepeatMin", "RepeatMax", "PlayerOnly")),
    6:  ("DEATH",                 ()),
    7:  ("EVADE",                 ()),
    8:  ("HIT_BY_SPELL",          ("SpellID", "School", "RepeatMin", "RepeatMax")),
    9:  ("RANGE",                 ("MinDist", "MaxDist", "RepeatMin", "RepeatMax")),
    10: ("OOC_LOS",               ("Reaction", "MaxRange", "RepeatMin", "RepeatMax")),
    11: ("SPAWNED",               ()),
    12: ("TARGET_HP",             ("HPMax%", "HPMin%", "RepeatMin", "RepeatMax")),
    13: ("TARGET_CASTING",        ("RepeatMin", "RepeatMax")),
    14: ("FRIENDLY_HP",           ("HPDeficit", "Radius", "RepeatMin", "RepeatMax")),
    15: ("FRIENDLY_IS_CC",        ("DispelType", "Radius", "RepeatMin", "RepeatMax")),
    16: ("FRIENDLY_MISSING_BUFF", ("SpellId", "Radius", "RepeatMin", "RepeatMax")),
    17: ("SUMMONED_UNIT",         ("CreatureId", "RepeatMin", "RepeatMax")),
    18: ("TARGET_MANA",           ("ManaMax%", "ManaMin%", "RepeatMin", "RepeatMax")),
    19: ("QUEST_ACCEPT",          ("QuestID",)),
    20: ("QUEST_COMPLETE",        ()),
    21: ("REACHED_HOME",          ()),
    22: ("RECEIVE_EMOTE",         ("EmoteId", "Condition", "CondValue1", "CondValue2")),
    23: ("AURA",                  ("SpellID", "Stacks", "RepeatMin", "RepeatMax")),
    24: ("TARGET_AURA",           ("SpellID", "Stacks", "RepeatMin", "RepeatMax")),
    25: ("SUMMONED_JUST_DIED",    ("CreatureId", "RepeatMin", "RepeatMax")),
    26: ("SUMMONED_JUST_DESPAWN", ("CreatureId", "RepeatMin", "RepeatMax")),
    27: ("MISSING_AURA",          ("SpellID", "Stacks", "RepeatMin", "RepeatMax")),
    28: ("TARGET_MISSING_AURA",   ("SpellID", "Stacks", "RepeatMin", "RepeatMax")),
    29: ("MOVEMENT_INFORM",       ("MotionType", "PointId", "RepeatMin", "RepeatMax")),
    30: ("LEAVE_COMBAT",          ()),
    31: ("SCRIPT",                ("EventID", "Data")),
    32: ("GROUP_MEMBER_DIED",     ("CreatureId", "IsLeader")),
    33: ("VICTIM_ROOTED",         ("RepeatMin", "RepeatMax")),
    34: ("HIT_BY_AURA",           ("AuraType", "-", "RepeatMin", "RepeatMax")),
    35: ("STEALTH_ALERT",         ("RepeatMin", "RepeatMax")),
    36: ("SPELL_HIT_TARGET",      ("SpellID", "School", "RepeatMin", "RepeatMax")),
}

# src/game/AI/CreatureEventAI.h :: enum EventFlags
EVENT_FLAGS = {
    0x01: "repeatable",
    0x02: "random-action",
    0x04: "not-while-casting",
    0x08: "check-result",
    0x10: "debug-only",
}

# src/game/Maps/ScriptCommands.h :: enum ScriptCommands (only what bosses use;
# anything else is rendered as its raw number so it cannot be silently dropped)
SCRIPT_COMMANDS = {
    0: "talk", 1: "emote", 2: "field_set", 3: "move_to", 4: "modify_flags",
    5: "interrupt_casts", 6: "teleport_to", 10: "summon_creature",
    11: "open_door", 12: "close_door", 14: "remove_aura", 15: "cast",
    16: "play_sound", 18: "despawn", 19: "set_equipment", 20: "movement",
    22: "set_faction", 23: "morph", 24: "mount", 25: "set_run",
    26: "attack_start", 27: "update_entry", 28: "stand_state",
    29: "modify_threat", 31: "terminate_script", 33: "evade",
    34: "set_home", 35: "turn_to", 37: "set_inst_data", 38: "set_inst_data64",
    39: "start_script", 42: "set_melee_attack", 43: "set_combat_movement",
    44: "SET_PHASE", 45: "set_phase_random", 46: "set_phase_range",
    47: "flee", 48: "deal_damage", 49: "zone_combat_pulse",
    50: "call_for_help", 52: "invincibility", 55: "swap_spell_list",
    56: "remove_guardians", 59: "set_react_state", 60: "start_waypoints",
    68: "start_script_for_all", 71: "respawn_creature", 73: "combat_stop",
    74: "add_aura", 75: "add_threat", 85: "send_script_event",
    88: "set_command_state", 90: "start_script_on_group",
}

# src/game/Maps/ScriptCommands.h :: enum ScriptTarget. Shared by the
# `target_type` of creature_ai_scripts AND the `castTarget` of creature_spells
# (ObjectMgr validates both through ScriptMgr::CheckScriptTargets).
TARGETS = {
    0: "provided/self", 1: "victim", 2: "2nd-aggro", 3: "last-aggro",
    4: "random-hostile", 5: "random-not-tank", 6: "nearest-hostile",
    7: "farthest-hostile", 8: "owner-or-self", 9: "owner",
    10: "nearest-creature-entry", 11: "creature-guid",
    12: "creature-from-inst-data", 13: "nearest-go-entry", 14: "go-guid",
    15: "go-from-inst-data", 16: "random-friendly", 17: "most-injured-friendly",
    18: "most-injured-friendly-except", 19: "friendly-missing-buff",
    20: "friendly-missing-buff-except", 21: "friendly-cc",
    22: "map-event-source", 23: "map-event-target", 24: "map-event-extra",
    25: "nearest-player", 26: "nearest-hostile-player",
    27: "nearest-friendly-player", 28: "random-creature-entry",
    29: "random-go-entry",
}

# src/game/Maps/ScriptCommands.h :: enum CastFlags
CAST_FLAGS = {
    0x001: "interrupt-previous", 0x002: "triggered", 0x004: "force",
    0x008: "main-ranged", 0x010: "target-unreachable", 0x020: "aura-not-present",
    0x040: "only-in-melee", 0x080: "not-in-melee", 0x100: "target-casting",
}


def decode_mask(mask, table):
    if not mask:
        return []
    return [name for bit, name in sorted(table.items()) if mask & bit]


# ---------------------------------------------------------------------------
# Database access. Credentials are parsed out of mangosd.conf and passed to
# mysql through MYSQL_PWD so they never appear in a process list or a
# transcript. Shelling out to the mysql client avoids requiring a Python
# driver on the server host.
# ---------------------------------------------------------------------------

class World:
    def __init__(self, conf=CONF_DEFAULT):
        info = None
        with open(conf) as fh:
            for line in fh:
                if line.startswith("WorldDatabase.Info"):
                    m = re.search(r'"(.*)"', line)
                    if m:
                        info = m.group(1)
                    break
        if not info:
            raise SystemExit(f"no WorldDatabase.Info in {conf}")
        self.host, self.port, self.user, self._pw, self.db = info.split(";")

    def query(self, sql, columns=None):
        """Return a list of row-tuples. NULLs come back as empty strings.

        Batch mode without --raw, deliberately: comment columns in the AI
        tables contain literal newlines, and unescaped they split one row
        across several lines and silently shift every field after them.
        """
        env = dict(os.environ, MYSQL_PWD=self._pw)
        out = subprocess.run(
            ["mysql", "-h", self.host, "-P", self.port, "-u", self.user,
             "-N", "-B", self.db, "-e", sql],
            capture_output=True, text=True, env=env,
        )
        if out.returncode:
            raise SystemExit(out.stderr.strip())
        rows = []
        for line in out.stdout.splitlines():
            row = ["" if c == "NULL" else c for c in line.split("\t")]
            if columns:
                # Pad rather than crash: a short row means a parse surprise,
                # and losing a trailing comment beats losing the whole map.
                row = (row + [""] * columns)[:columns]
            rows.append(tuple(row))
        return rows


# spell_template.effectImplicitTargetA. Only the two that decide whether
# aiming a spell at an enemy is a mistake.
TARGET_UNIT_CASTER = 1    # self-targets regardless of the unit target
TARGET_UNIT_FRIEND = 21   # explicit friendly; fails on a hostile target


def spell_names(w, ids):
    """spell_template is keyed (entry, patch), so collapse to one row per id."""
    return {k: v["name"] for k, v in spell_info(w, ids).items()}


def spell_info(w, ids):
    """Name plus the implicit targets, which decide whether a cast can land."""
    ids = {int(i) for i in ids if i and int(i) > 0}
    if not ids:
        return {}
    lst = ",".join(str(i) for i in sorted(ids))
    rows = w.query(
        "SELECT entry, MIN(name), MIN(effectImplicitTargetA1), "
        "MIN(effectImplicitTargetA2), MIN(effectImplicitTargetA3) "
        f"FROM spell_template WHERE entry IN ({lst}) GROUP BY entry",
        columns=5,
    )
    return {int(r[0]): dict(name=r[1],
                            implicit=[int(r[2] or 0), int(r[3] or 0), int(r[4] or 0)])
            for r in rows}


def creature_names(w, ids):
    ids = {int(i) for i in ids if i and int(i) > 0}
    if not ids:
        return {}
    lst = ",".join(str(i) for i in sorted(ids))
    rows = w.query(f"SELECT entry, name FROM creature_template WHERE entry IN ({lst})")
    return {int(r[0]): r[1] for r in rows}


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

def fetch_template(w, entries):
    lst = ",".join(str(e) for e in entries)
    rows = w.query(
        "SELECT entry, name, level_min, level_max, rank, health_multiplier, "
        "spell_list_id, ai_name, script_name, mechanic_immune_mask, "
        "school_immune_mask, static_flags1, leash_range, call_for_help_range "
        f"FROM creature_template WHERE entry IN ({lst})", columns=14
    )
    out = {}
    for r in rows:
        out[int(r[0])] = dict(
            entry=int(r[0]), name=r[1], level_min=int(r[2]), level_max=int(r[3]),
            rank=int(r[4]), health_multiplier=float(r[5] or 1),
            spell_list_id=int(r[6] or 0), ai_name=r[7], script_name=r[8],
            mechanic_immune_mask=int(r[9] or 0), school_immune_mask=int(r[10] or 0),
            static_flags1=int(r[11] or 0), leash_range=float(r[12] or 0),
            call_for_help_range=float(r[13] or 0),
        )
    return out


def fetch_rotation(w, list_ids):
    """creature_spells, unpivoted from its 8-slot wide layout.

    Timers are stored in seconds here and multiplied by IN_MILLISECONDS when
    ObjectMgr loads them, so they are reported in seconds to match the table.
    """
    if not list_ids:
        return {}
    lst = ",".join(str(i) for i in sorted(list_ids))
    parts = []
    for i in range(1, 9):
        parts.append(
            f"SELECT entry, {i} AS slot, spellId_{i} sp, probability_{i} prob, "
            f"castTarget_{i} tgt, targetParam1_{i} tp1, castFlags_{i} cf, "
            f"delayInitialMin_{i} imin, delayInitialMax_{i} imax, "
            f"delayRepeatMin_{i} rmin, delayRepeatMax_{i} rmax, scriptId_{i} sid "
            f"FROM creature_spells WHERE entry IN ({lst}) AND spellId_{i} > 0"
        )
    rows = w.query(" UNION ALL ".join(parts) + " ORDER BY entry, slot")
    names = spell_names(w, [r[2] for r in rows])
    out = {}
    for r in rows:
        out.setdefault(int(r[0]), []).append(dict(
            slot=int(r[1]), spell_id=int(r[2]), spell=names.get(int(r[2]), "?"),
            probability=int(r[3]), target=TARGETS.get(int(r[4]), f"target:{r[4]}"),
            target_param=int(r[5] or 0),
            cast_flags=decode_mask(int(r[6] or 0), CAST_FLAGS),
            initial_s=[int(r[7]), int(r[8])], repeat_s=[int(r[9]), int(r[10])],
            script_id=int(r[11] or 0),
        ))
    return out


def fetch_events(w, entries):
    """creature_ai_events plus the creature_ai_scripts actions they fire."""
    lst = ",".join(str(e) for e in entries)
    rows = w.query(
        "SELECT id, creature_id, event_type, event_inverse_phase_mask, event_chance, "
        "event_flags, event_param1, event_param2, event_param3, event_param4, "
        "action1_script, action2_script, action3_script, comment "
        f"FROM creature_ai_events WHERE creature_id IN ({lst}) ORDER BY creature_id, id",
        columns=14,
    )
    script_ids = set()
    for r in rows:
        for c in (10, 11, 12):
            if r[c] and int(r[c]):
                script_ids.add(int(r[c]))

    actions = {}
    if script_ids:
        slist = ",".join(str(i) for i in sorted(script_ids))
        arows = w.query(
            "SELECT id, delay, command, datalong, datalong2, datalong3, "
            "target_type, target_param1, data_flags, comments "
            f"FROM creature_ai_scripts WHERE id IN ({slist}) ORDER BY id, delay",
            columns=10,
        )
        # datalong is a spell id for cast/remove_aura/add_aura, and a creature
        # id for summon_creature. Resolve both so summons read as names.
        spell_ids = [a[3] for a in arows if int(a[2]) in (14, 15, 74)]
        creat_ids = [a[3] for a in arows if int(a[2]) == 10]
        sinfo = spell_info(w, spell_ids)
        cnames = creature_names(w, creat_ids)
        for a in arows:
            cmd = int(a[2])
            dl = int(a[3] or 0)
            entry = dict(
                delay=int(a[1] or 0),
                command=SCRIPT_COMMANDS.get(cmd, f"cmd:{cmd}"),
                datalong=dl, datalong2=int(a[4] or 0), datalong3=int(a[5] or 0),
                target_type=int(a[6] or 0),
                target=TARGETS.get(int(a[6] or 0), f"target:{a[6]}"),
                data_flags=int(a[8] or 0),
                comment=a[9] if len(a) > 9 else "",
            )
            # SF_GENERAL_TARGET_SELF rewrites the target to the caster, so it
            # is the difference between "heal myself" and "heal the tank".
            if entry["data_flags"] & SF_GENERAL_TARGET_SELF:
                entry["target"] = "self"
            if cmd in (14, 15, 74):
                info = sinfo.get(dl, {})
                entry["spell"] = info.get("name", "?")
                entry["implicit"] = info.get("implicit", [])
            elif cmd == 10:
                entry["summons"] = cnames.get(dl, "?")
            actions.setdefault(int(a[0]), []).append(entry)

    out = {}
    for r in rows:
        etype = int(r[2])
        name, params = EVENT_TYPES.get(etype, (f"EVENT:{etype}", ()))
        raw = [int(r[6] or 0), int(r[7] or 0), int(r[8] or 0), int(r[9] or 0)]
        out.setdefault(int(r[1]), []).append(dict(
            id=int(r[0]), trigger=name,
            params={k: v for k, v in zip(params, raw)},
            raw_params=raw,
            inverse_phase_mask=int(r[3] or 0), chance=int(r[4] or 0),
            flags=decode_mask(int(r[5] or 0), EVENT_FLAGS),
            actions=[a for c in (10, 11, 12) if r[c] and int(r[c])
                     for a in actions.get(int(r[c]), [])],
            comment=r[13] if len(r) > 13 else "",
        ))
    return out


def build(w, entries):
    tmpl = fetch_template(w, entries)
    if not tmpl:
        return []
    rot = fetch_rotation(w, {t["spell_list_id"] for t in tmpl.values() if t["spell_list_id"]})
    evts = fetch_events(w, entries)

    models = []
    for entry in entries:
        t = tmpl.get(entry)
        if not t:
            continue
        m = dict(t)
        m["rotation"] = rot.get(t["spell_list_id"], [])
        m["events"] = evts.get(entry, [])
        m["phases_used"] = sorted({
            a["datalong"] for e in m["events"] for a in e["actions"]
            if a["command"] == "SET_PHASE"
        })
        m["summons"] = sorted({
            a.get("summons", "?") for e in m["events"] for a in e["actions"]
            if a["command"] == "summon_creature"
        })
        m["blind_spots"] = blind_spots(m)
        m["checks"] = check(m)
        models.append(m)
    return models


# ---------------------------------------------------------------------------
# Internal consistency checks.
#
# These find places where our own data contradicts itself or contradicts the
# engine that reads it. That is a strictly weaker question than "does this
# match vanilla" -- a boss can be perfectly self-consistent and still be
# missing an entire phase -- but it is the part that needs no external
# reference, so it is free and it is exact.
#
# Every rule here was checked against the engine before being trusted. The
# rule that did NOT survive that check is worth recording: "self-heals are
# aimed at the victim" looked like 70 broken bosses until `data_flags & 0x4`
# (SF_GENERAL_TARGET_SELF, Map.cpp:2619) turned out to be set on 69 of them.
# ---------------------------------------------------------------------------

# Which event types hand an invoker to their actions. For every other type a
# target_type of 0 resolves to the creature's victim (CreatureEventAI.cpp:463),
# so a self-cast must set data_flags 0x4 to be correct.
EVENTS_WITH_INVOKER = {5, 8, 14, 15, 16, 17, 22, 25, 26, 32, 36}

# Where the repeat interval lives, per event type, since the columns are
# positional and differ by type.
REPEAT_PARAM_INDEX = {}
for _t in (0, 1, 2, 3, 8, 9, 10, 12, 14, 15, 16, 18, 23, 24, 27, 28, 29, 34, 36):
    REPEAT_PARAM_INDEX[_t] = 2
for _t in (5, 13, 33, 35):
    REPEAT_PARAM_INDEX[_t] = 0
for _t in (17, 25, 26):
    REPEAT_PARAM_INDEX[_t] = 1

SF_GENERAL_TARGET_SELF = 0x4


def check(m):
    """Return a list of (severity, rule, detail) for one behaviour model."""
    found = []
    for e in m["events"]:
        etype = e["trigger"]
        raw = e["raw_params"]
        repeatable = "repeatable" in e["flags"]
        idx = REPEAT_PARAM_INDEX.get(_event_type_num(etype))
        repeat = raw[idx] if idx is not None and idx < len(raw) else 0
        casts = [a for a in e["actions"] if a["command"] == "cast"]

        if repeat and not repeatable:
            # An HP threshold that fires once is usually what the author meant
            # (enrage, a summon wave). A combat timer that fires once almost
            # never is, because the interval it configures can never elapse.
            sev = "warn" if etype == "HP" else "bug"
            found.append((
                sev, "dead-repeat",
                f"{etype} sets a {repeat}ms repeat but is not flagged repeatable, "
                f"so it fires once per combat and the interval is dead"
                f"{' -- ' + e['comment'] if e['comment'] else ''}",
            ))

        if _event_type_num(etype) not in EVENTS_WITH_INVOKER:
            for a in casts:
                if a["target_type"] != 0 or a["data_flags"] & SF_GENERAL_TARGET_SELF:
                    continue
                # Resolving to the victim is correct for anything offensive,
                # which is most of them. It is only wrong when the spell
                # explicitly wants a friendly unit, because Spell::CheckCast
                # then rejects the cast outright with BAD_TARGETS.
                if TARGET_UNIT_FRIEND not in a.get("implicit", []):
                    continue
                found.append((
                    "bug", "friendly-at-victim",
                    f"{etype} casts {a.get('spell', a['datalong'])}, which requires a "
                    f"friendly target, but this event supplies no invoker so it "
                    f"resolves to the current victim and the cast fails. Set "
                    f"data_flags 0x4 for self"
                    f"{' -- ' + a['comment'] if a['comment'] else ''}",
                ))

        if (etype == "TIMER_IN_COMBAT" and not repeatable
                and not any(raw)):
            found.append((
                "bug", "fires-once",
                f"TIMER_IN_COMBAT with every timer zero and no repeatable flag is "
                f"just an aggro trigger: {', '.join(c.get('spell', '?') for c in casts) or 'its actions'} "
                f"happens at the pull and never again"
                f"{' -- ' + e['comment'] if e['comment'] else ''}",
            ))

    if m["rotation"]:
        for s in m["rotation"]:
            if s["repeat_s"] == [0, 0] and s["initial_s"] == [0, 0]:
                found.append((
                    "warn", "rotation-once",
                    f"{s['spell']} has no initial and no repeat delay",
                ))
    return found


def _event_type_num(name):
    for num, (n, _) in EVENT_TYPES.items():
        if n == name:
            return num
    return None


def blind_spots(m):
    """What this extract provably cannot see. The point of the whole tool.

    An empty model is ambiguous: it means either "this creature genuinely does
    nothing" or "this creature's behaviour is in C++". Saying which is the
    difference between a finding and a false positive.
    """
    out = []
    if m["script_name"]:
        out.append(
            f"script_name={m['script_name']}: logic lives in C++ and is NOT in this "
            "extract. Read src/scripts/**/ before drawing any conclusion."
        )
    if not m["rotation"] and not m["events"] and not m["script_name"]:
        out.append(
            "no rotation, no events, no script: melee-only, or genuinely unimplemented."
        )
    if m["ai_name"] and m["ai_name"] not in ("EventAI", ""):
        out.append(f"ai_name={m['ai_name']}: behaviour partly from that AI class.")
    if m["rotation"] and not m["ai_name"] and not m["script_name"]:
        out.append(
            "has a spell list but no AI name: rotation runs, but nothing adds "
            "phases or thresholds on top of it."
        )
    return out


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def fmt_range(lo, hi, unit):
    return f"{lo}{unit}" if lo == hi else f"{lo}-{hi}{unit}"


def render(m, out=sys.stdout):
    p = out.write
    rank = {0: "normal", 1: "elite", 2: "rare-elite", 3: "boss"}.get(m["rank"], m["rank"])
    lvl = (str(m["level_min"]) if m["level_min"] == m["level_max"]
           else f"{m['level_min']}-{m['level_max']}")
    p(f"\n{'=' * 74}\n{m['entry']}  {m['name']}   lv{lvl} {rank}  hp x{m['health_multiplier']:g}\n")
    src = []
    if m["spell_list_id"]:
        src.append(f"spell_list={m['spell_list_id']}")
    if m["ai_name"]:
        src.append(f"ai={m['ai_name']}")
    if m["script_name"]:
        src.append(f"script={m['script_name']}")
    p(f"  sources: {', '.join(src) or 'none'}\n")
    if m["mechanic_immune_mask"] or m["school_immune_mask"]:
        p(f"  immune: mechanic=0x{m['mechanic_immune_mask']:x} "
          f"school=0x{m['school_immune_mask']:x}\n")

    if m["rotation"]:
        p("\n  ROTATION (creature_spells)\n")
        for s in m["rotation"]:
            bits = [f"-> {s['target']}"]
            if s["probability"] != 100:
                bits.append(f"{s['probability']}%")
            bits.append("first " + fmt_range(*s["initial_s"], "s"))
            if s["repeat_s"] != [0, 0]:
                bits.append("every " + fmt_range(*s["repeat_s"], "s"))
            else:
                bits.append("ONCE")
            if s["cast_flags"]:
                bits.append("[" + ",".join(s["cast_flags"]) + "]")
            p(f"    {s['spell']} ({s['spell_id']})  {'  '.join(bits)}\n")

    if m["events"]:
        p("\n  EVENTS (creature_ai_events)\n")
        for e in m["events"]:
            ps = ", ".join(f"{k}={v}" for k, v in e["params"].items() if v or k.endswith("%"))
            head = e["trigger"] + (f"({ps})" if ps else "")
            extra = []
            if e["chance"] != 100:
                extra.append(f"{e['chance']}%")
            if e["inverse_phase_mask"]:
                extra.append(f"not-in-phases=0x{e['inverse_phase_mask']:x}")
            if e["flags"]:
                extra.append(",".join(e["flags"]))
            p(f"    {head}{'  [' + ' '.join(extra) + ']' if extra else ''}\n")
            for a in e["actions"]:
                what = a["command"]
                if "spell" in a:
                    what = f"{a['command']} {a['spell']} ({a['datalong']}) -> {a['target']}"
                elif "summons" in a:
                    what = f"summon {a['summons']} ({a['datalong']}) x{a['datalong2'] or 1}"
                elif a["command"] == "SET_PHASE":
                    what = f"SET_PHASE {a['datalong']}"
                elif a["datalong"]:
                    what = f"{a['command']} {a['datalong']}"
                p(f"        {what}\n")

    if m["phases_used"]:
        p(f"\n  PHASES: {m['phases_used']}\n")
    if m["summons"]:
        p(f"  SUMMONS: {', '.join(m['summons'])}\n")
    for sev, rule, detail in m.get("checks", []):
        p(f"\n  [{sev}] {rule}: {detail}\n")
    for b in m["blind_spots"]:
        p(f"\n  !! {b}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", type=int, help="every rank>0 creature spawned on this map")
    ap.add_argument("--entry", type=int, nargs="+", help="specific creature entries")
    ap.add_argument("--min-level", type=int, default=0)
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--check", action="store_true",
                    help="only report consistency findings, one line each")
    ap.add_argument("--conf", default=CONF_DEFAULT)
    args = ap.parse_args()

    if not args.map and not args.entry:
        ap.error("need --map or --entry")

    w = World(args.conf)
    if args.entry:
        entries = args.entry
    else:
        rows = w.query(
            "SELECT DISTINCT ct.entry FROM creature c "
            "JOIN creature_template ct ON ct.entry = c.id "
            f"WHERE c.map = {args.map} AND ct.rank > 0 "
            f"AND ct.level_min >= {args.min_level} "
            "ORDER BY ct.rank DESC, ct.level_min DESC, ct.entry"
        )
        entries = [int(r[0]) for r in rows]

    models = build(w, entries)
    if args.json:
        json.dump(models, sys.stdout, indent=1)
        sys.stdout.write("\n")
    elif args.check:
        n = 0
        for m in models:
            for sev, rule, detail in m["checks"]:
                n += 1
                print(f"{sev:4} {rule:16} {m['entry']:6} {m['name'][:26]:26} {detail}")
        scripted = [m for m in models if m["script_name"]]
        print(f"\n{n} findings over {len(models)} creatures; "
              f"{len(scripted)} are C++ scripted and therefore not covered here"
              + (": " + ", ".join(m["name"] for m in scripted) if scripted else ""))
    else:
        for m in models:
            render(m)


if __name__ == "__main__":
    main()
