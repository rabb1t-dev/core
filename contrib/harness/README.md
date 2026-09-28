# The test harness

Reference for whoever is driving this next, which is usually an agent with no game client and no
memory of the last session. It covers the `.harness` chat commands, the Python client that wraps
them, the suites built on top, and the traps that have each cost an afternoon at least once.

The purpose of the whole thing is to make the game observable and drivable from a script. A bot
does not report what it is doing, and almost every bug in this project has presented as "the bot
does not do X", which is also what a deliberate decision looks like. These commands exist to tell
those two apart.

## Getting a command to run at all

SOAP binds to loopback, so **the driver runs on the server host**, not on the machine you are
editing from. Over SSH that means the script goes to the box, not the other way round.

The server host is the Raspberry Pi at `ssh rabb1t@10.0.1.7`, and has been since the move off
the WSL2 desktop. It is the only one: the old box at `ssh vmangos` still has the tree and the
databases as they were on the day of the move, but its services are disabled and starting them
would give you a second world with a stale copy of everyone's characters. The Pi's firewall
refuses to originate connections into the LAN, so a script running there cannot reach back to
your workstation -- copy files *to* it, never pull from it.

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

All are administrator-only and console-enabled. Those that address a character take its
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
| resources | `health maxhealth power maxpower powertype ammo ammocount mhenchant ohenchant incombat selfres form` |
| `item` (one per backpack slot) | `entry count slot name` |
| group, then one `member` each | `group members raid leader`, then `name class level subgroup alive deathstate map zone incombat` |

Fields worth knowing rather than guessing:

- `deathstate` distinguishes a corpse still waiting for a resurrection (`CORPSE`) from a released
  ghost (`DEAD`). `alive=0` cannot express that difference, and all of wipe recovery turns on it.
- `teleporting` and `motion` say whether the AI is running at all. A bot stuck mid-teleport is not
  ticked, which from outside is identical to one that has decided to stand still.
- `mhenchant` and `ohenchant` are the temporary enchant on each weapon, which is how shaman imbues
  and rogue poisons are applied. They leave no other trace.
- The `item` lines exist because a consumable that is spent looks exactly like one that is not
  until the stack is counted. This is how poison consumption is verified.
- `incombat` is what separates a resurrection during the fight from one after it, which is the
  whole distinction combat resurrection is about. It appears on the `member` lines too, so the
  state of a forty-man raid is one call rather than forty.
- `selfres` is the charge the engine is holding for this death, already resolved into whichever
  of soulstone, Ankh or Twisting Nether applies. Still set on a bot that is still dead means
  nothing tried to spend it; cleared on one that is still dead means it was spent on a cast that
  failed, and the two want opposite fixes.
- `form` gates more of a druid's spell list than any other single piece of state. A druid in bear
  form declining to cast and a druid missing the spell entirely are the same silence otherwise,
  and this is what identified a flaky Rebirth test as a Moonkin.

### `.harness threat <character>`

The threat list of whatever this character is currently attacking, highest first: a summary line
of `entries topthreat combat victim`, then one `hostile` line each of
`threat percent top melee dist name`.

Threat decides who a boss hits and is invisible from every other angle. Percentages are of the
current victim's threat rather than absolute, because the rule that decides the target is written
as a ratio: `ThreatContainer::selectNextVictim` switches above 130 percent, or above 110 percent
when the creature can reach the candidate with a melee swing.

`melee` is which of those two a given hostile is actually being judged by, asked of the creature
rather than inferred. The rule follows position and not class, so a caster that has drifted into
reach flips at 110 like anything else, and a mage sitting at 117 percent reads as comfortably
safe until you notice `melee=1`. Do not guess this from the class; that mistake hid a real
handover for an afternoon.

Note the character has to be *attacking* something, not merely in combat with it. The leader is
the usual mistake here: nothing drives it, so it never has a victim and the whole group reads as
having never engaged. Ask through a bot instead.

Read peaks with the clock in mind, which is what `combat` is for: it is how many seconds the mob
has been fighting, and in the first few of them the tank's threat is near zero and every ratio
against it is enormous, so anything measured there describes arithmetic rather than behaviour.

`combat` also tells you whether you are watching the fight you started. A test target left over
from an earlier run reads in the hundreds, and a group that latched onto one is not measuring its
own pull at all. Clear the field with `.harness despawn` before summoning.

### `.harness despawn <character> <entry> [range]`

Remove every creature of one entry within range of this character, default 500 yards. Reports
`despawn entry range removed`.

This exists because `.npc despawn` works on the caller's selection and a command arriving over
SOAP has selected nothing, so a suite that summons a target has no way to take it away again.
Summoned targets are worse than untidy: one left in combat with an unkillable harness character
never resets, and the next run's bots assist against it rather than against the mob that run
summoned. Call it before summoning, not after, so a suite that crashed still leaves a clean field
for the next one.

### `.harness dynobj <character> [spell] [range]`

Every dynamic object near this character: a summary line, then one `dynobj` line each of
`guid spell radius duration hostile dist x y z inside`.

The one hazard in a dungeon that no other command here can see. A persistent area aura is a
DynamicObject on the floor with a radius and a periodic effect -- there is no creature for
`.harness enemy` to list, no gameobject for `.harness gobject` to report, no threat entry and no
cast. From outside, a party standing in Maraudon's Noxious Cloud and a party standing in clean air
differ only by their health going down.

`inside` is the field to assert on: the party members whose feet are within the radius right now,
comma separated, or `-`. Everything else on the line is context for reading a failure.

`hostile` is asked of the object rather than inferred from the spell, because the same spell id can
belong to a patch laid down by either side and a test that counted the party's own Blizzard as a
hazard would fail on a mage. Spell zero means every dynamic object.

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

### The gear commands

Seven commands exist only to make `ItemEvaluator` observable, since gear decisions otherwise happen
inside a trade window that cannot be conducted over SOAP.

| Command | What it does |
| --- | --- |
| `.harness items <character>` | A summary of `character equipped bag mailed`, then one `equipped slot=` line each, one `bag slot=` line per backpack slot, a `container slot=` line and `bag slot=<bag>:<index>` lines per equipped bag, and one `mailed entry=` line per item in the mail |
| `.harness equipnew <character>` | Runs the equip pass directly, which is otherwise reachable only by completing a trade |
| `.harness itemstats <entry>` | The resolved stat vector for an item prototype, including its equip-trigger spells |
| `.harness spellstats <spell>` | The same for a bare spell, which is how a disagreement gets attributed to a spell rather than an item |
| `.harness loadout <class> <spec> <entry>...` | Scores a hypothetical set of gear nobody is wearing, linear and capped, listing the set bonuses it triggered |
| `.harness wear <character> <entry>` | Equips a new item into its natural slot, which is the only way to give a bot a bag |
| `.harness stow <character> <entry> <container>` | Puts a new item *inside* an equipped bag, named by its container slot 19 to 22, which `.additem` cannot do because it fills the backpack first |

The bag and mail lines are not cosmetic. Every gear test distinguishes an item displaced from one
destroyed by asking where it went, and displaced gear goes to a bag or, when the bags are full, to
the mail — so a report that stopped at the backpack would read "moved" as "gone".

Naming the same item ten times in `loadout` is how twenty points of hit gets tested, which is the
only way to ask a question about a cap without assembling the gear to reach it.

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
| `test_maraudon.py` | Maraudon's eight fixed bosses in turn, plus the ground hazard, each asserting the tactic it exists to exercise | `--only`, `--level`, `--keep` |
| `test_combat_resurrection.py` | A bot killed mid-fight is raised before the fight ends, by a druid or by its own Ankh | ~5 min. `--only rebirth\|reincarnation` |
| `test_threat_throttling.py` | Damage dealers let the tank open, then hold below the pull threshold while it keeps the target | ~5 min. `--size`, `--duration` |
| `test_spell_population.py` | The bot spell struct is filled, class by class | `--only`, `--empty` |
| `test_premade_specs.py` | A talent build is deliberate and legal | `--only authored\|determinism\|fallback` |
| `test_totem_and_blessing_choice.py` | Totems and blessings are chosen rather than drawn at random | `--skip` |
| `test_raid_group.py` | A roster grows past five and stops at the raid ceiling | |
| `test_raid_guild_roster.py` | An authored roster becomes real characters that can log in | |
| `test_raid_guild_summon.py` | A provisioned roster can be summoned as a raid and sent home | |
| `test_bot_gear_preservation.py` | Handing a bot an upgrade does not destroy what it was wearing | |
| `test_item_evaluator.py` | The engine reads all 2475 items and 498 equip spells the way Classic Gear Ranker does | ~2 min. Writes every disagreement to `/tmp/item_evaluator_failures.txt` |
| `test_item_evaluator_behaviour.py` | A bot wears the better of two necks, declines the worse, and keeps both | |
| `test_item_evaluator_loadout.py` | Hit and weapon skill pay nothing past their caps, and a set bonus lands at its threshold and not before | Scores hypothetical gear, so it summons nobody |
| `test_item_evaluator_sets.py` | A bot assembles a six-piece set, keeps it against a small upgrade, breaks it for a large one, and equips out of a bag | |
| `sweep_corpse_runs.py` | Cheap survey: could a ghost *in principle* walk each route | Superseded by the live suite; a route existing and a bot walking it are different claims |
| `audit_premade_specs.py` | Reports what each level 60 premade build actually is | Not a pass/fail test |
| `talent_dbc.py` | Reads `Talent.dbc` so builds can be authored against real data | Library and CLI |
| `roster_fixture.py` | Leader plus bot roster and inventory helpers shared by the two gear suites | Library |

## Traps

Each of these produced a failure that looked like something else entirely.

**Do not blind-sleep waiting for a suite.** Watch it, so the wait ends when the work does:

```bash
tail -n +1 -f --pid=$(pgrep -f '[t]est_corpse_runs_live' | head -1) /tmp/run.log
```

Bracket the first character of the pattern so `pgrep` cannot match its own command line, which
otherwise leaves you tailing a process that already exited. The same trap applies to `pkill`, which
will happily kill the shell that invoked it.

**To watch a fight you need a fight that lasts and kills nobody.** These two pull opposite ways,
because health and damage both scale with level, so a mob tough enough to last is usually lethal
and a harmless one dies in seconds. Creature 11080, `[PH[ Combat Tester`, is a level 60 with a
hundred times the usual health and entirely ordinary damage, which is the combination and almost
nothing else in the game has it. Summon it with `.npc summon`, not `.npc add`: add writes a
permanent row to the creature table, summon does not.

A real raid boss was tried first and was worse in the way that matters. It killed the druid under
test, and the suite reported that as the druid declining to resurrect anyone.

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

**The world restarts itself for honor maintenance.** `HonorMaintenancer::CheckMaintenanceDay` logs
"Server needs to be restarted to perform honor rank calculations" and then calls
`ShutdownServ(900, SHUTDOWN_MASK_RESTART, RESTART_EXIT_CODE)` -- a fifteen minute countdown nothing
driving the server is told about, on a config (`AutoHonorRestart`) that defaults to on. It cost a
whole Zul'Farrak run: the instance and the party vanished mid-encounter, and from outside that is
identical to a stuck fight, so the suite waited out its full timeout on creatures that no longer
existed and reported a wave three failure. The only trace is in journald, because the restart
truncates `Server.log` without archiving it and takes the bot tick log with it.

It is off on the dev server now, and the maintenance had nothing to do anyway -- it reports
`Alliance: 0, Horde: 0, Inactive: 0`. Any suite that runs for more than a few minutes should still
read `server info` at both ends and void its own result if the uptime went *down*, which is the
only cheap way to tell "the encounter failed" from "the world went away".

**Restarting mangosd needs an explicit wait.** A socket in `TIME_WAIT` does not appear in the listen
table, so starting into one leaves mangosd running with **no SOAP at all** — indistinguishable from
a healthy server until every command times out. Use `~/bin/vmangos-restart`, which waits for the
process to exit and for the port to rebind before returning.

**A hand-started world is invisible to `systemctl`.** `vmangos-mangosd.service` can read
`inactive (dead)` while a mangosd started from the build tree keeps serving on PPID 1. `systemctl
stop` is then a no-op that still exits zero, and a restart script that trusts it walks past the
stop into the install and fails with `cp: Text file busy` -- overwriting the binary that is still
running. It cost a deploy on 2026-09-27, where the unit had been dead since the 26th and the world
had been up for two days regardless. `vmangos-restart` now asks systemd first and `pgrep -f
"[s]erver/bin/mangosd"` second, and SIGTERMs whatever the unit did not own, which is the signal
mangosd's own shutdown handler is on. Check `systemctl is-active vmangos-mangosd` against
`pgrep -af '[s]erver/bin/mangosd'` before believing either one.

**A refused route reports a shortfall of zero.** `.harness path` answers an unreachable
destination with `type=NOPATH` and the straight line it drew instead -- two points, the full
distance as `length`, and `shortfall=0.0`. So a survey that reads the shortfall and not the type
scores every blocked bearing as a clean route. Measured into Noxxion's pool in Maraudon, eight
bearings at thirty yards: five are NOPATH and all five report 0.0, while the three that work
report 0.1. Read `type` as flag names, reject `NOPATH|NOT_USING_PATH|INCOMPLETE|SHORTCUT`, and
treat the shortfall as a second check rather than the first.

**Standing within sight of a boss is not standing within reach of one.** The Maraudon suite's
first version staged thirty yards back along -x and settled for the boss being in `.harness
enemy` range. At Noxxion that is the wall of his pool: every bot could see him, every bot was
ordered onto him, and the tank logged `victim='Noxxion' vdist=28.6 moving=1 dmg=0` at an
unchanging position for the whole four hundred and twenty second timeout. From outside that is
identical to bots refusing to fight. Ask the mesh for a route from the candidate before staging
on it.

**The revision string is generated from git, not from the source that was compiled.** A tree
updated by rsync without its `.git` builds the new code and still reports the old commit in
`server info`, because `revision_data.h` comes from `git describe`. Confirm a deploy by asking for
something the new build has -- a command that did not exist before, a new field on a line -- not by
reading the revision back.

**Two workers, two worlds.** A restart takes the world down for a minute, which is fatal to a suite
that runs for five. `~/bin/server2` is a second world on SOAP 7879 with its own characters database,
sharing the world data. Note that `account_access` is keyed by realm, so a GM account on realm 1 is
an ordinary account on realm 2 until its rows are mirrored.

## Testing a dungeon end to end

Every dungeon is going to get this treatment, so the shape below is worth reusing rather than
rediscovering. Each rule under it was paid for once already, in Shadowfang Keep, and cost a build
and a run each time.

The shape that works is a **sequence of named stages**, one per encounter or pack, each returning
either `None` or a sentence saying what went wrong. A suite that only reports "the bots did not kill
the boss" cannot distinguish a stuck door from a stuck corridor from a lost fight, and those want
different fixes. Name the stage in the failure and the next step is obvious.

```
form party -> teleport in -> [clear pack -> rest] * n -> boss -> rest -> traverse -> ...
```

### Read the encounter out of the database before writing the test

The world database is the authority on what the fight actually is on this server, and it disagrees
with every guide often enough to matter. For each boss, before writing anything:

- `creature_template` for `spell_list_id`, `ai_name`, `level_min/max`.
- `creature_spells` for what it casts, and **`castFlags`** — `CF_ONLY_IN_MELEE` (0x40) and
  `CF_NOT_IN_MELEE` (0x80) on the same spell mean two different timers, which is a tactic. Arugal's
  Void Bolt is five to seven seconds with somebody in melee and **one second** without.
- `spell_template.castingTimeIndex` against `SpellCastTimes.dbc`, because "long cast" is the whole
  premise of any interrupt or line-of-sight tactic and half of them turn out to be instant.
  Index 1 is 0ms, 5 is 2000ms, 14 is 3000ms.
- `creature_ai_events` for the summons and the phases, and `spell_target_position` for any teleport.
  Arugal has three fixed Shadow Ports and that single fact invalidates every plan to hold him
  anywhere.

### Never enumerate creature entries by hand

`.harness enemy <char> 0 <range>` lists **every** hostile creature in range with its entry and name.
Each line also carries the creature's own `x y z`, which is what a standoff assertion has to be
measured against -- `dist` is the distance to the harness lead, and the lead stands wherever the
stage left it.
Use it. The alternative is a hand-written list of entries, and the dungeon will always have one more
than the list: Nandos calls three worgs of three entries, a Lupine Horror summons Lupine Delusions
of a fourth, and the room holds Wolfguard Worgs of a fifth. A stage that clears the entries it knows
about then waits out its timeout on the ones it does not, with the party standing around out of
combat, which looks exactly like bots refusing to fight.

### Scope every search to the pack, not to the instance

The default 300 yard search is almost always wrong. Entries are reused across a whole instance —
eight Sons of Arugal are spawned in Shadowfang and only three are in Arugal's room — so a wide
search finds mobs two floors away and orders the party onto all of them at once. That produced a
four bot party at level 26 attacking nine Bleak Worgs simultaneously and reading the resulting wipe
as a bot failure. Twenty five to thirty yards is a pack.

### Pull one mob at a time, and rest between pulls

Ordering the party onto everything of an entry at once is not how the game is played and not what
the bot code is written for. Pull the nearest, fight it, rest, repeat.

Resting matters as much as the pulls. Bots eat and drink on their own out of combat but only if
given the time, and a suite that walks from a boss straight into its adds on no mana loses fights
the party wins rested. Wait on the actual numbers — worst health and worst mana across the party,
with a timeout — not on a fixed sleep.

### Re-issue the attack order every poll; never latch it

The single most misleading bug so far. Issuing `partybot attackstart` once and then only watching
the target's health means that if the party disengages for any reason — the mob evades and resets,
the target dies and aggro scatters, a summon steals it — nothing ever re-orders them, and the suite
burns its whole timeout watching a party stand still. `victim='none'` in the tick log with mana
climbing is the signature: that is a party at rest, not a party losing.

### Verify state changes, do not assume them

An encounter that opens a door is only half tested by the boss dying. A door's open or shut is
instance state held in memory, so it cannot be read from the world database — `.harness gobject
<char> <entry> <range>` reports `statename=open|shut`. Check it is shut in a fresh instance before
the boss and open after, or the test cannot tell a working script from a lucky teleport.

### Make the party walk, and check that it arrived

`go xyz` teleports the leader and the bots follow by teleporting too, which tests nothing about
navigation. To test a corridor, move the leader in ten to fifteen yard legs and after each leg
assert that every bot is near the leader, naming the ones that are not and where they are. That is
what catches `pullblock` refusals and stuck geometry, which is the failure the bots are most prone
to in a keep.

### Measure the room before writing a tactic for it

Two commands exist for this and both answer questions no amount of reasoning will:

- `.harness ground <char> <x0> <y0> <x1> <y1> <step> [probeZ]` reads the floor over a rectangle.
  **Teleporting a character about and reading its position back does not work** — the position
  correction looks for ground near the height it was handed, so a character dropped onto a raised
  walkway stays on the walkway and the sunken floor two yards away never appears, and a character
  dropped into thin air simply floats there and reports the height you asked for. Probe from just
  above the floor you care about; probing from high up finds the ceiling.
- `.harness cover <char> <wx> <wy> <wz> [dynlos]` reports where, from where the character stands,
  it could get out of sight of a caster at that point, plus whether the character can see it at all.
  Dynamic line of sight is **off** by default on purpose: a closed door reads as cover that will not
  be there during the fight, and measuring Arugal's doorway with his lair shut made every spot on
  the far side look safe.

### A verdict has to be witnessed

A mob absent from the grid is **unknown**, never dead. Require `alive=0` on a creature that is still
there; report an absent one as its own outcome. Two suites have reported clean kills for raids that
achieved nothing because absence was read as death.

### Expect the harness itself to be the bug first

Roughly two thirds of the failures in the Shadowfang run were in the test, not the server: search
radii, latched orders, missing rests, a walk started while the previous fight was still going. Before
concluding the bots cannot do something, check the tick log for what they were actually doing. It is
cheap and it has been right more often than the hypothesis was.

Other things that bite:

- **`character level` takes the character out of the world briefly**, so a `revive` immediately after
  it fails with `Player not found`. Tolerate it and re-login rather than racing it.
- **The group holds its old bots.** `partybot removeall` runs in the leader's session; a run that
  starts before the previous run's bots are gone silently forms a short party. Assert the party size
  after forming and fail loudly.
- **A fresh instance is not free.** Walk the leader out, `instance unbind all`, then back in;
  otherwise the boss is dead and the door is already open from the last attempt. `.harness respawn`
  resets creatures in place and is about a minute faster when the instance itself can be reused.
