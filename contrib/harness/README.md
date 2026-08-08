# The test harness

Reference for whoever is driving this next, which is usually an agent with no game client and no
memory of the last session. It covers the ten `.harness` chat commands, the Python client that
wraps them, the suites built on top, and the traps that have each cost an afternoon at least once.

The purpose of the whole thing is to make the game observable and drivable from a script. A bot
does not report what it is doing, and almost every bug in this project has presented as "the bot
does not do X", which is also what a deliberate decision looks like. These commands exist to tell
those two apart.

## Getting a command to run at all

SOAP binds to loopback, so **the driver runs on the server host**, not on the machine you are
editing from. Over SSH that means the script goes to the box, not the other way round.

Three things have to be true:

- `Harness.Enable = 1` in `mangosd.conf`. It defaults to **off** and every command checks it,
  answering `Harness: disabled` when it is not set. It is on for the dev server.
- `VMANGOS_SOAP_USER` and `VMANGOS_SOAP_PASSWORD` name an account with administrator rights.
  Nothing reads a config file for these, and `Harness.from_env()` raises if they are unset.
- `VMANGOS_SOAP_URL` if you are not on the default world. Server 1 is `http://127.0.0.1:7878/`
  and is assumed; server 2 is `http://127.0.0.1:7879/`.

A one-off command, without writing a script:

```bash
export VMANGOS_SOAP_USER=... VMANGOS_SOAP_PASSWORD=...
python3 contrib/harness/vmangos_harness.py harness info Harnessbot
python3 contrib/harness/vmangos_harness.py --as-character Harnessbot partybot add rogue
```

`--as-character` logs the character in first if it is not already in the world, then routes the
command through `.harness exec`.

## The commands

All ten are administrator-only and console-enabled. Those that address a character take its
*name*, never a GUID; `graveyard` and `loadmmaps` are world queries and take a map instead. The
output is `key=value` throughout, one space-separated line per fact, and **a value containing
spaces is always last on its line** so nothing has to be quoted. Parse accordingly.

### `.harness exec <character> <command>`

Runs any chat command in the session of an already logged-in character. This is the load-bearing
one: it is what makes client-only commands reachable from a script, `.partybot` above all, which
is flagged in-game-only and cannot be run from the console directly.

The command runs with **the caller's** access level, not the puppet's, so the character being
driven does not need to be a GM. A leading dot is accepted and discarded. Output is relayed back
to the caller.

### `.harness info <character>`

The state of a character and its group. Four kinds of line:

| Line | Fields |
| --- | --- |
| position | `name guid level map instance zone alive deathstate corpse teleporting motion x y z` |
| resources | `health maxhealth power maxpower powertype ammo ammocount mhenchant ohenchant` |
| `item` (one per backpack slot) | `entry count slot name` |
| group, then one `member` each | `group members raid leader`, then `name class level subgroup alive deathstate map zone` |

Fields worth knowing rather than guessing:

- `deathstate` distinguishes a corpse still waiting for a resurrection (`CORPSE`) from a released
  ghost (`DEAD`). `alive=0` cannot express that difference, and all of wipe recovery turns on it.
- `teleporting` and `motion` say whether the AI is running at all. A bot stuck mid-teleport is not
  ticked, which from outside is identical to one that has decided to stand still.
- `mhenchant` and `ohenchant` are the temporary enchant on each weapon, which is how shaman imbues
  and rogue poisons are applied. They leave no other trace.
- The `item` lines exist because a consumable that is spent looks exactly like one that is not
  until the stack is counted. This is how poison consumption is verified.

### `.harness spells <character>`

What spell population actually produced, slot by slot: a summary line, then one `slot` line per
named slot including the empty ones, then one `totem` line per totem slot.

Read `known=0` carefully. It is legitimate for exactly the two rogue poison slots, because a rogue
learns the trade spell that makes the vial and never the enchant the vial applies. Anywhere else it
is a bug.

Population matches spells **by name** into named struct slots, so a slot whose matcher is wrong
stays null forever and the bot simply never casts that spell. That is indistinguishable from a
rotation choice, which is why several of these survived a nine-class audit. Naming the empty slots
is the entire point of the command.

### `.harness talents <character>`

A character's build: a summary, one `tab` line per tree, one `talent` line per learned talent, and
one `illegal` line per talent that could not have been learned in the client.

Premade specs are applied with `LearnSpell` rather than `LearnTalent`, so the server never checks
tier requirements or the point budget. An impossible build applies without complaint, and `illegal`
plus `spent` against `available` is the only way to see it.

### `.harness path <character> <x> <y> <z> [trigger]`

What the navigation mesh answers for a route the character would have to walk:
`type points length from to reached shortfall arrived`.

Detour degrades quietly. It returns a partial route or a straight-line shortcut rather than
failing, so without this an unreachable destination is indistinguishable from a slow bot. `type` is
decoded as flags rather than matched as a value, because the interesting answers are combinations:
`NORMAL|NOT_USING_PATH` is a straight line drawn because the mesh was unavailable, and reporting
that as an unhelpful "OTHER" once hid a whole sweep's worth of false passes.

Pass an area trigger id to get `arrived`, using the same five-yard test the bot itself applies. A
dungeon portal is a volume and some are tall, so measuring distance to the trigger's centre calls
Ragefire Chasm a failure while the ghost is standing in the doorway.

### `.harness graveyard <map> <x> <y> <z> [team]`

Where a ghost dying at that spot releases to: `graveyard id map x y z`, or `graveyard none` if
nothing resolves, in which case the ghost stays where it died.

Ask rather than assume. For a death inside an instance the answer is a graveyard out on the
entrance map, chosen by faction, and that release point is where a corpse run actually begins.
Team is a faction id: 67 Horde, 469 Alliance.

### `.harness loadmmaps <map>`

Pulls every navigation tile of a map into memory: `mmaps map newly_loaded total_tiles`.

Needed before any survey done from a distance. Tiles normally load alongside the grids around a
player, and a path query into an unloaded tile does not fail — it abandons the mesh and answers
with a straight line, so an unprepared survey reports success everywhere. A continent costs well
under a gigabyte.

### `.harness rewardquest <character> <quest>`

Grants a quest as though the character had handed it in at the ender: `quest rewarded`.

This exists because **attunement cannot be granted by `.quest complete`**. That stops at
`QUEST_STATUS_COMPLETE`, and only `AUTO_REWARDED` quests go further, whereas the gates are checked
with `GetQuestRewardStatus`. Blackhand's Command is flagged `RAID` and is not auto-rewarded, so
without this a bot walks the full eighteen hundred yards to Blackwing Lair and is turned away at
the door, which reads exactly like a broken route.

### `.harness createchar <account> <name> <race> <class> [gender]`

Writes a real character row without a game client: `created name guid account race class`. Used to
provision puppets and roster members.

### `.harness login <name>`

Brings an existing character into the world with no client attached: `logging in name guid`, or
`already online name guid` if it is already there.

It passes an explicit AI so the entry is flagged as a custom bot, which is what stops
`PlayerBotMgr::Update` from skipping it while random bots are disabled.

## The Python client

`vmangos_harness.py`. `Harness.from_env()` reads the credentials and URL described above.

- `run(command, allow_failure=False)` — the workhorse. **The server reports success even when it
  refuses to run a command**, so `run` also inspects the text for known refusal markers and raises
  `CommandError`. Use `allow_failure=True` only when a refusal is an expected outcome; silencing it
  everywhere is how a setup step disappears and comes back later looking like a route bug.
- `info(character)` — parsed `.harness info`, or `None` when the character is offline. Repeated
  lines are collected into `members_detail` and `items` rather than merged, since folding them into
  one dict leaves only whichever came last.
- `login`, `logout`, `execute`, `teleport` — character lifecycle. `login` waits for the character to
  reach the world rather than returning immediately.
- `spells(character)`, `talents(character)` — parsed into `summary` / `slots` / `totems` and
  `summary` / `trees` / `talents` / `illegal`.
- `graveyard(...)`, `path(...)` — parsed world queries.

## The suites

Run from the repo root on the server host. Times are for the dev box.

| Script | What it proves | Notes |
| --- | --- | --- |
| `test_corpse_runs_live.py` | A bot dies inside each of the 26 instances and gets back in unaided | ~4 min. `--only`, `--raids`, `--dungeons`, `--workers`, `--trace` |
| `test_raid_wipe_recovery.py` | Each raid, filled to its player cap, wipes with nobody left standing and walks back | ~11 min. `--only`, `--size` |
| `test_wipe_recovery.py` | The single-group version of the same thing | |
| `test_spell_population.py` | The bot spell struct is filled, class by class | `--only`, `--empty` |
| `test_premade_specs.py` | A talent build is deliberate and legal | `--only authored\|determinism\|fallback` |
| `test_totem_and_blessing_choice.py` | Totems and blessings are chosen rather than drawn at random | `--skip` |
| `test_raid_group.py` | A roster grows past five and stops at the raid ceiling | |
| `test_raid_guild_roster.py` | An authored roster becomes real characters that can log in | |
| `test_raid_guild_summon.py` | A provisioned roster can be summoned as a raid and sent home | |
| `sweep_corpse_runs.py` | Cheap survey: could a ghost *in principle* walk each route | Superseded by the live suite; a route existing and a bot walking it are different claims |
| `audit_premade_specs.py` | Reports what each level 60 premade build actually is | Not a pass/fail test |
| `talent_dbc.py` | Reads `Talent.dbc` so builds can be authored against real data | Library and CLI |

## Traps

Each of these produced a failure that looked like something else entirely.

**Do not blind-sleep waiting for a suite.** Watch it, so the wait ends when the work does:

```bash
tail -n +1 -f --pid=$(pgrep -f '[t]est_corpse_runs_live' | head -1) /tmp/run.log
```

Bracket the first character of the pattern so `pgrep` cannot match its own command line, which
otherwise leaves you tailing a process that already exited. The same trap applies to `pkill`, which
will happily kill the shell that invoked it.

**One leader per account.** `PlayerBotMgr` allows one session per account, so a second and third
concurrent leader silently fail to log in. The live corpse-run suite creates `harnesslead0`,
`harnesslead1`, … for exactly this, sets each to GM level 6, and erases any character of that name
sitting on the wrong account first.

**Form groups on open ground, never where the leader is standing.** A leader that has just finished
a five-man is still inside it, and that instance's player cap turns away the last bot of the group
being formed for the next run. The reported symptom is "only 4 of 5 bots joined", which is the room
being full and not the group. The suites stage at `(-600, -2515, 92)` on map 1, in the Barrens.

**`Instance.PerHourLimit` defaults to 5.** A sequential suite dies after five instances with an
error that does not mention the limit. The dev server runs 1000.

**The Ahn'Qiraj gates re-close under you.** Stopping game event 83 once is not enough; the event
manager re-reads its schedule and puts it back, and both Ahn'Qiraj instances then fail standing at
an entrance that refuses them, which reads as a broken route. `test_corpse_runs_live.py` runs a
daemon thread that re-stops it every 20 seconds and restores it afterwards.

**Restarting mangosd needs an explicit wait.** A socket in `TIME_WAIT` does not appear in the listen
table, so starting into one leaves mangosd running with **no SOAP at all** — indistinguishable from
a healthy server until every command times out. Use `~/bin/vmangos-restart`, which waits for the
process to exit and for the port to rebind before returning.

**Two workers, two worlds.** A restart takes the world down for a minute, which is fatal to a suite
that runs for five. `~/bin/server2` is a second world on SOAP 7879 with its own characters database,
sharing the world data. Note that `account_access` is keyed by realm, so a GM account on realm 1 is
an ordinary account on realm 2 until its rows are mirrored.
