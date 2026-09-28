# vmangos party-bot work

## Before testing a dungeon, read `contrib/harness/README.md`

Its last section, "Testing a dungeon end to end", is the accumulated guidance for driving an
instance from a script: the stage shape, which database tables to read before writing a tactic, and
the traps that have each cost a build and a run. Every dungeon gets this treatment in turn, so the
rules there are meant to be applied rather than rediscovered.

The two that account for most wasted runs: **scope creature searches to the pack** (the default 300
yards finds the same entry two floors away) and **re-issue the attack order every poll** rather than
latching it once. And when a stage stalls, read the bots' tick log before concluding they cannot do
the thing -- most stalls so far have been the test, not the server.
