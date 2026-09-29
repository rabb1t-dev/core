# Agent rules: vmangos party-bot work

Canonical file. `CLAUDE.md` and `.github/copilot-instructions.md` are symlinks to this one, so every
tool reads the same rules and there is one place to edit.

Each rule below was paid for once already, in a session that lost time or a run to not having it.
They are meant to be applied rather than rediscovered.

## Before testing a dungeon, read `contrib/harness/README.md`

Its last section, "Testing a dungeon end to end", is the accumulated guidance for driving an
instance from a script: the stage shape, which database tables to read before writing a tactic, and
the traps that have each cost a build and a run.

The two that account for most wasted runs: **scope creature searches to the pack** (the default 300
yards finds the same entry two floors away) and **re-issue the attack order every poll** rather than
latching it once. And when a stage stalls, read the bots' tick log before concluding they cannot do
the thing -- most stalls so far have been the test, not the server.

## Know the encounter before you run it

**Do not run an experiment whose outcome you can predict.** Encounter knowledge first is what makes
an attempt informative rather than confirmatory, and an attempt is the most expensive thing in this
loop.

- **The script is authoritative, then the database -- but always read both.** `src/scripts/**/boss_*.cpp`
 is what the boss does on this server. `creature_template` gives rank, level and `spell_list_id`;
 `creature_spells` gives casts and `castFlags`; `creature_ai_events` gives summons and phases. A
 `spell_list_id` of 0 means the spells live in the script, not the table. The world database
 disagrees with every online guide often enough to matter, and the script disagrees with the
 database.
- **A boss with no script is invisible to a search of the scripts.** The trap that follows directly
 from the rule above, and it produced a wrong `wantsTremorTotem` for Molten Core. Magmadar is
 EventAI driven from a spell list, so grepping `src/scripts/**/molten_core` for a fear returns
 nothing and the instance reads as fear-free -- while his Panic, an area fear on a thirty second
 repeat, is sitting in `creature_spells`. Enumerate the instance's bosses from `creature_template`
 first, then account for each one, rather than reasoning from the set of files that happen to
 exist.
- **Resolve a spell by its effect, never by its name.** `spell_template.effectApplyAuraName1` of 7
 is a fear and 6 is a charm. "Panic" reads like flavour text and is a fear; "Dominate Mind" is a
 real charm that nothing in the source casts. Neither could be settled from the name.
- **A boss with no `DungeonTactics` entry is not ready to be attempted.** Author the entry first.
  Then a wipe is information rather than a restatement of what the table does not say yet.
- **Worked example, Golemagg.** `boss_golemagg.cpp` zeroes all incoming damage on a Core Rager below
  50 percent and heals it to full until Golemagg is dead, and a Rager taken more than 100 yards from
  him forces the boss into evade mode. A 39-bot attempt with no tactics entry would therefore have
  parked the raid on an immortal add, or reset the encounter, and both were readable from one pass
  over a 400-line script. That is the whole of this rule.

## Never read raw logs into context

The single largest avoidable cost in this project. A 39-bot fight at full combat logging is megabytes
of prose, and reading it is almost never what answers the question.

- **Aggregate on the Pi and return a verdict plus a handful of counts.** `grep -c`, `awk`, `sort |
  uniq -c` and a small summary script beat `tail -200` every time.
- When detail is genuinely needed, grep for **one** event kind and take the first and last few lines,
  never the middle.
- This is why the aggregator in *Phase 6c* of `docs/bot-raid-encounter-awareness.md` is scheduled
  ahead of the structured event channel: it can run against the `[BotCombat]` prose that already
  exists, and it is the cost control rather than a convenience.

## A missing log line is not a missing event

Two ways the log lies by omission, both hit in one session.

- **Most bot telemetry is throttled per bot.** `Player::ShouldLogPullBlock` allows one refusal line
 per bot per five seconds and is *shared across refusal kinds*, so a follow refusal suppresses
 every chase refusal behind it. Zero lines of a kind is therefore consistent with the event firing
 constantly. Before concluding a code path is cold, find its throttle and check whether a louder
 neighbour is eating the budget.
- **Archived logs are only a baseline if the telemetry existed when they were written.** A
 pre-change run showing zero of an event proves nothing if the event was added after it;
 `git log -S "<the log string>"` dates the telemetry, and only archives newer than that commit can
 be compared against. One run was nearly reported as a 23x regression on exactly this mistake.

When a stall or a refusal is ambiguous, **add the field that disambiguates it and rerun**, rather
than reasoning further from what is already printed. Two `uint32`s in an existing format string
cost one incremental build and turn the next run into an answer; another blind run costs far more
and usually returns the same ambiguity.

## Batch wide-header changes

Touching a header that is included everywhere -- `src/shared/Log.h`, `src/game/SharedDefines.h`,
`src/game/Objects/Unit.h` -- forces a near-total rebuild. An incremental build of two `.cpp` files on
the Pi is well under a minute; a full one is hours on that hardware.

**Never spend a full rebuild on a change that buys no observable behaviour yet.** Batch it with
something that needs the rebuild anyway.

## The host, the keys, the trees

- **Host:** `ssh rabb1t@10.0.1.7`, hostname `wow-pi`. SOAP binds to loopback, so the harness driver
  runs **on the Pi**, never on the workstation.
- **Keys are passphrase-protected and live in the macOS keychain.** The first action of any session
  that needs the Pi is `ssh-add --apple-load-keychain`. Without it every key fails with
  `Permission denied (publickey)`, which reads like revoked access rather than an unloaded agent and
  has cost a session's opening exchanges.
- **`~/vmangos` on the Pi is a working copy, not a mirror.** Its git HEAD runs behind the
  workstation's while its *file contents* equal the workstation's HEAD, because the historic loop
  edited or rsynced there and committed here. Before syncing, prove it rather than assuming: compare
  `git hash-object <file>` on the Pi against `git rev-parse HEAD:<file>` locally. Sync only the files
  a change actually touches; never rsync the whole tree over it.
- **`~/wq "SELECT ..."`** on the Pi queries the world database. Credentials are parsed out of
  `mangosd.conf` inside the script and never reach a transcript. `-v` gives vertical output.
- **`. ~/.vmangos-harness.env`** exports `VMANGOS_SOAP_USER` and `VMANGOS_SOAP_PASSWORD` for the
  harness suites. Nothing in the server reads those from a config file.

## Restarting and deploying

`contrib/harness/README.md` has the detail. The short form: build in `~/build-vmangos`, back up
`~/server/bin/mangosd`, then **stop the service, copy, and start it again** -- in that order.

`systemctl restart` with the copy done first does not work and fails in a way that reads like
success. The running process holds its own executable, so the copy dies with `Text file busy`
while every later command in the same block still reports `active`, and the restart then brings the
*old* binary back up. Prove the deploy with `md5sum` over the built and installed paths and check
that it reports one hash twice, rather than trusting `is-active`.

**Then prove SOAP bound, because `active` does not mean reachable.** Starting too soon after a stop
leaves the old process still holding port 7878, and mangosd logs
`ERROR: MaNGOSsoap: Couldn't bind to 127.0.0.1:7878`, **never retries**, and runs on as a healthy
`active` server that no harness can reach. The port then reads as free, which makes it look like
nothing was ever wrong. Wait for `ss -ltn | grep 7878` to show a listener before starting a suite;
a good start reaches it in about six seconds, so a wait that goes much past that means the bind
failed and the service needs restarting rather than more waiting.

**Never `kill` mangosd by hand.** The unit carries `Restart=on-failure`, so a manual kill races
systemd's own restart against whatever you start next and both fight over the SOAP port. Check
liveness with `systemctl show -p MainPID --value vmangos-mangosd` and by polling `.server info`, not
by grepping `Server.log` for a bind message -- that message matches previous runs and has reported a
dead server as ready.

## Shell style

Restated here because a tool that does not load the user-level rules still needs them.

- **No banner `echo` lines.** No `echo "=== doing the thing ==="`. Say it in the response text.
- **No em dashes** in shell commands or scripts. Use `--` for flags and `-` elsewhere.
- **Never blind-sleep to wait for a job.** Watch the thing instead, so the wait ends when the work
  does: `tail -n +1 -f --pid=$(pgrep -f '[t]est_name' | head -1) /tmp/job.log`. Bracket the first
  character of the pattern so `pgrep` cannot match its own command line. Fall back to `cat` when the
  job has already finished.
