#!/usr/bin/env python3
"""Check that a ghost could walk from its release graveyard to each instance portal.

Whether a corpse run succeeds is a static property of the navigation mesh, so it can be
measured instead of performed. Running one with a bot takes minutes of walking to prove
what a few path queries prove in seconds, and there are far more entrances than there is
patience: Dire Maul alone has twenty eight of them.

What this cannot answer, so a clean sweep is necessary rather than sufficient: entry
requirements are reported but not tested, closed doors are invisible to the mesh, and a
route existing is not quite the same as the bot's movement executing it.

Run on the server host, with VMANGOS_SOAP_USER and VMANGOS_SOAP_PASSWORD set to a GM
account: python3 sweep_corpse_runs.py --raids
"""

import argparse
import collections
import os
import subprocess
import sys
import time

from vmangos_harness import Harness, CommandError, HORDE

# Continents. A portal anywhere else is inside another instance, which a ghost released
# outdoors cannot reach without first running a second dungeon.
OUTDOOR_MAPS = (0, 1)

# Emerald Dream is an unfinished map with no way in.
EXCLUDED_MAPS = (169,)

# Mirrors PB_CORPSE_RUN_PROGRESS_STEP: closing less than this per leg is not progress.
PROGRESS_STEP = 5.0

# A corpse run of a few thousand yards would still be well inside this.
MAX_HOPS = 80

# Cross-map teleports need a worldport ack to land before the next query.
SETTLE = 0.6

# Path flags meaning the answer did not come from the navigation mesh.
OFF_MESH = {"SHORTCUT", "NOT_USING_PATH", "NOPATH"}

Portal = collections.namedtuple(
    "Portal", "trigger map_id x y z inside_x inside_y inside_z level condition"
)


def query(sql):
    command = os.environ.get("VMANGOS_MYSQL", "sudo mysql mangos")
    result = subprocess.run(
        command.split() + ["-N", "-B", "-e", sql],
        capture_output=True, text=True, check=True,
    )
    return [line.split("\t") for line in result.stdout.splitlines() if line]


def load_instances(map_type):
    """Every instance of the given type, with its entrance portals deduplicated."""
    rows = query(f"""
        SELECT tp.target_map, mt.map_name, tp.id, t.map_id, t.x, t.y, t.z,
               tp.target_position_x, tp.target_position_y, tp.target_position_z,
               tp.required_level, tp.required_condition
        FROM areatrigger_teleport tp
        JOIN areatrigger_template t ON t.id = tp.id
        JOIN map_template mt ON mt.entry = tp.target_map
        WHERE mt.map_type = {map_type}
          AND mt.entry NOT IN ({",".join(str(m) for m in EXCLUDED_MAPS)})
        GROUP BY tp.target_map, mt.map_name, tp.id, t.map_id, t.x, t.y, t.z,
                 tp.target_position_x, tp.target_position_y, tp.target_position_z,
                 tp.required_level, tp.required_condition
        ORDER BY tp.target_map
    """)

    instances = collections.OrderedDict()
    for row in rows:
        map_id, name = int(row[0]), row[1]
        portal = Portal(int(row[2]), int(row[3]), *(float(v) for v in row[4:10]),
                        int(row[10]), int(row[11]))
        entry = instances.setdefault(name, (map_id, []))
        # The same trigger appears once per supported client build.
        if portal not in entry[1]:
            entry[1].append(portal)
    return instances


def walk(harness, probe, start, target, trigger=0, verbose=False):
    """Follow the mesh toward the target in legs, the way a corpse run does.

    A single query cannot answer this. Detour caps a path at 256 polygons and then returns
    its best partial route rather than failing, so asking once reports a false dead end on
    any journey longer than a few hundred yards. Re-issuing from where the last leg ended
    is what the bot does, and it is the only way to tell a long route from a blocked one.
    """
    map_id, x, y, z = start
    harness.teleport(probe, x, y, z, map_id)
    time.sleep(SETTLE)

    best = None
    travelled = 0.0

    for hop in range(MAX_HOPS):
        leg = harness.path(probe, *target, trigger)
        travelled += leg["length"]
        gap = leg["shortfall"]

        if verbose:
            x, y, z = leg["reached"]
            print(f"      leg {hop + 1:>2}  {leg['type']:<22} ran {leg['length']:>7.1f}y  "
                  f"stopped {x:.1f} {y:.1f} {z:.1f}  {gap:.1f}y remaining")

        # Checked before the mesh flags, because a route that ends inside the doorway has
        # arrived however unflatteringly Detour describes the last few yards of it.
        if leg["arrived"]:
            return True, travelled, hop + 1, f"inside the portal, {gap:.0f}y from its centre"

        # Checked before arrival, because this is how a bogus arrival looks. Detour does not
        # fail when it cannot use the mesh, it draws a straight line from here to the target
        # and reports reaching it, so an unreachable destination and a trivially reachable
        # one produce the same answer unless the flags are read. A route across open water
        # lands here too, which is why this asks for a look rather than declaring defeat.
        off_mesh = OFF_MESH.intersection(leg["type"].split("|"))
        if off_mesh:
            return False, travelled, hop + 1, \
                f"left the mesh {gap:.0f}y short ({'|'.join(sorted(off_mesh))})"

        if best is not None and gap > best - PROGRESS_STEP:
            return False, travelled, hop + 1, f"stalled {gap:.0f}y short ({leg['type']})"

        best = gap
        harness.teleport(probe, *leg["reached"], map_id)
        time.sleep(SETTLE)

    return False, travelled, MAX_HOPS, "exceeded hop budget"


def ghost_entrance_portal(instance_map, known):
    """The map's own ghost entrance, for instances entered by no teleport portal.

    Blackwing Lair is the only one: its way in is a scripted trigger beside the Orb of Command
    that answers only to the dead and appears in no teleport table. The map names the spot, and
    the trigger sitting on it supplies the height and the volume to test arrival against.
    """
    rows = query(f"""
        SELECT t.id, t.map_id, t.x, t.y, t.z
        FROM map_template mt
        JOIN areatrigger_template t ON t.map_id = mt.ghost_entrance_map
         AND ABS(t.x - mt.ghost_entrance_x) < 10 AND ABS(t.y - mt.ghost_entrance_y) < 10
        WHERE mt.entry = {instance_map} AND mt.ghost_entrance_map >= 0
        GROUP BY t.id, t.map_id, t.x, t.y, t.z
    """)
    if not rows:
        return None

    row = rows[0]
    return known._replace(trigger=int(row[0]), map_id=int(row[1]),
                          x=float(row[2]), y=float(row[3]), z=float(row[4]))


def check(harness, probe, name, map_id, portals, verbose=False):
    outdoor = [p for p in portals if p.map_id in OUTDOOR_MAPS]
    if not outdoor:
        fallback = ghost_entrance_portal(map_id, portals[0])
        if fallback and fallback.map_id in OUTDOOR_MAPS:
            outdoor = [fallback]

    if not outdoor:
        elsewhere = sorted({p.map_id for p in portals})
        return None, f"no portal on a continent; only reachable from map(s) {elsewhere}"

    # Where the ghost actually starts. For a death inside an instance the engine sends it
    # to a graveyard out on the entrance map, picked by faction, so this must be asked
    # rather than assumed.
    inside = outdoor[0]
    release = harness.graveyard(
        map_id, inside.inside_x, inside.inside_y, inside.inside_z, HORDE
    )
    if not release:
        return False, "no Horde graveyard is linked to this instance"

    failures = []
    for portal in outdoor:
        if portal.map_id != release[0]:
            failures.append(f"portal on map {portal.map_id}, ghost releases to {release[0]}")
            continue

        if verbose:
            print(f"   release {release[1]:.1f} {release[2]:.1f} {release[3]:.1f} "
                  f"on map {release[0]}  ->  portal {portal.x:.1f} {portal.y:.1f} {portal.z:.1f}")

        ok, distance, hops, why = walk(
            harness, probe, release, (portal.x, portal.y, portal.z),
            portal.trigger, verbose
        )
        if ok:
            return True, f"{distance:.0f}y over {hops} legs"
        failures.append(why)

    return False, "; ".join(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", default="Harnessbot")
    parser.add_argument("--raids", dest="map_type", action="store_const", const=2)
    parser.add_argument("--dungeons", dest="map_type", action="store_const", const=1)
    parser.add_argument("--only", action="append", default=[],
                        help="substring of an instance name; repeatable")
    parser.add_argument("--verbose", action="store_true", help="report every leg")
    parser.set_defaults(map_type=2)
    args = parser.parse_args()

    harness = Harness.from_env()
    harness.login(args.probe)
    harness.execute(args.probe, "revive")

    # Without this every answer below would be a straight line, because a path query whose
    # destination lies in an unloaded tile quietly stops using the mesh.
    for map_id in OUTDOOR_MAPS:
        print(harness.run(f"harness loadmmaps {map_id}"))

    instances = load_instances(args.map_type)
    print(f"{len(instances)} instances, Horde release points\n")

    results = []
    for name, (map_id, portals) in instances.items():
        if args.only and not any(w.lower() in name.lower() for w in args.only):
            continue
        if args.verbose:
            print(f"\n{name}")
        try:
            ok, detail = check(harness, args.probe, name, map_id, portals, args.verbose)
        except CommandError as exc:
            ok, detail = False, f"harness error: {exc}"

        mark = {True: "pass", False: "FAIL", None: "SKIP"}[ok]
        levels = {p.level for p in portals}
        conditions = {p.condition for p in portals if p.condition}
        gate = f" [level {max(levels)}" + (f", condition {sorted(conditions)}" if conditions else "") + "]"
        print(f"{mark}  {name:<22} {detail}{gate}")
        results.append((ok, name))

    failed = [name for ok, name in results if ok is False]
    skipped = [name for ok, name in results if ok is None]
    print(f"\n{sum(1 for ok, _ in results if ok)} reachable, "
          f"{len(failed)} unreachable, {len(skipped)} structural")
    if failed:
        print("live-test and fix: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
