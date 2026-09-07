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

// How often an armour slot gets a permanent enchant. Weapons and shields always do: those are the
// pieces anybody bothers with, because a weapon enchant scales everything the character does.
// Bracers and boots are what a real player enchants when the mats happen to be lying around.
static constexpr int32 CB_ARMOR_ENCHANT_CHANCE = 50;

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

class CombatBotBaseAI : public PlayerBotAI
{
public:

    CombatBotBaseAI() : PlayerBotAI(nullptr)
    {
        for (auto& ptr : m_spells.raw.spells)
            ptr = nullptr;
    }

    virtual void OnPacketReceived(WorldPacket const* packet) override;
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
    void LearnPremadeSpecForClass();
    PlayerPremadeSpecTemplate const* FindPremadeSpecByName(std::string const& name) const;
    PlayerPremadeSpecTemplate const* SelectPremadeSpecTemplate() const;
    void EquipPremadeGearTemplate();
    void EquipRandomGearInEmptySlots();
    void AutoEquipGear(uint32 option);
    void LearnClassSpellsForLevel();
    void LearnRandomTalents();

    // Permanent enchants on everything worn that has a slot for one, and the hour-long
    // consumables a real group turns up carrying. See BotProvisions.h for what and why.
    void ApplyProvisionEnchants();
    void StockProvisionConsumables();
    bool UseProvisionConsumables();

    // Starting a heal before anybody needs it, and throwing it away if nobody comes to need it.
    bool BeginSpeculativeHeal();
    bool ReconsiderHealInFlight();
    
    uint8 GetAttackersInRangeCount(float range) const;
    Unit* SelectAttackerDifferentFrom(Unit const* pExcept) const;
    Unit* SelectHealTarget(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f) const;
    Unit* SelectPeriodicHealTarget(float selfHealPercent = 100.0f, float groupHealPercent = 100.0f) const;
    Player* SelectBuffTarget(SpellEntry const* pSpellEntry) const;
    Player* SelectBuffTarget(SpellEntry const* pSingleSpellEntry, SpellEntry const* pGroupSpellEntry, SpellEntry const*& pSelectedSpellEntry) const;
    Player* SelectDispelTarget(SpellEntry const* pSpellEntry) const;
    bool IsValidBuffTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const;
    bool IsValidHealTarget(Unit const* pTarget, float healthPercent = 100.0f) const;
    float GetMaxHealSpellRange() const;
    float GetManaAdjustedHealPercent(float requestedPercent) const;
    bool IsRationingHealsForTank() const;
    bool IsAlreadyHealing(ObjectGuid guid) const;
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
    int32 GetIncomingdamage(Unit const* pTarget) const;
    bool AreOthersOnSameTarget(ObjectGuid guid, bool checkMelee = true, bool checkSpells = true) const;

    SpellCastResult DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry);
    virtual bool CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const;

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
    bool WouldPositionPullExtraEnemies(float x, float y, float z) const;
    bool WouldFearPullExtraEnemies() const;
    bool SummonShamanTotems();

    // Who a totem dropped right now would actually reach.
    struct TotemAudience
    {
        uint32 members = 0;
        uint32 melee = 0;
        uint32 manaUsers = 0;
    };
    CombatBotRoles GetEffectiveRole(Player const* pTarget) const;
    TotemAudience SurveyTotemAudience() const;
    SpellEntry const* SelectTotemForSlot(TotemSlot slot) const;
    bool IsTotemSpellCoveredByAnotherShaman(TotemSlot slot, SpellEntry const* pSpellEntry) const;
    SpellEntry const* SelectBlessingForTarget(Player const* pTarget) const;
    Player* SelectBlessingTarget(SpellEntry const*& pSelectedSpellEntry) const;
    SpellCastResult CastWeaponBuff(SpellEntry const* pSpellEntry, EquipmentSlots slot);
    bool UseTrinketEffects(bool onlyToBreakCC = false);
    bool UseItemEffect(Item* pItem, bool onlyToBreakCC = false);
    void BreakCrowdControlEffects();

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
    // Told to hold a spot and wait for the fight to arrive, rather than closing on it. Lives here
    // rather than with the rest of the party bot's pull state so that BeginChasing, which every
    // class rotation reaches for and which is defined on this class, can decline. A battleground bot
    // never sets it.
    bool m_holdPosition = false;
    bool m_equipCheckPending = false;
    uint8 m_visualHonorRank = 0;
    CombatBotRoles m_role = ROLE_INVALID;

    // Who the heal currently in flight was begun for on spec, rather than because they were
    // already hurt. Empty whenever this bot is not holding such a cast.
    ObjectGuid m_speculativeHealTarget;

    // Name or entry of a `player_premade_spell_template` to build this bot from. Set before the
    // bot initialises. Empty means fall back to picking by role, which cannot distinguish two
    // specs that share one, so anything caring which build it gets should set this.
    std::string m_specName;
};

#endif
