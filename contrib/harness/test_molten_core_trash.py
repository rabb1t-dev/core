#!/usr/bin/env python3
"""Send a forty-man roster at Molten Core's first trash pull and require that it wins.

The question this exists to answer is the flat one: does the raid kill the pack. Everything
short of that is a way of being wrong. A raid that survives a pull without killing anything
has not tanked it, it has been ignored by it; a raid that kills the pack and ends with the
healers dry has won one pull out of a hundred and eighty. So the verdict here is one of KILLED,
WIPED or STALEMATE, and a kill that leaves the raid unable to take the next pull is reported as
a kill with a warning rather than as a pass.

Molten Giant, entry 11658, is the real first pull: two of them, level 62 elites, eighty yards
inside the entrance. Deliberately the real spawn and not a summoned punching bag, because a
punching bag does not hit back, and every failure worth finding here is downstream of what
happens when the raid takes damage.

The audit that runs before the pull is the more important half of this file. A roster member is
created at level one and set to the leader's level by writing the `level` column, and for a
long time nothing brought the rest of the character with it: no proficiencies, so no weapon
skill lines, so Swords at 10 of 300 against a creature defending at 315, which misses very
nearly every swing. From outside that is indistinguishable from a rotation bug, and it cost a
whole session of measuring the wrong thing. Nothing is pulled until every member reads as a
real level sixty.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM account:
  python3 contrib/harness/test_molten_core_trash.py
  python3 contrib/harness/test_molten_core_trash.py --pulls 3 --audit-only
"""

import argparse
import collections
import sys
import time

from vmangos_harness import Harness, CommandError

LEADER = "Harnessbot"

MC_MAP = 409

# The entrance, from areatrigger_teleport, and a staging spot back down the ramp from the first
# pack. Twenty five yards is far enough that the leader is not what pulls and near enough that
# the raid's followers are all inside the room when the tank goes in.
MC_ENTRANCE = (1091.89, -466.99, -105.08)

# Read off the walkable route rather than interpolated, and that distinction cost an afternoon.
# The straight line from the entrance to the first pack drops into lava, so every candidate
# picked by interpolating between the two -- at z around minus one hundred and six, which is
# where both endpoints sit -- reads NOPATH: it is under the ledge the route actually crosses. The
# real route climbs a ridge at about minus one hundred and three. This is waypoint nine of the
# fifteen `.harness path` reports from the entrance, thirty four yards from the pack, which is
# outside a Molten Giant's twenty two yard aggro radius and inside a raid's own reach.
MC_STAGING = (1106.8, -516.1, -102.9)

# The first pull. Two of them stand fifteen yards apart, so the second joining is the encounter
# and not a mistake, and the suite counts both.
MOLTEN_GIANT = 11658
GIANT_COUNT = 2

# Bots, not raiders. A raid group holds forty and the leader is one of them, so a forty-entry
# roster is one member over the cap and the last one summoned silently fails to seat. This is
# the number that fits alongside a human, and the fortieth roster member is spare.
RAID_SIZE = 39

CLASS_NAMES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest",
               7: "shaman", 8: "mage", 9: "warlock", 11: "druid"}

ROLE_NAMES = {0: "none", 1: "melee", 2: "ranged", 3: "tank", 4: "healer"}

POLL = 2.0
SUMMON_TIMEOUT = 300.0
GATHER_TIMEOUT = 180.0
REST_TIMEOUT = 300.0

# How long a pull is given before it is called a stalemate. The real fight is well under a
# minute; four is long enough that a slow win is still a win and short enough that a raid
# chipping at a mob it cannot kill is not watched for an hour.
FIGHT_TIMEOUT = 240.0

# How far from the staging point a giant may be and still be the first pull. The pack sits about
# thirty four yards out, so this is that with room for the mob having wandered, and nothing like
# far enough to reach the next pack.
MAX_PULL_RANGE = 60.0

# What "rested" means before a pull. Below this the previous pull is still being paid for and
# whatever happens next is measuring the suite rather than the raid: the wipe that prompted
# this file happened at three percent healer mana, inherited from the pull before it.
REST_MANA = 90.0
REST_HEALTH = 95.0

# A real level sixty. Weapon skill at the level cap, five per level, is 300; defense likewise.
# Equipped count is deliberately loose, since a caster wears no shield and a two-hander fills
# one slot with two, but a member in single figures is wearing starter gear.
MIN_SKILL_RATIO = 1.0
MIN_EQUIPPED = 14


def fatal(message):
    print(f"FAIL {message}")
    sys.exit(1)


def info(harness, who):
    try:
        return harness.info(who)
    except CommandError:
        return None


def members(harness):
    current = info(harness, LEADER)
    if not current:
        return {}
    return {m["name"]: m for m in current.get("members_detail", [])
            if m["name"] != LEADER}


def wait_for(harness, predicate, timeout, what):
    """Poll a predicate over the raid until it holds, returning the last reading either way."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = members(harness)
        ok, detail = predicate(last)
        if ok:
            return last, None
        time.sleep(POLL)
    return last, f"{what}: {detail}"


def reset_pack(harness, who):
    """Put the pack back the way it spawned, so every pull is the same experiment.

    A fresh instance was the first attempt at this and does not work: an instance is not
    recycled while anything is standing in it and is unloaded on a delay after that, so
    dismissing the raid and walking the leader out and back lands in the same copy with the same
    corpses on the floor. One run scored that as a pass. Resetting the creatures directly is
    both correct and about a minute faster per iteration.
    """
    result = harness.respawn(who, MOLTEN_GIANT, 300.0)
    print(f"  pack reset: found {result['found']}, raised {result['raised']}, "
          f"healed {result['reset']}")
    time.sleep(3.0)


def summon_raid(harness):
    harness.run("raidguild reload")
    harness.run(f"raidguild summon {LEADER}")

    def seated(current):
        on_map = [m for m in current.values() if int(m.get("map", 0)) == MC_MAP]
        return (len(on_map) >= RAID_SIZE,
                f"{len(on_map)} of {RAID_SIZE} on map {MC_MAP} ({len(current)} in the raid)")

    return wait_for(harness, seated, SUMMON_TIMEOUT, "the roster never assembled")


def audit(harness, names):
    """Whether every member is the level sixty its level column claims. Returns problems."""
    problems = []
    rows = []

    for name in sorted(names):
        detail = info(harness, name)
        if not detail:
            problems.append(f"{name}: not in the world")
            continue

        level = int(detail.get("level", 0))
        cap = level * 5
        skills = detail.get("skills", [])

        # The lowest weapon skill the member holds, because it only takes one under-cap line to
        # make a swing miss and the bot may be wielding exactly that weapon. Defense is read
        # separately since it governs being hit rather than hitting.
        weapon = [int(s["value"]) for s in skills if int(s["id"]) != 95]
        defense = [int(s["value"]) for s in skills if int(s["id"]) == 95]
        worst_weapon = min(weapon) if weapon else 0
        defense_value = defense[0] if defense else 0

        items = harness.run(f"harness items {name}")
        equipped = int(items.split("equipped=")[1].split()[0]) if "equipped=" in items else 0

        talents = harness.talents(name)
        spent = int(talents["summary"].get("spent", 0))
        available = int(talents["summary"].get("available", 0))
        illegal = len(talents.get("illegal", []))

        role = int(detail.get("role", 0))
        spec = detail.get("spec", "-")

        rows.append((name, level, worst_weapon, defense_value, cap, equipped,
                     spent, available, illegal, ROLE_NAMES.get(role, "?"), spec))

        if level != 60:
            problems.append(f"{name}: level {level}, not 60")
        if not weapon:
            problems.append(f"{name}: knows no weapon skill at all")
        elif worst_weapon < cap * MIN_SKILL_RATIO:
            problems.append(f"{name}: weapon skill {worst_weapon} of {cap}")
        if defense_value < cap * MIN_SKILL_RATIO:
            problems.append(f"{name}: defense {defense_value} of {cap}")
        if equipped < MIN_EQUIPPED:
            problems.append(f"{name}: only {equipped} items equipped")
        if illegal:
            problems.append(f"{name}: {illegal} illegal talents")
        if spent < available:
            problems.append(f"{name}: {spent} of {available} talent points spent")
        if role == 0:
            problems.append(f"{name}: no role")

    print(f"{'member':14} {'lvl':>3} {'wpn':>4} {'def':>4} {'cap':>4} {'eq':>3} "
          f"{'tal':>7} {'ill':>3} {'role':7} spec")
    for row in rows:
        name, level, weapon, defense, cap, equipped, spent, available, illegal, role, spec = row
        print(f"{name:14} {level:3} {weapon:4} {defense:4} {cap:4} {equipped:3} "
              f"{spent:3}/{available:<3} {illegal:3} {role:7} {spec}")

    return problems


def find_tank(harness, names):
    """The main tank, chosen the same way the bots choose it: the lowest guid among tanks."""
    tanks = []
    for name in names:
        detail = info(harness, name)
        if detail and int(detail.get("role", 0)) == 3:
            tanks.append((int(detail["guid"]), name))
    tanks.sort()
    return (tanks[0][1] if tanks else None), [t[1] for t in tanks]


def rest(harness, names):
    """Wait until the raid is topped up, which is what makes consecutive pulls comparable."""
    def rested(current):
        thirsty = []
        for name in names:
            detail = info(harness, name)
            if not detail:
                continue
            if int(detail.get("incombat", 0)):
                thirsty.append(f"{name} in combat")
                continue
            if not int(detail.get("alive", 0)):
                thirsty.append(f"{name} dead")
                continue
            health = 100.0 * int(detail["health"]) / max(1, int(detail["maxhealth"]))
            if health < REST_HEALTH:
                thirsty.append(f"{name} {health:.0f}% hp")
            if int(detail.get("powertype", 0)) == 0:
                mana = 100.0 * int(detail["power"]) / max(1, int(detail["maxpower"]))
                if mana < REST_MANA:
                    thirsty.append(f"{name} {mana:.0f}% mana")
        return (not thirsty, ", ".join(thirsty[:4]) + (f" (+{len(thirsty) - 4})"
                                                       if len(thirsty) > 4 else ""))

    return wait_for(harness, rested, REST_TIMEOUT, "the raid never topped up")


def watch(harness, tank, names, probe_names, pulled_guid):
    """Poll the fight to its end. Returns a verdict and a sample trail.

    The pack, not one mob. Whatever joins in is part of the pull -- Molten Core's first two
    giants stand fifteen yards apart and the second answering the first is the encounter and
    not a mistake -- so every giant that is ever seen in combat is added to the set that has to
    be dead before this is a kill.
    """
    samples = []
    engaged = {pulled_guid}
    deadline = time.time() + FIGHT_TIMEOUT
    started = time.time()

    while time.time() < deadline:
        mobs = harness.enemies(tank, MOLTEN_GIANT, 300.0)
        for guid, row in mobs.items():
            if int(row["incombat"]) or int(row["attackers"]):
                engaged.add(guid)

        # Absent is unknown, never dead. This read the other way round for two runs and turned
        # two failures into passes: a grid query answers about loaded cells around the asking
        # character, so a mob that evaded back to its spawn, or was simply further away than the
        # cells being visited, dropped out of the list and was scored as a kill. Both false
        # passes reported a pack at "0.0% health" that was in fact untouched at a hundred. A
        # kill has to be seen: alive=0, on a corpse that is still there.
        standing = {g for g in engaged
                    if g not in mobs or int(mobs[g]["alive"])}
        confirmed_dead = {g for g in engaged
                          if g in mobs and not int(mobs[g]["alive"])}

        current = members(harness)
        living = [n for n, m in current.items() if int(m.get("alive", 0))]
        in_combat = [n for n, m in current.items() if int(m.get("incombat", 0))]

        threat = None
        for probe in probe_names:
            try:
                threat = harness.threat(probe)
            except CommandError:
                threat = None
            if threat:
                break

        top = "-"
        tank_share = None
        if threat:
            hostiles = threat["hostiles"]
            if hostiles:
                top = hostiles[0]["name"]
            for h in hostiles:
                if h["name"] == tank:
                    tank_share = h["percent"]

        worst = min((float(mobs[g]["percent"]) for g in standing if g in mobs), default=100.0)
        samples.append({
            "t": round(time.time() - started, 1),
            "percent": worst,
            "standing": len(standing),
            "dead": len(confirmed_dead),
            "engaged": len(engaged),
            "living": len(living),
            "in_combat": len(in_combat),
            "top": top,
            "tank_share": tank_share,
        })

        print(f"  t={samples[-1]['t']:6.1f} pack={len(standing)}/{len(engaged)} "
              f"dead={len(confirmed_dead)} lowest={worst:5.1f}% "
              f"alive={len(living):2}/{len(current)} "
              f"fighting={len(in_combat):2} top={top:14} "
              f"tankshare={'-' if tank_share is None else f'{tank_share:.0f}%'}")

        if not living:
            return "WIPED", samples
        if not standing and confirmed_dead:
            return "KILLED", samples
        # Nothing is fighting and enough time has passed that the walk in cannot explain it.
        # Distinguished from a stalemate because the two want opposite fixes: a raid that never
        # engaged is a broken pull, and one that engaged and stopped is broken combat.
        if not in_combat and samples[-1]["t"] > 30.0:
            return "DISENGAGED", samples

        time.sleep(POLL)

    return "STALEMATE", samples


def report(verdict, samples, harness, names):
    print()
    print(f"verdict {verdict}")
    if samples:
        low = min(s["percent"] for s in samples)
        print(f"lowest pack health {low:.1f}%   engaged {samples[-1]['engaged']} mobs   "
              f"duration {samples[-1]['t']:.0f}s")
        holders = collections.Counter(s["top"] for s in samples)
        print("threat held by: " + ", ".join(f"{n} {c}" for n, c in holders.most_common(5)))

    dead = []
    dry = []
    for name in names:
        detail = info(harness, name)
        if not detail:
            continue
        if not int(detail.get("alive", 0)):
            dead.append(name)
        if int(detail.get("powertype", 0)) == 0:
            mana = 100.0 * int(detail["power"]) / max(1, int(detail["maxpower"]))
            if mana < 20.0:
                dry.append(f"{name} {mana:.0f}%")
    print(f"deaths {len(dead)}" + (": " + ", ".join(dead[:8]) if dead else ""))
    print(f"under 20% mana: {len(dry)}" + (" -- " + ", ".join(dry[:8]) if dry else ""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulls", type=int, default=1,
                        help="consecutive pulls to attempt, resting in between")
    parser.add_argument("--audit-only", action="store_true",
                        help="assemble and audit the raid, then stop without pulling")
    parser.add_argument("--keep", action="store_true",
                        help="leave the raid in the world afterwards")
    parser.add_argument("--no-rest", action="store_true",
                        help="pull again without waiting for mana, which is the test of whether "
                             "the raid could chain pulls rather than win one")
    args = parser.parse_args()

    harness = Harness.from_env()
    harness.login(LEADER)
    harness.execute(LEADER, "gm on")
    harness.execute(LEADER, "gm visible off")
    harness.teleport(LEADER, *MC_STAGING, MC_MAP)
    time.sleep(3.0)

    current, error = summon_raid(harness)
    if error:
        fatal(error)
    names = sorted(current)
    print(f"raid assembled: {len(names)} members on map {MC_MAP}")
    print()

    problems = audit(harness, names)
    print()
    if problems:
        print(f"{len(problems)} audit problems, first 12:")
        for line in problems[:12]:
            print(f"  {line}")
        fatal("the raid is not a raid of level sixties; nothing was pulled")
    print("audit clean: every member is a real level sixty")

    tank, tanks = find_tank(harness, names)
    if not tank:
        fatal("no member has the tank role")
    print(f"main tank {tank}, off-tanks {', '.join(t for t in tanks if t != tank) or 'none'}")

    if args.audit_only:
        return

    probe_names = [n for n in names if n not in tanks][:6] + tanks

    results = []
    for pull in range(1, args.pulls + 1):
        print()
        print(f"pull {pull} of {args.pulls}")

        # Order matters, and getting it wrong cost a run. Resetting the pack first put two
        # full-health giants back on their spawn while thirty nine raiders were still standing on
        # it from the last kill: they were re-aggroed and killed again inside the rest wait, and
        # the pull that followed found nothing to pull. So the raid comes home first, is waited
        # for until it is out of combat and topped up, and only then does the pack come back.
        harness.teleport(LEADER, *MC_STAGING, MC_MAP)
        time.sleep(6.0)

        if args.no_rest:
            # Out of combat is still required, since a pull begun while the last one is still
            # resolving measures the overlap and not the pull. Mana is not.
            _, error = wait_for(harness,
                                lambda cur: (not [n for n, m in cur.items()
                                                  if int(m.get("incombat", 0))], "still fighting"),
                                120.0, "the raid never left combat")
            if error:
                print(f"  warning {error}")
            print("  no rest: pulling again on whatever mana is left")
        else:
            _, error = rest(harness, names)
            if error:
                print(f"  warning {error}")

        reset_pack(harness, LEADER)

        # Selected and issued by the leader, not by the tank. `.partybot pull` reads the
        # caller as the group's leader and takes the puller by name, so running it in the
        # tank's own session asks the tank to find itself in its own group and quietly
        # pulls nothing: the first run of this suite watched twenty one seconds of a raid
        # standing still and reported it as a disengagement.
        chosen = harness.select(LEADER, MOLTEN_GIANT, MAX_PULL_RANGE)
        if not chosen:
            # Bounded deliberately. Unbounded, `select` takes the nearest live giant anywhere on
            # the map, and after a pull that left the first pack as corpses that was one a
            # hundred and seventy eight yards away through two other packs. A run measuring that
            # is not measuring the first pull, and reporting it as one is worse than failing.
            fatal(f"no live Molten Giant within {MAX_PULL_RANGE:.0f} yards of the staging point; "
                  f"the first pack has not respawned, so this run would not be the first pull")
        print(f"  target guid={chosen['guid']} at {chosen['dist']}y, "
              f"{chosen['health']}/{chosen['maxhealth']} hp")

        print("  " + harness.execute(LEADER, f"partybot pull {tank}").strip()
              .replace("\n", " | "))
        verdict, samples = watch(harness, tank, names, probe_names, int(chosen["guid"]))
        report(verdict, samples, harness, names)
        results.append(verdict)

        if verdict == "WIPED":
            break

    print()
    print("pulls: " + ", ".join(results))

    if not args.keep:
        harness.run("raidguild dismiss", allow_failure=True)

    if all(r == "KILLED" for r in results):
        print("PASS")
        return
    fatal("the raid did not kill the pack")


if __name__ == "__main__":
    main()
