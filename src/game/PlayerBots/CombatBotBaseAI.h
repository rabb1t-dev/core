#ifndef MANGOS_COMBAT_BOT_BASE_H
#define MANGOS_COMBAT_BOT_BASE_H

#include "PlayerBotAI.h"
#include "SpellEntry.h"
#include "Player.h"

struct StatWeights;

struct PlayerPremadeSpecTemplate;

struct HealSpellCompare
{
    bool operator() (SpellEntry const* const lhs, SpellEntry const* const rhs) const
    {
        uint32 spell1dmg = 0;
        uint32 spell2dmg = 0;

        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (lhs->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    spell1dmg += lhs->EffectBasePoints[i];
                    break;
            }
        }
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (rhs->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    spell2dmg += rhs->EffectBasePoints[i];
                    break;
            }
        }

        return spell1dmg > spell2dmg;
    }
};

struct HealAuraCompare
{
    bool operator() (SpellEntry const* const lhs, SpellEntry const* const rhs) const
    {
        uint32 spell1dmg = 0;
        uint32 spell2dmg = 0;

        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (lhs->Effect[i])
            {
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    if (lhs->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                        spell1dmg += lhs->EffectBasePoints[i];
                    break;
            }
        }
        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (rhs->Effect[i])
            {
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    if (rhs->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                        spell2dmg += rhs->EffectBasePoints[i];
                    break;
            }
        }

        return spell1dmg > spell2dmg;
    }
};

// Mana bands a healer rations against, and how much it tightens up in each.
//
// A healer that treats every point of missing health as worth a cast spends its bar on the first
// forty seconds of a fight and is a spectator for the rest of it. Two captures of the Wailing
// Caverns escort show the shape of it: the shaman went from ninety six percent mana to one percent
// in seventy eight seconds and died with the boss at half health, and the priest spent 1327 mana in
// twenty six seconds, four casts of it into a hunter's pet and four more into an ally sitting above
// eighty percent health. Neither ran out because the incoming damage was unsurvivable. They ran out
// because nothing distinguished a tank about to die from a rogue that had taken one hit.
//
// So above the conserve band nothing changes and the thresholds each class asks for stand. Inside
// it the bot stops topping anybody off. Below the critical band it spends only on whoever is
// holding the mobs, because a live tank is the only thing keeping the rest of the group from being
// hit at all.
static constexpr float CB_HEAL_MANA_CONSERVE_PERCENT = 60.0f;
static constexpr float CB_HEAL_MANA_CRITICAL_PERCENT = 30.0f;
static constexpr float CB_HEAL_CONSERVE_CAP_PERCENT = 70.0f;
static constexpr float CB_HEAL_CRITICAL_CAP_PERCENT = 50.0f;

// The ceiling on any in-combat heal, whatever the caller asked for.
//
// This is the one that mattered most. Of 947 heals cast across a run, 555 landed on somebody
// between 80 and 89 percent health and another 175 on somebody between 90 and 100 - three
// quarters of every heal, and every one of them mostly overheal. Only 50 went to anybody below
// half. The callers were asking for 90, and one of them for 100, so a healer would open a fight
// by topping off a rogue that had taken a single hit and arrive at the part that mattered with
// nothing left. The mana bands above only start rationing once the bar is already half gone,
// which is far too late if the first half went on targets that were never in danger.
//
// A mob at these levels takes roughly a tenth of a tank's health per swing and a direct heal
// restores rather more than that, so a cast begun at three quarters health lands before the
// target is in any trouble. Above that there is nothing to heal.
static constexpr float CB_HEAL_COMBAT_CEILING_PERCENT = 78.0f;

// The tank is the exception, by a little: it is the one target taking damage continuously rather
// than in bursts, so the cast has to be started earlier to keep ahead of it at all.
static constexpr float CB_HEAL_TANK_CEILING_BONUS = 7.0f;

// Below this, speed beats efficiency. See HealInjuredTargetDirect.
static constexpr float CB_HEAL_EMERGENCY_PERCENT = 45.0f;

// Below this the shaman stops laying totems. See SummonShamanTotems.
static constexpr float CB_TOTEM_MANA_FLOOR = 25.0f;

// Dispelling in combat. The remaining-duration floor keeps a cast off a debuff that will expire
// before the cast has paid for itself, and the repeat window stops a mob that reapplies faster
// than the healer can clear from turning dispelling into a mana race.
static constexpr int32 CB_DISPEL_MIN_REMAINING_MS = 3000;
static constexpr time_t CB_DISPEL_REPEAT_SECONDS = 8;

// How often an armour slot gets a permanent enchant. Weapons and shields always do: those are the
// pieces anybody bothers with, because a weapon enchant scales everything the character does.
// Bracers and boots are what a real player enchants when the mats happen to be lying around.
static constexpr int32 CB_ARMOR_ENCHANT_CHANCE = 50;

// Racials, granted at character creation rather than trained, so they appear in no class spell
// list and have to be named. War Stomp is handled with the interrupts in PartyBotAI.
enum CombatBotRacials
{
    CB_SPELL_WILL_OF_THE_FORSAKEN = 7744,   // undead: breaks charm, fear and sleep
    CB_SPELL_BLOOD_FURY = 20572,            // orc: attack power, at the cost of healing received
    CB_SPELL_BERSERKING = 26297,            // troll: haste
};

// Rogue poisons. Off a quest chain rather than a trainer, so nothing that learns spells by level
// finds them and only some of the premade templates list them.
enum CombatBotPoisons
{
    CB_SPELL_POISONS_SKILL = 2842,          // grants the Poisons trade skill
    CB_SPELL_INSTANT_POISON = 8681,         // level 20, makes item 6947
    CB_SPELL_DEADLY_POISON = 2835,          // level 30, makes item 2892
    CB_SPELL_WOUND_POISON = 13220,          // level 32, makes item 10918

    CB_ITEM_INSTANT_POISON = 6947,
    CB_ITEM_DEADLY_POISON = 2892,
    CB_ITEM_WOUND_POISON = 10918,

    // Blind's reagent. The spell is useless without it, and the bot has no way to buy one.
    CB_SPELL_BLIND = 2094,
    CB_ITEM_BLINDING_POWDER = 5530,
};

static constexpr uint32 CB_BLINDING_POWDER_STACK = 10;
static constexpr uint32 CB_BLIND_MIN_LEVEL = 26;

static constexpr uint32 CB_POISON_MIN_LEVEL = 20;
static constexpr uint32 CB_POISON_STACK_SIZE = 10;

// Potions. Five is a stack, and at one per two minute cooldown that is more fights than a bot
// lives through between respawns.
static constexpr uint32 CB_POTION_STACK_SIZE = 5;

// How far below the best damage per second on offer a weapon may sit and still be treated as an
// equal, so that its quality decides instead. Inside a five level item band this is the whole
// difference between a green and a blue: the damage is set by item level and is nearly identical,
// and the stats are not.
static constexpr float CB_WEAPON_QUALITY_DPS_TOLERANCE = 0.06f;

// When a potion is worth the cooldown. The health figure is low because a potion is a poor heal
// and a good panic button, and the mana figure is high because a healer that waits until it is
// empty has already missed the casts the potion was going to pay for -- measured against the
// Antu'sul run where the priest crossed thirty five percent at fourteen twenty six and was at
// zero by fourteen forty three, seventeen seconds later.
static constexpr float CB_POTION_HEALTH_PERCENT = 30.0f;
static constexpr float CB_POTION_MANA_PERCENT = 35.0f;

// Below this a healer's own health bar outranks its mana bar, because it can no longer heal
// itself out of trouble anyway.
static constexpr float CB_POTION_HEALER_MANA_FLOOR = 10.0f;

// How many of the best weapons in a level band a bot picks between. One would mean every warrior
// of a level carrying the same axe; the top handful means they all carry a good one.
static constexpr uint32 CB_WEAPON_TOP_CHOICES = 4;

// Speculative healing: starting a cast before anybody needs it, so that the heal lands at the
// moment somebody does.
//
// This is what separates a healer that keeps a group alive from one that is merely never idle. A
// direct heal at these levels takes two and a half seconds, and a mob swings every two, so a
// healer that waits for the health bar to move is always one swing behind: it starts casting when
// the tank is at half and the heal lands after the hit that would have killed it. A raid healer
// solves this by keeping a cast running at all times against nothing in particular, and throwing
// it away if the damage never arrives.
//
// It is free to throw away. Spell::TakePower runs from Spell::cast, at the end of the cast, so a
// cancelled cast has cost no mana at all - only the global cooldown, which has already elapsed by
// the time the decision is made.
//
// Only begun with mana to spare: speculating is a bet, and a healer down to its last third should
// not be betting.
static constexpr float CB_HEAL_PRECAST_MIN_MANA_PERCENT = 55.0f;

// How much health a target has to be missing before a cast is begun on spec. Not zero: a group at
// full health after a fight has ended does not need a healer winding up at it.
static constexpr float CB_HEAL_PRECAST_TRIGGER_PERCENT = 96.0f;

// How close to landing the cast gets before the keep-or-cancel decision is made. The whole value
// is in the cast already being most of the way through when the damage arrives, so this wants to
// be short - long enough to act on, short enough that the decision uses near-current health.
static constexpr uint32 CB_HEAL_PRECAST_COMMIT_WINDOW_MS = 500;

// How much health a tank is treated as being down by when ranking heal targets, so an equally hurt
// damage dealer does not outrank it. Picked to be worth roughly one hit at the levels these bots
// run dungeons at: enough to break the tie, not enough to ignore somebody genuinely dying.
static constexpr float CB_HEAL_TANK_PRIORITY_BONUS = 15.0f;

// The same, for an escort the instance has asked the group to keep alive, and more of it than a
// tank gets. A tank at forty percent is a fight going badly and the group has answers to it; an
// escort at forty percent is the run ending, because nobody can taunt for it, it does not heal
// itself, and in several of these events it is forbidden from defending itself at all. Belnistrasz
// spends four minutes channelling with SetCombatMovement(false) and an AttackStart that returns
// without doing anything, taking four waves of adds on a health pool of a level 36 caster.
//
// The ceiling bonus is what gets a heal started on him at all: a rotation that only heals below
// sixty percent will not touch an escort losing a quarter of its bar a wave until it is nearly
// gone, by which point the cast does not land in time.
static constexpr float CB_HEAL_ESCORT_PRIORITY_BONUS = 25.0f;
static constexpr float CB_HEAL_ESCORT_CEILING_BONUS = 15.0f;

class CombatBotBaseAI : public PlayerBotAI
{
public:

    CombatBotBaseAI() : PlayerBotAI(nullptr)
    {
        for (auto& ptr : m_spells.raw.spells)
            ptr = nullptr;
    }

    virtual void OnPacketReceived(WorldPacket const* packet) override;
    // Whether this AI answers group loot rolls on its own schedule. False here, so a headless
    // session that has no opinion about loot still votes and never leaves a roll waiting out its
    // timer; PartyBotAI says true, because it has an evaluator and wants to be asked.
    virtual bool AnswersLootRollsItself() const { return false; }
    void SendBattlefieldPortPacket();
    void SendBattlemasterJoinPacket(uint8 battlegroundId);
    void SendAreaTriggerPacket(uint32 areaTriggerId);
    void ActivateNearbyAreaTrigger();

    // A named slot paired with whatever population put in it, for .harness spells.
    struct SpellSlot
    {
        char const* name;
        SpellEntry const* spell;
    };

    void AutoAssignRole();
    void PopulateSpellData();
    std::vector<SpellSlot> GetSpellSlots() const;
    void ResetSpellData();
    void AddAllSpellReagents();
    void SummonPetIfNeeded();
    void LearnArmorProficiencies();
    void LearnWeaponProficiencies();

    // What the generated-bot init branch does minus everything that touches gear, so that a
    // roster member loaded from the database is the level sixty its level column claims
    // rather than a level one wearing level sixty gear.
    void MakeCharacterCurrentForLevel();

    void LearnPremadeSpecForClass();
    PlayerPremadeSpecTemplate const* FindPremadeSpecByName(std::string const& name) const;
    PlayerPremadeSpecTemplate const* SelectPremadeSpecTemplate() const;
    void EquipPremadeGearTemplate();
    void EquipRandomGearInEmptySlots();
    ItemPrototype const* SelectWeaponForSlot(std::vector<ItemPrototype const*> const& candidates,
                                             uint8 slot) const;
    void AutoEquipGear(uint32 option);
    void LearnClassSpellsForLevel();
    void LearnRandomTalents();

    // Permanent enchants on everything worn that has a slot for one, and the hour-long
    // consumables a real group turns up carrying. See BotProvisions.h for what and why.
    void ApplyProvisionEnchants();
    void StockProvisionConsumables();
    void LearnRoguePoisons();

    // Drink a health or mana potion mid fight. True when one went down, so the caller spends the
    // tick on it.
    bool TryUseRestorePotion();
    bool UseProvisionConsumables();

    // Starting a heal before anybody needs it, and throwing it away if nobody comes to need it.
    bool BeginSpeculativeHeal();
    bool ReconsiderHealInFlight();
    
    uint8 GetAttackersInRangeCount(float range) const;
    Unit* SelectAttackerDifferentFrom(Unit const* pExcept) const;
    Unit* SelectHealTarget(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f) const;
    Unit* SelectPeriodicHealTarget(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f) const;

    // The escort this instance asks the group to keep alive, when one is near enough to be the
    // group's problem. Null for every bot that is not a party bot on a map with an escort tactic.
    //
    // Asked for here rather than reached for directly because the heal selectors live in this
    // class, shared with the battleground bots, and the tactics table is a party bot's business.
    // The selectors need only one thing from it: a unit that is not in the group and has to live.
    virtual Unit* GetGuardedEscort() const { return nullptr; }
    // Whether another group member is already putting this buff on this subgroup, and the order
    // this bot should walk the subgroups in. Together they stop every buffer in a raid choosing
    // the same target on the same tick, which at a raid buff's mana cost is most of a bar.
    bool IsBuffAlreadyIncoming(Player const* pTarget, SpellEntry const* pSpellEntry) const;
    uint8 GetBuffSubGroupOrder(uint8 subGroup) const;

    Player* SelectBuffTarget(SpellEntry const* pSpellEntry) const;
    Player* SelectBuffTarget(SpellEntry const* pSingleSpellEntry, SpellEntry const* pGroupSpellEntry, SpellEntry const*& pSelectedSpellEntry) const;
    Player* SelectDispelTarget(SpellEntry const* pSpellEntry) const;
    bool IsWorthDispelling(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool IsValidBuffTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool IsValidHealTarget(Unit const* pTarget, float healthPercent = 100.0f) const;
    float GetMaxHealSpellRange() const;
    float GetManaAdjustedHealPercent(float requestedPercent) const;
    bool IsRationingHealsForTank() const;
    bool IsAlreadyHealing(ObjectGuid guid) const;
    bool IsAttackableHostileTarget(Unit const* pTarget) const;
    bool IsProtectedByCrowdControl(Unit const* pTarget) const;
    bool IsValidHostileTarget(Unit const* pTarget) const;
    bool IsValidDispelTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool FindAndPreHealTarget();
    bool FindAndHealInjuredAlly(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f);
    bool HealInjuredTarget(Unit* pTarget);
    bool HealInjuredTargetDirect(Unit* pTarget);
    bool HealInjuredTargetPeriodic(Unit* pTarget);
    template <class T>
    SpellEntry const* SelectMostEfficientHealingSpell(Unit const* pTarget, std::set<SpellEntry const*, T>& spellList) const;
    template <class T>
    SpellEntry const* SelectMostEfficientHealingSpell(Unit const* pTarget, int32 missingHealth, std::set<SpellEntry const*, T>& spellList) const;
    template <class T>
    SpellEntry const* SelectFastestHealingSpell(Unit const* pTarget, std::set<SpellEntry const*, T>& spellList) const;
    int32 GetIncomingdamage(Unit const* pTarget) const;
    bool AreOthersOnSameTarget(ObjectGuid guid, bool checkMelee = true, bool checkSpells = true) const;

    SpellCastResult DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry);
    virtual bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    // Which of CanTryToCastSpell's gates refused, for the log. A heal declined inside
    // SelectMostEfficientHealingSpell never reaches DoCastSpell and so leaves no trace at
    // all, which is how a healer standing over a dying tank reads as nothing happening.
    char const* DescribeCastRefusal(Unit const* pTarget, SpellEntry const* pSpellEntry) const;

    // Whether this bot's combat decisions are being recorded, gated on the runtime switch.
    bool IsCombatLogged() const;
    void LogCombatCast(Unit const* pTarget, SpellEntry const* pSpellEntry, SpellCastResult result) const;
    static char const* GetRoleName(CombatBotRoles role);
    bool IsWearingShield(Player* pPlayer) const;
    bool IsInDuel() const;
    CombatBotRoles GetRole() const;

    void EquipOrUseNewItem();
    StatWeights const* GetStatWeights() const;
    static char const* GetDefaultSpecNameForRole(uint8 classId, CombatBotRoles role);

    // An item has arrived, from a trade, a corpse or a won roll. Only noted here: the wearing of it
    // is done out of combat, because re-solving the whole loadout mid-fight costs a tick the bot
    // owes to the fight and can swap the weapon out from under a swing.
    void OnReceivedItem(Item const* /*pItem*/) override { m_equipCheckPending = true; }

    bool AddItemToInventory(uint32 itemId, uint32 count = 1);
    void AddHunterAmmo();
    uint8 GetHighestHonorRankFromEquippedItems() const;
    void UpdateVisualHonorRankBasedOnItems();
    void BeginChasing(Unit* pVictim) const;

    // The closest a ranged bot or a healer may stand to this particular creature, whatever the
    // ordinary standoff would have been. Zero, which is what every bot without instance tactics
    // answers, leaves the standoff alone.
    //
    // Declared here because BeginChasing is where every standoff in the game is chosen and it
    // lives on this class, and answered here as "no opinion" so that a battleground bot, which
    // has no dungeon to have tactics for, is unaffected.
    virtual float GetTacticalStandoff(Unit const* /*pTarget*/) const { return 0.0f; }

    // Whether the bot is standing on ground its instance says to hold, and this destination would
    // take it off. False everywhere there is no such ground, which is almost everywhere.
    //
    // A hook for the same reason GetTacticalStandoff is one: the tactics table is a party bot's
    // business and these movement helpers are shared with the battleground bots.
    virtual bool WouldLeaveHeldGround(float /*x*/, float /*y*/, float /*z*/) const { return false; }

    // Creature entries this bot will close on even though waking them is a pull, because the
    // instance it is standing in says they have to die. Nothing by default; see PartyBotAI.
    virtual std::vector<uint32> const* GetApproachAnywayEntries() const { return nullptr; }

    // Where melee fight this particular creature, when its instance says the spot matters.
    // False for everything without an entry, which leaves the chase to decide as it always has.
    virtual bool GetFightAnchor(Unit const* /*pVictim*/, float& /*x*/, float& /*y*/, float& /*z*/,
                                float& /*radius*/) const { return false; }
    bool WouldPositionPullExtraEnemies(float x, float y, float z, float extraMargin = 0.0f) const;

    // The route, not just where it ends. A destination clear of every aggro radius is no use if
    // getting there crosses one, which is how a bot walks through a pack to stand safely past it.
    bool WouldPathPullExtraEnemies(float x, float y, float z) const;

    // The same two questions with the policy taken out: purely whether the geometry wakes
    // something, with no regard for whether the bot is under orders.
    //
    // The pair above answer "should I decline this move", and under orders the answer is always
    // no, because declining an ordered move is refusing the order while appearing to accept it.
    // These answer "would this route pull", which is still worth knowing under orders -- not to
    // refuse with, but to steer with. A bot told to kill something should still walk around the
    // camp between it and the target rather than through it.
    bool PositionWouldAggroUnengaged(float x, float y, float z, float extraMargin = 0.0f) const;
    bool PathWouldAggroUnengaged(float x, float y, float z) const;

    // A way to somewhere that the direct line cannot reach safely: the same destination approached
    // off a bearing, the way a player steers a few degrees wide of a camp rather than stopping.
    // False when nothing within the search found a clear route.
    // Whether the bot could actually walk to a spot. Asked of the pathfinder, because line of
    // sight was standing in for this and a point across a railing is visible and unreachable.
    bool CanWalkTo(float x, float y, float z) const;

    bool FindSafeDetour(float destX, float destY, float destZ,
                        float& outX, float& outY, float& outZ) const;

    // Somewhere out of one creature's sight, reachable, and no further off than maxDistance, which
    // is how far the caller has worked out the bot has time to walk. Purely geometric: the decision
    // to move at all is the caller's.
    bool FindBreakSightSpot(Unit const* pWatcher, float maxDistance,
                            float& outX, float& outY, float& outZ) const;

    // Somewhere this target can be seen from, which is the mirror of the search above and the
    // thing a ranged bot actually wants. Within maxRange of the target and no further than
    // maxTravel from where the bot stands, reachable, in sight of the target, and not across
    // anything else's aggro radius. Purely geometric, like its mirror: whether to move is the
    // caller's decision.
    bool FindFiringPosition(Unit const* pTarget, float minRange, float maxRange, float maxTravel,
                            float& outX, float& outY, float& outZ) const;

    // Whether this spot can see that unit, asked from a position the bot is not standing in yet.
    bool PositionSeesTarget(float x, float y, float z, Unit const* pTarget) const;

    // Somewhere clear of every unengaged creature's aggro radius, reachable, and no further than
    // maxTravel from where the bot stands. Bearings taken from directly away from pAwayFrom, which
    // is the creature the caller is standing too close to, so the first answer found is the
    // shortest retreat rather than a walk round it. Purely geometric: whether to move is the
    // caller's decision.
    bool FindSpotClearOfUnengaged(Unit const* pAwayFrom, float maxTravel,
                                  float& outX, float& outY, float& outZ) const;

    // The same search against a patch of ground rather than a creature: somewhere at least
    // clearRadius from (px, py), reachable, no further than maxTravel, and clear of everything
    // unengaged. Bearings from directly away from the point, so the first answer is the shortest
    // step out.
    //
    // A separate entry point rather than a Unit overload because the thing being escaped here has
    // no Unit to pass. A persistent area aura is a DynamicObject on the floor, and the creature
    // that laid it down is usually either somewhere else by now or dead -- Maraudon's Noxious
    // Slime casts its cloud as it dies, so the only thing left to measure from is the patch
    // itself.
    // `keepWithin`, when non-zero, also caps how far from the point the answer may be. A bot
    // stepping out of a five yard cloud does not care; a caster walking out to a twenty five yard
    // standoff does, because its own spells reach thirty and a spot at forty is a spot where it
    // has stopped fighting.
    bool FindSpotClearOfPoint(float px, float py, float clearRadius, float maxTravel,
                              float& outX, float& outY, float& outZ,
                              float keepWithin = 0.0f) const;

    // Whether landing this spell would drag something in that nobody is fighting.
    //
    // The movement rules cannot see this one. They ask where the bot's feet are, and an area
    // effect pulls what stands inside the spell's radius whatever the bot is standing in - so a
    // tank's Consecration wakes the next pack from a position every other rule approves of.
    bool WouldSpellPullExtraEnemies(Unit const* pTarget, SpellEntry const* pSpellEntry) const;

    bool WouldFearPullExtraEnemies() const;
    bool SummonShamanTotems();
    bool IsBreakableCrowdControlInRange(float radius, Unit const* pAround = nullptr) const;

    // How much further from the target than the tank a bot has to be before it stops waiting. A
    // body length or so: enough that two bots walking in together do not both read as ahead, small
    // enough that it never holds a bot that is genuinely behind.
    static constexpr float CB_TANK_LEAD_MARGIN = 3.0f;

    // And how long it is willing to wait before going anyway.
    static constexpr uint32 CB_TANK_LEAD_MAX_HOLD_MS = 10000;

    // When this bot started waiting for the tank to get ahead of it, or zero when it is not.
    // Mutable because BeginChasing is const and every caller expects it to stay that way.
    mutable uint32 m_tankLeadHoldSince = 0;

    bool IsAheadOfTankOnPull(Unit const* pVictim) const;

    // Who a totem dropped right now would actually reach.
    struct TotemAudience
    {
        uint32 members = 0;
        uint32 melee = 0;
        uint32 manaUsers = 0;
    };
    CombatBotRoles GetEffectiveRole(Player const* pTarget) const;

    // The one tank the group is relying on, so that a raid with four of them still has one
    // threat list to hold rather than four tanks trading a mob between themselves.
    Player* GetGroupMainTank() const;

    // Healing already in flight at this unit from anywhere in the group, this bot included.
    int32 GetIncomingHeals(Unit const* pTarget) const;
    TotemAudience SurveyTotemAudience() const;
    SpellEntry const* SelectTotemForSlot(TotemSlot slot) const;
    bool IsTotemSpellCoveredByAnotherShaman(TotemSlot slot, SpellEntry const* pSpellEntry) const;
    SpellEntry const* SelectBlessingForTarget(Player const* pTarget) const;
    Player* SelectBlessingTarget(SpellEntry const*& pSelectedSpellEntry) const;
    SpellCastResult CastWeaponBuff(SpellEntry const* pSpellEntry, EquipmentSlots slot);
    bool UseTrinketEffects(bool onlyToBreakCC = false);
    bool UseItemEffect(Item* pItem, bool onlyToBreakCC = false);
    void BreakCrowdControlEffects();
    bool HasCrowdControlOfMechanic(std::initializer_list<uint32> mechanics) const;
    bool UseOffensiveRacial();

    virtual void UpdateInCombatAI() = 0;
    virtual void UpdateOutOfCombatAI() = 0;
    virtual void UpdateInCombatAI_Paladin() = 0;
    virtual void UpdateOutOfCombatAI_Paladin() = 0;
    virtual void UpdateInCombatAI_Shaman() = 0;
    virtual void UpdateOutOfCombatAI_Shaman() = 0;
    virtual void UpdateInCombatAI_Hunter() = 0;
    virtual void UpdateOutOfCombatAI_Hunter() = 0;
    virtual void UpdateInCombatAI_Mage() = 0;
    virtual void UpdateOutOfCombatAI_Mage() = 0;
    virtual void UpdateInCombatAI_Priest() = 0;
    virtual void UpdateOutOfCombatAI_Priest() = 0;
    virtual void UpdateInCombatAI_Warlock() = 0;
    virtual void UpdateOutOfCombatAI_Warlock() = 0;
    virtual void UpdateInCombatAI_Warrior() = 0;
    virtual void UpdateOutOfCombatAI_Warrior() = 0;
    virtual void UpdateInCombatAI_Rogue() = 0;
    virtual void UpdateOutOfCombatAI_Rogue() = 0;
    virtual void UpdateInCombatAI_Druid() = 0;
    virtual void UpdateOutOfCombatAI_Druid() = 0;

    static bool IsPhysicalDamageClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_HUNTER:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsRangedDamageClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_HUNTER:
            case CLASS_PRIEST:
            case CLASS_SHAMAN:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsMeleeDamageClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsMeleeWeaponClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_ROGUE:
            case CLASS_SHAMAN:
                return true;
        }
        return false;
    }
    static bool IsShieldClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_SHAMAN:
                return true;
        }
        return false;
    }
    static bool IsTankClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsHealerClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_PALADIN:
            case CLASS_PRIEST:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }
    static bool IsStealthClass(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_ROGUE:
            case CLASS_DRUID:
                return true;
        }
        return false;
    }

    SpellEntry const* GetCrowdControlSpell() const
    {
        switch (me->GetClass())
        {
            case CLASS_PALADIN:
                return m_spells.paladin.pHammerOfJustice;
            case CLASS_MAGE:
                return m_spells.mage.pPolymorph;
            case CLASS_PRIEST:
                return m_spells.priest.pShackleUndead;
            case CLASS_WARLOCK:
                return m_spells.warlock.pBanish;
            case CLASS_ROGUE:
                return m_spells.rogue.pBlind;
            // No hunter entry, deliberately. Freezing Trap is the only control in the game with
            // no creature type restriction, and it is flagged
            // SPELL_ATTR_NOT_IN_COMBAT_ONLY_PEACEFUL -- it cannot be cast during the fight it is
            // wanted in. Feign Death was tried as a way round that and is not one: it takes the
            // hunter out of the fight, needs the trap off cooldown, needs the feign to hold, and
            // needs the add to walk onto it. Built, measured, and removed -- one attempt had the
            // hunter lie down for sixty nine seconds and never lay the trap at all.
            case CLASS_DRUID:
                return m_spells.druid.pHibernate;
        }
        return nullptr;
    }

    SpellEntry const* m_resurrectionSpell = nullptr;

    // Everything the bot could put in a totem slot, and every blessing it knows. These are held
    // outside m_spells because neither choice can be made when the spell is learned. The right
    // totem depends on who is standing in range and which schools the other shamans already
    // cover, and the right blessing depends on the target rather than on the paladin, so both are
    // decided at cast time from what is recorded here.
    struct
    {
        SpellEntry const* pWindfury;
        SpellEntry const* pGraceOfAir;
        SpellEntry const* pNatureResistance;
        SpellEntry const* pWindwall;
        SpellEntry const* pTranquilAir;
        SpellEntry const* pStrengthOfEarth;
        SpellEntry const* pStoneskin;
        SpellEntry const* pStoneclaw;
        SpellEntry const* pTremor;
        SpellEntry const* pEarthbind;
        SpellEntry const* pSearing;
        SpellEntry const* pMagma;
        SpellEntry const* pFireNova;
        SpellEntry const* pFlametongue;
        SpellEntry const* pFrostResistance;
        SpellEntry const* pManaSpring;
        SpellEntry const* pHealingStream;
        SpellEntry const* pPoisonCleansing;
        SpellEntry const* pDiseaseCleansing;
        SpellEntry const* pFireResistance;
    } m_totems = {};
    struct
    {
        SpellEntry const* pMight;
        SpellEntry const* pWisdom;
        SpellEntry const* pKings;
        SpellEntry const* pSanctuary;
        SpellEntry const* pLight;
    } m_blessings = {};

    std::vector<SpellEntry const*> m_spellListTaunt;
    std::set<SpellEntry const*, HealAuraCompare> m_spellListPeriodicHeal;
    std::set<SpellEntry const*, HealSpellCompare> m_spellListDirectHeal;
    union
    {
        struct
        {
            SpellEntry const* spells[45];
        } raw;
        struct
        {
            SpellEntry const* pAura;
            SpellEntry const* pSeal;
            SpellEntry const* pBlessingBuff;
            SpellEntry const* pBlessingOfProtection;
            SpellEntry const* pBlessingOfFreedom;
            SpellEntry const* pBlessingOfSacrifice;
            SpellEntry const* pHammerOfJustice;
            SpellEntry const* pJudgement;
            SpellEntry const* pExorcism;
            SpellEntry const* pConsecration;
            SpellEntry const* pHammerOfWrath;
            SpellEntry const* pCleanse;
            SpellEntry const* pDivineShield;
            SpellEntry const* pLayOnHands;
            SpellEntry const* pRighteousFury;
            SpellEntry const* pHolyShock;
            SpellEntry const* pDivineFavor;
            SpellEntry const* pHolyWrath;
            SpellEntry const* pTurnEvil;
            SpellEntry const* pHolyShield;
        } paladin;
        struct
        {
            SpellEntry const* pLightningBolt;
            SpellEntry const* pChainLightning;
            SpellEntry const* pEarthShock;
            SpellEntry const* pFlameShock;
            SpellEntry const* pFrostShock;
            SpellEntry const* pPurge;
            SpellEntry const* pStormstrike;
            SpellEntry const* pElementalMastery;
            SpellEntry const* pLightningShield;
            SpellEntry const* pGhostWolf;
            SpellEntry const* pCureDisease;
            SpellEntry const* pCurePoison;
            SpellEntry const* pAirTotem;
            SpellEntry const* pEarthTotem;
            SpellEntry const* pFireTotem;
            SpellEntry const* pWaterTotem;
            SpellEntry const* pManaTideTotem;
            SpellEntry const* pWeaponBuff;
        } shaman;
        struct
        {
            SpellEntry const* pAspectOfTheCheetah;
            SpellEntry const* pAspectOfTheMonkey;
            SpellEntry const* pAspectOfTheHawk;
            SpellEntry const* pSerpentSting;
            SpellEntry const* pArcaneShot;
            SpellEntry const* pAimedShot;
            SpellEntry const* pMultiShot;
            SpellEntry const* pConcussiveShot;
            SpellEntry const* pWingClip;
            SpellEntry const* pHuntersMark;
            SpellEntry const* pMongooseBite;
            SpellEntry const* pRaptorStrike;
            SpellEntry const* pDisengage;
            SpellEntry const* pFeignDeath;
            SpellEntry const* pFreezingTrap;
            SpellEntry const* pScareBeast;
            SpellEntry const* pVolley;
        } hunter;
        struct
        {
            SpellEntry const* pIceArmor;
            SpellEntry const* pArcaneIntellect;
            SpellEntry const* pArcaneBrilliance;
            SpellEntry const* pIceBarrier;
            SpellEntry const* pManaShield;
            SpellEntry const* pPolymorph;
            SpellEntry const* pFrostbolt;
            SpellEntry const* pFireBlast;
            SpellEntry const* pFireball;
            SpellEntry const* pArcaneExplosion;
            SpellEntry const* pFrostNova;
            SpellEntry const* pConeofCold;
            SpellEntry const* pBlink;
            SpellEntry const* pCounterspell;
            SpellEntry const* pPresenceOfMind;
            SpellEntry const* pArcanePower;
            SpellEntry const* pRemoveLesserCurse;
            SpellEntry const* pScorch;
            SpellEntry const* pPyroblast;
            SpellEntry const* pEvocation;
            SpellEntry const* pIceBlock;
            SpellEntry const* pBlizzard;
            SpellEntry const* pBlastWave;
            SpellEntry const* pCombustion;
        } mage;
        struct
        {
            SpellEntry const* pPowerWordFortitude;
            SpellEntry const* pDivineSpirit;
            SpellEntry const* pPrayerofSpirit;
            SpellEntry const* pPrayerofFortitude;
            SpellEntry const* pPrayerofShadowProtection;
            SpellEntry const* pInnerFire;
            SpellEntry const* pShadowProtection;
            SpellEntry const* pPowerWordShield;
            SpellEntry const* pHolyNova;
            SpellEntry const* pHolyFire;
            SpellEntry const* pMindBlast;
            SpellEntry const* pMindFlay;
            SpellEntry const* pShadowWordPain;
            SpellEntry const* pInnerFocus;
            SpellEntry const* pAbolishDisease;
            SpellEntry const* pDispelMagic;
            SpellEntry const* pManaBurn;
            SpellEntry const* pDevouringPlague;
            SpellEntry const* pPsychicScream;
            SpellEntry const* pShadowform;
            SpellEntry const* pVampiricEmbrace;
            SpellEntry const* pSilence;
            SpellEntry const* pFade;
            SpellEntry const* pShackleUndead;
            SpellEntry const* pSmite;
        } priest;
        struct
        {
            SpellEntry const* pDemonArmor;
            SpellEntry const* pDeathCoil;
            SpellEntry const* pDetectInvisibility;
            SpellEntry const* pShadowWard;
            SpellEntry const* pShadowBolt;
            SpellEntry const* pCorruption;
            SpellEntry const* pConflagrate;
            SpellEntry const* pShadowburn;
            SpellEntry const* pSearingPain;
            SpellEntry const* pImmolate;
            SpellEntry const* pRainOfFire;
            SpellEntry const* pDemonicSacrifice;
            SpellEntry const* pDrainLife;
            SpellEntry const* pSiphonLife;
            SpellEntry const* pBanish;
            SpellEntry const* pFear;
            SpellEntry const* pHowlofTerror;
            SpellEntry const* pCurseofAgony;
            SpellEntry const* pCurseofDoom;
            SpellEntry const* pCurseoftheElements;
            SpellEntry const* pCurseofShadow;
            SpellEntry const* pCurseofRecklessness;
            SpellEntry const* pCurseofTongues;
            SpellEntry const* pCurseofExhaustion;
            SpellEntry const* pLifeTap;
        } warlock;
        struct
        {
            SpellEntry const* pBattleStance;
            SpellEntry const* pBerserkerStance;
            SpellEntry const* pDefensiveStance;
            SpellEntry const* pCharge;
            SpellEntry const* pIntercept;
            SpellEntry const* pOverpower;
            SpellEntry const* pHeroicStrike;
            SpellEntry const* pCleave;
            SpellEntry const* pExecute;
            SpellEntry const* pMortalStrike;
            SpellEntry const* pBloodthirst;
            SpellEntry const* pBloodrage;
            SpellEntry const* pBerserkerRage;
            SpellEntry const* pRecklessness;
            SpellEntry const* pRetaliation;
            SpellEntry const* pDeathWish;
            SpellEntry const* pIntimidatingShout;
            SpellEntry const* pPummel;
            SpellEntry const* pRend;
            SpellEntry const* pDisarm;
            SpellEntry const* pWhirlwind;
            SpellEntry const* pBattleShout;
            SpellEntry const* pDemoralizingShout;
            SpellEntry const* pHamstring;
            SpellEntry const* pThunderClap;
            SpellEntry const* pSweepingStrikes;
            SpellEntry const* pLastStand;
            SpellEntry const* pShieldBlock;
            SpellEntry const* pShieldWall;
            SpellEntry const* pShieldBash;
            SpellEntry const* pShieldSlam;
            SpellEntry const* pSunderArmor;
            SpellEntry const* pRevenge;
            SpellEntry const* pTaunt;
            SpellEntry const* pConcussionBlow;
            SpellEntry const* pPiercingHowl;
        } warrior;
        struct
        {
            SpellEntry const* pSliceAndDice;
            SpellEntry const* pSinisterStrike;
            SpellEntry const* pAdrenalineRush;
            SpellEntry const* pEviscerate;
            SpellEntry const* pStealth;
            SpellEntry const* pGarrote;
            SpellEntry const* pAmbush;
            SpellEntry const* pCheapShot;
            SpellEntry const* pPremeditation;
            SpellEntry const* pBackstab;
            SpellEntry const* pHemorrhage;
            SpellEntry const* pGhostlyStrike;
            SpellEntry const* pGouge;
            SpellEntry const* pRupture;
            SpellEntry const* pExposeArmor;
            SpellEntry const* pKidneyShot;
            SpellEntry const* pColdBlood;
            SpellEntry const* pBladeFlurry;
            SpellEntry const* pVanish;
            SpellEntry const* pBlind;
            SpellEntry const* pPreparation;
            SpellEntry const* pEvasion;
            SpellEntry const* pRiposte;
            SpellEntry const* pKick;
            SpellEntry const* pSprint;
            SpellEntry const* pMainHandPoison;
            SpellEntry const* pOffHandPoison;
        } rogue;
        struct
        {
            SpellEntry const* pBearForm;
            SpellEntry const* pCatForm;
            SpellEntry const* pTravelForm;
            SpellEntry const* pAquaticForm;
            SpellEntry const* pMoonkinForm;
            SpellEntry const* pWrath;
            SpellEntry const* pMoonfire;
            SpellEntry const* pStarfire;
            SpellEntry const* pHurricane;
            SpellEntry const* pInsectSwarm;
            SpellEntry const* pBarkskin;
            SpellEntry const* pNaturesGrasp;
            SpellEntry const* pMarkoftheWild;
            SpellEntry const* pGiftoftheWild;
            SpellEntry const* pThorns;
            SpellEntry const* pRemoveCurse;
            SpellEntry const* pCurePoison;
            SpellEntry const* pAbolishPoison;
            SpellEntry const* pRebirth;
            SpellEntry const* pFaerieFire;
            SpellEntry const* pInnervate;
            SpellEntry const* pNaturesSwiftness;
            SpellEntry const* pEntanglingRoots;
            SpellEntry const* pHibernate;
            // Cat
            SpellEntry const* pProwl;
            SpellEntry const* pPounce;
            SpellEntry const* pRavage;
            SpellEntry const* pClaw;
            SpellEntry const* pShred;
            SpellEntry const* pRake;
            SpellEntry const* pRip;
            SpellEntry const* pFerociousBite;
            SpellEntry const* pTigersFury;
            SpellEntry const* pDash;
            SpellEntry const* pFaerieFireFeral;
            SpellEntry const* pCower;
            // Bear
            SpellEntry const* pGrowl;
            SpellEntry const* pChallengingRoar;
            SpellEntry const* pDemoralizingRoar;
            SpellEntry const* pEnrage;
            SpellEntry const* pFrenziedRegeneration;
            SpellEntry const* pSwipe;
            SpellEntry const* pMaul;
            SpellEntry const* pBash;
            SpellEntry const* pFeralCharge;
        } druid;
    } m_spells;

    bool m_initialized = false;
    bool m_isBuffing = false;
    bool m_preventCasting = false;
    bool m_receivedBgInvite = false;
    // Throttles for the two refusal logs. Mutable because the checks that write them are questions
    // about a position and nothing else, and const is worth keeping for that; the alternative is
    // every caller of a read-only predicate having to be non-const to carry a log timestamp.
    // Counted separately so that a bot refusing spots all fight cannot hide the one line explaining
    // why its fear never went off.
    mutable time_t m_lastPullLog = 0;
    mutable time_t m_lastFearLog = 0;
    mutable time_t m_lastSpellPullLog = 0;
    // When each ally was last dispelled by this bot. Bounded by party size, and written from a
    // const selector, hence mutable.
    mutable std::map<ObjectGuid, time_t> m_lastDispel;
    // Told to hold a spot and wait for the fight to arrive, rather than closing on it. Lives here
    // rather than with the rest of the party bot's pull state so that BeginChasing, which every
    // class rotation reaches for and which is defined on this class, can decline. A battleground bot
    // never sets it.
    bool m_holdPosition = false;
    bool m_equipCheckPending = false;
    uint8 m_visualHonorRank = 0;
    CombatBotRoles m_role = ROLE_INVALID;

    // Whether a melee bot settled in front of its target rather than behind it, which it only does
    // when behind would have meant standing in something else's aggro radius. Recorded so the
    // decision can be revisited: the chase generator is issued once and runs until something
    // clears it, so without this the fallback lasts the whole fight.
    mutable bool m_chasingInFront = false;

    // Who the heal currently in flight was begun for on spec, rather than because they were
    // already hurt. Empty whenever this bot is not holding such a cast.
    ObjectGuid m_speculativeHealTarget;

    // Name or entry of a `player_premade_spell_template` to build this bot from. Set before the
    // bot initialises. Empty means fall back to picking by role, which cannot distinguish two
    // specs that share one, so anything caring which build it gets should set this.
    std::string m_specName;
};

#endif
