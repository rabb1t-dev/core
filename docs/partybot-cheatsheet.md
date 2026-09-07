# `.partybot` cheat sheet

Commands for the PlayerBot party companions on this vmangos server. All of them require
`SEC_ADMINISTRATOR` and are typed into chat like any other GM command.

## The targeting rule that governs almost everything

Most of these commands read your current selection to decide their scope:

- **A bot selected** → the command applies to that bot alone.
- **Nothing selected, or yourself selected** → the command applies to every bot in your party.

`setrole`, `unequip` and `remove` are the exceptions: they always require a selected bot.
`attackstart`, `attackstop`, `pull`, `aoe` and `usegobject` read your selection as the
*subject* of the order (the mob or object), not as the bot to order.

## Spawning and removing

| Command | What it does |
| --- | --- |
| `.partybot add <class>` | Spawns a bot of that class at your level, in your party |
| `.partybot add <role>` | Spawns a random class suited to the role |
| `.partybot add <class> <role>` | Spawns that class in that role, e.g. `.partybot add shaman healer` |
| `.partybot add <class\|role> <level>` | Spawns at a specific level (GM accounts, or `PartyBot.SkipChecks = 1`) |
| `.partybot add <class\|role> <level> <spec>` | Forces a talent spec by name or entry |

The role argument accepts `tank`, `healer` (or `heal`), `meleedps` (or `melee`), `rangedps` (or
`ranged`, `caster`), and `dps`, which resolves to melee or ranged from the class. A class that
cannot fill the role is refused rather than spawned wrong, so `.partybot add mage healer` is an
error rather than a mage that never heals.

Role decides three separate things, and only two of them work at every level:

- **The AI**, always. The rotation, target selection and positioning all follow the role.
- **Gearing**, always. Stat weights are chosen from the class and role together.
- **Talents**, only where a template exists for that class, role and level. See below.
| `.partybot clone` | Spawns a bot copying the selected player's race and class |
| `.partybot load <name>` | Loads an existing character from the database as a bot |
| `.partybot remove` | Removes the selected bot |

Classes: `warrior`, `paladin` (Alliance only), `hunter`, `rogue`, `priest`, `shaman`
(Horde only), `mage`, `warlock`, `druid`.

Roles: `tank` (always a warrior), `healer` (random from priest, druid, and shaman or
paladin by faction), `dps` (random from warrior, hunter, rogue, mage, warlock).

The spec argument exists because a role can't express which of two builds sharing it you
want — `.partybot add mage fire-pve` is the only way to insist on one.

## The talent gap below level 60

`player_premade_spell_template` holds the talent builds, and **every template below level 60 is a
damage build**. There are twink specs at 19, 29, 39 and 49, and all of them are dps; no class has a
tank or healer template below 60.

This cannot be worked around by reaching for the level 60 build, because
`ApplyPremadeSpecTemplateToPlayer` drags a character *up* to an unordered template's level: giving a
level 20 bot the level 60 resto build would make it a level 60 bot.

So a low level bot asked for tank or healer gets the AI and the gearing it asked for, and somebody
else's talents. The mismatch is written to the server log when it happens, naming the class, the
level, the role wanted and the template used, rather than being left to be discovered from a fight
going badly. Closing it is data work: author the missing rows in
`player_premade_spell_template` and `player_premade_spell`.

## Roles

```
.partybot setrole tank | dps | meleedps | rangedps | healer
```

Requires a selected bot. `dps` picks melee or ranged automatically from the bot's class.
Changing role rebuilds the bot's spell data, so it takes effect immediately.

## Combat orders

| Command | What it does |
| --- | --- |
| `.partybot attackstart` | All bots attack your selected target |
| `.partybot attackstop` | All bots break off and stop attacking |
| `.partybot pull [ms]` | **Tanks** attack your target; **DPS** are paused so they don't follow in |
| `.partybot aoe` | Bots use area damage centred on your selected target |
| `.partybot caststart` | Allows spellcasting again |
| `.partybot caststop` | Forbids spellcasting and interrupts anything in progress |

`pull` is the command for controlled pulls: it sends only the tanks and holds the damage
dealers back for the duration, defaulting to 10 seconds.

## Target marks

```
.partybot focusmark <mark>     # bots prioritise this mark as a kill target
.partybot ccmark <mark>        # bots crowd-control this mark instead of killing it
.partybot clearmarks           # forget all mark assignments
```

Valid marks: `star`, `circle`, `diamond`, `triangle`, `moon`, `square`, `cross`, `skull`.

Marks accumulate — calling `focusmark` twice with different marks gives a kill order of
both, in the order you assigned them. `clearmarks` wipes focus and crowd-control
assignments together.

**Marks do not initiate combat.** A bot only consults its focus marks once it is already
in combat, or once the party leader has a target it is engaged with. Marking a skull on a
fresh pack and waiting will do nothing on its own — that is what `pull` and `attackstart`
are for. This is the usual reason a tank bot appears to work on the first pull and then
ignore every pull afterwards.

Raid icons themselves are set by the client, not by these commands. A macro per icon is
the practical way:

```
/script SetRaidTarget("target", 8)   -- skull
/script SetRaidTarget("target", 7)   -- cross
/script SetRaidTarget("target", 6)   -- square
/script SetRaidTarget("target", 5)   -- moon
/script SetRaidTarget("target", 4)   -- triangle
/script SetRaidTarget("target", 3)   -- diamond
/script SetRaidTarget("target", 2)   -- circle
/script SetRaidTarget("target", 1)   -- star
```

## Movement and utility

| Command | What it does |
| --- | --- |
| `.partybot cometome` | Bots move to your position |
| `.partybot usegobject` | Bots interact with your selected game object (in range only) |
| `.partybot pause [ms] [all]` | Freezes bots in place; defaults to 5 minutes |
| `.partybot unpause [all]` | Resumes them |
| `.partybot unequip <item link>` | Destroys an item on the selected bot |

`unequip` takes an item link or ID and destroys the item outright rather than moving it to
a bag, which is how you get a bot to stop using a piece of gear you don't want it in.

## Durations are milliseconds, not seconds

`pull` and `pause` both take their argument in **milliseconds**, while the confirmation
message they print divides by 1000 and calls the result "seconds". The message is only
accurate for the defaults:

- `.partybot pause` → 5 minutes, reported as "300 seconds". Correct.
- `.partybot pause 30` → 30 **milliseconds**, reported as "0 seconds". Almost certainly
  not what was intended.
- `.partybot pause 30000` → 30 seconds. This is the form to use.

Same applies to `.partybot pull 15000` for a 15 second hold on the damage dealers.

## Gearing configuration

Bots gear themselves from random items when they spawn. These keys live in
`mangosd.conf` and are the values currently running on this server:

| Key | Value | Meaning |
| --- | --- | --- |
| `PartyBot.AutoEquip` | `1` | Gear bots automatically on spawn |
| `PartyBot.RandomGearLevelDifference` | `10` | How far *below* the bot's level items may be |
| `PartyBot.RandomGearMaxLevelAbove` | `0` | How far *above* the bot's level items may be |
| `PartyBot.RandomGearUncommonChance` | `25` | Base chance of a green item, in percent |
| `PartyBot.RandomGearGreenRampStartLevel` | `15` | Below this level, greens stay at the base chance |
| `PartyBot.RandomGearGreenRampEndLevel` | `20` | By this level, gear is effectively all green |
| `PartyBot.RandomGearRareRampStartLevel` | `35` | Blues begin appearing at this level |
| `PartyBot.RandomGearRareMaxChance` | `20` | Blue chance at max level, in percent |

The two ramps make bot gear track character progression: mostly white with a few greens
in the teens, green by 20, and a minority of blues creeping in from the mid thirties.
Epics and greys are excluded entirely while levelling. If no item exists at the quality
rolled for a slot, the selection falls back down a quality band rather than leaving the
slot empty.

Other relevant keys:

| Key | Value | Meaning |
| --- | --- | --- |
| `PartyBot.MaxBots` | `0` | Cap on simultaneous party bots; `0` is unlimited |
| `PartyBot.SkipChecks` | `0` | Bypass the level/party requirement checks on `add` |
| `PartyBot.AutoRevive` | `0` | Whether bots resurrect themselves after dying |
| `PartyBot.DeathRecoveryTimeout` | `60` | Seconds before a dead bot gives up recovering |
