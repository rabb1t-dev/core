#include "World.h"
#include "CombatBotBaseAI.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Group.h"
#include "Totem.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "CharacterDatabaseCache.h"
#include "Utilities/Random.h"

#include <random>

enum CombatBotSpells
{
    SPELL_MAIL_PROFICIENCY = 8737,
    SPELL_PLATE_PROFICIENCY = 750,

    SPELL_SHIELD_SLAM = 23922,
    SPELL_HOLY_SHIELD = 20925,
    SPELL_SANCTITY_AURA = 20218,
    SPELL_SHADOWFORM = 15473,
    SPELL_ELEMENTAL_MASTERY = 16166,
    SPELL_STORMSTRIKE = 17364,
    SPELL_MOONKIN_FORM = 24858,
    SPELL_LEADER_OF_THE_PACK = 17007,

    SPELL_SUMMON_IMP = 688,
    SPELL_SUMMON_VOIDWALKER = 697,
    SPELL_SUMMON_FELHUNTER = 691,
    SPELL_SUMMON_SUCCUBUS = 712,
    SPELL_TAME_BEAST = 13481,
    SPELL_REVIVE_PET = 982,
    SPELL_CALL_PET = 883,

    PET_WOLF    = 565,
    PET_CAT     = 681,
    PET_BEAR    = 822,
    PET_CRAB    = 831,
    PET_GORILLA = 1108,
    PET_BIRD    = 1109,
    PET_BOAR    = 1190,
    PET_BAT     = 1554,
    PET_CROC    = 1693,
    PET_SPIDER  = 1781,
    PET_OWL     = 1997,
    PET_STRIDER = 2322,
    PET_SCORPID = 3127,
    PET_SERPENT = 3247,
    PET_RAPTOR  = 3254,
    PET_TURTLE  = 3461,
    PET_HYENA   = 4127,
};

void CombatBotBaseAI::AutoAssignRole()
{
    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
        {
            if (me->HasSpell(SPELL_SHIELD_SLAM))
                m_role = ROLE_TANK;
            else
                m_role = ROLE_MELEE_DPS;
            return;
        }
        case CLASS_ROGUE:
        {
            m_role = ROLE_MELEE_DPS;
            return;
        }
        case CLASS_HUNTER:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        {
            m_role = ROLE_RANGE_DPS;
            return;
        }
        case CLASS_PALADIN:
        {
            if (me->HasSpell(SPELL_HOLY_SHIELD))
                m_role = ROLE_TANK;
            else if (me->HasSpell(SPELL_SANCTITY_AURA))
                m_role = ROLE_MELEE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
        case CLASS_PRIEST:
        {
            if (me->HasSpell(SPELL_SHADOWFORM))
                m_role = ROLE_RANGE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
        case CLASS_SHAMAN:
        {
            if (me->HasSpell(SPELL_ELEMENTAL_MASTERY))
                m_role = ROLE_RANGE_DPS;
            else if (me->HasSpell(SPELL_STORMSTRIKE))
                m_role = ROLE_MELEE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
        case CLASS_DRUID:
        {
            if (me->HasSpell(SPELL_MOONKIN_FORM))
                m_role = ROLE_RANGE_DPS;
            else if (me->HasSpell(SPELL_LEADER_OF_THE_PACK))
                m_role = ROLE_MELEE_DPS;
            else
                m_role = ROLE_HEALER;
            return;
        }
    }

    m_role = ROLE_MELEE_DPS;
}

void CombatBotBaseAI::ResetSpellData()
{
    for (auto& ptr : m_spells.raw.spells)
        ptr = nullptr;

    m_resurrectionSpell = nullptr;
    m_spellListDirectHeal.clear();
    m_spellListPeriodicHeal.clear();
    m_spellListTaunt.clear();
}

void CombatBotBaseAI::PopulateSpellData()
{
    // Paladin Seals
    SpellEntry const* pSealOfRighteousness = nullptr;
    SpellEntry const* pSealOfCommand = nullptr;
    SpellEntry const* pSealOfFury = nullptr;

    // Paladin Blessings
    SpellEntry const* pBlessingOfLight = nullptr;
    SpellEntry const* pBlessingOfMight = nullptr;
    SpellEntry const* pBlessingOfWisdom = nullptr;
    SpellEntry const* pBlessingOfKings = nullptr;
    SpellEntry const* pBlessingOfSanctuary = nullptr;

    // Paladin and warlock spells that a higher level one supersedes, kept aside so the
    // slot falls back to them rather than staying empty below the level that grants it.
    SpellEntry const* pPurify = nullptr;
    SpellEntry const* pDemonSkin = nullptr;

    // Paladin Auras
    SpellEntry const* pDevotionAura = nullptr;
    SpellEntry const* pConcentrationAura = nullptr;
    SpellEntry const* pRetributionAura = nullptr;
    SpellEntry const* pSanctityAura = nullptr;
    SpellEntry const* pShadowResistanceAura = nullptr;
    SpellEntry const* pFrostResistanceAura = nullptr;
    SpellEntry const* pFireResistanceAura = nullptr;

    // Air Totems
    SpellEntry const* pGraceOfAirTotem = nullptr;
    SpellEntry const* pNatureResistanceTotem = nullptr;
    SpellEntry const* pWindfuryTotem = nullptr;
    SpellEntry const* pWindwallTotem = nullptr;
    SpellEntry const* pTranquilAirTotem = nullptr;

    // Earth Totems
    SpellEntry const* pEarthbindTotem = nullptr;
    SpellEntry const* pStoneclawtotem = nullptr;
    SpellEntry const* pStoneskinTotem = nullptr;
    SpellEntry const* pStrengthOfEarthTotem = nullptr;
    SpellEntry const* pTremorTotem = nullptr;

    // Fire Totems
    SpellEntry const* pFireNovaTotem = nullptr;
    SpellEntry const* pMagmaTotem = nullptr;
    SpellEntry const* pSearingTotem = nullptr;
    SpellEntry const* pFlametongueTotem = nullptr;
    SpellEntry const* pFrostResistanceTotem = nullptr;

    // Water Totems
    SpellEntry const* pFireResistanceTotem = nullptr;
    SpellEntry const* pDiseaseCleansingTotem = nullptr;
    SpellEntry const* pHealingStreamTotem = nullptr;
    SpellEntry const* pManaSpringTotem = nullptr;
    SpellEntry const* pPoisonCleansingTotem = nullptr;

    // Shaman Weapon Buffs
    SpellEntry const* pFrostbrandWeapon = nullptr;
    SpellEntry const* pRockbiterWeapon = nullptr;
    SpellEntry const* pWindfuryWeapon = nullptr;

    // Mage Polymorph
    SpellEntry const* pPolymorphSheep = nullptr;
    SpellEntry const* pPolymorphCow = nullptr;
    SpellEntry const* pPolymorphPig = nullptr;
    SpellEntry const* pPolymorphTurtle = nullptr;

    // Mage Frost Armor (to replace ice armor at low level)
    SpellEntry const* pFrostArmor = nullptr;

    // The best poison of each kind the rogue can actually make. Held as the spell rather
    // than a flag because a poison's rank is spelled into its name, so the name is the only
    // thing that identifies which enchant to look for later.
    SpellEntry const* pKnownDeadlyPoison = nullptr;
    SpellEntry const* pKnownInstantPoison = nullptr;
    SpellEntry const* pKnownCripplingPoison = nullptr;
    SpellEntry const* pKnownWoundPoison = nullptr;
    SpellEntry const* pKnownMindNumbingPoison = nullptr;

    for (const auto& spell : me->GetSpellMap())
    {
        if (spell.second.disabled)
            continue;

        if (spell.second.state == PLAYERSPELL_REMOVED)
            continue;

        SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(spell.first);
        if (!pSpellEntry)
            continue;

        if (pSpellEntry->HasAttribute(SPELL_ATTR_PASSIVE))
            continue;

        if (pSpellEntry->HasAttribute(SPELL_ATTR_DO_NOT_DISPLAY))
            continue;

        auto IsHigherRankSpell = [pSpellEntry](SpellEntry const* pOldSpell)
        {
            if (!pOldSpell)
                return true;

            uint32 newRank = pSpellEntry->GetRank();
            if (newRank)
                return newRank > pOldSpell->GetRank();

            return pSpellEntry->Id > pOldSpell->Id;
        };

        switch (me->GetClass())
        {
            case CLASS_PALADIN:
            {
                if (pSpellEntry->SpellName[0].find("Seal of Righteousness") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSealOfRighteousness))
                        pSealOfRighteousness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Seal of Command") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSealOfCommand))
                        pSealOfCommand = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Judgement") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pJudgement))
                        m_spells.paladin.pJudgement = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hammer of Justice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHammerOfJustice))
                        m_spells.paladin.pHammerOfJustice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Sacrifice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pBlessingOfSacrifice))
                        m_spells.paladin.pBlessingOfSacrifice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Freedom") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pBlessingOfFreedom))
                        m_spells.paladin.pBlessingOfFreedom = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Protection") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pBlessingOfProtection))
                        m_spells.paladin.pBlessingOfProtection = pSpellEntry;
                }
                // Matched ahead of the five below and deliberately dropped. A Greater Blessing is
                // the reagent-consuming version that buffs everyone of the target's class at
                // once, and its name contains the single-target name exactly, so an unguarded
                // find() files it as an ordinary blessing. Nothing here casts one yet, and a slot
                // meant to be cast on one member at a time is the wrong place for it.
                else if (pSpellEntry->SpellName[0].find("Greater Blessing of") != std::string::npos)
                {
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Sanctuary") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfSanctuary))
                        pBlessingOfSanctuary = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Kings") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfKings))
                        pBlessingOfKings = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Wisdom") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfWisdom))
                        pBlessingOfWisdom = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Might") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfMight))
                        pBlessingOfMight = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blessing of Light") != std::string::npos)
                {
                    if (IsHigherRankSpell(pBlessingOfLight))
                        pBlessingOfLight = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Devotion Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pDevotionAura))
                        pDevotionAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Retribution Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pRetributionAura))
                        pRetributionAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Concentration Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pConcentrationAura))
                        pConcentrationAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sanctity Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSanctityAura))
                        pSanctityAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Resistance Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pShadowResistanceAura))
                        pShadowResistanceAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Resistance Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostResistanceAura))
                        pFrostResistanceAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Resistance Aura") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFireResistanceAura))
                        pFireResistanceAura = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Exorcism") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pExorcism))
                        m_spells.paladin.pExorcism = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Consecration") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pConsecration))
                        m_spells.paladin.pConsecration = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hammer of Wrath") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHammerOfWrath))
                        m_spells.paladin.pHammerOfWrath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cleanse") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pCleanse))
                        m_spells.paladin.pCleanse = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Purify") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPurify))
                        pPurify = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Divine Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pDivineShield))
                        m_spells.paladin.pDivineShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Lay on Hands") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pLayOnHands))
                        m_spells.paladin.pLayOnHands = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Righteous Fury") != std::string::npos) // post 1.9
                {
                    if (IsHigherRankSpell(m_spells.paladin.pRighteousFury))
                        m_spells.paladin.pRighteousFury = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Seal of Fury") != std::string::npos) // pre 1.9
                {
                    if (IsHigherRankSpell(pSealOfFury))
                        pSealOfFury = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHolyShock))
                        m_spells.paladin.pHolyShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Divine Favor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pDivineFavor))
                        m_spells.paladin.pDivineFavor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Wrath") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHolyWrath))
                        m_spells.paladin.pHolyWrath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Turn Evil") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pTurnEvil))
                        m_spells.paladin.pTurnEvil = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.paladin.pHolyShield))
                        m_spells.paladin.pHolyShield = pSpellEntry;
                }
                break;
            }
            case CLASS_SHAMAN:
            {
                if (pSpellEntry->SpellName[0].find("Lightning Bolt") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pLightningBolt))
                        m_spells.shaman.pLightningBolt = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Chain Lightning") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pChainLightning))
                        m_spells.shaman.pChainLightning = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Earth Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pEarthShock))
                        m_spells.shaman.pEarthShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Flame Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pFlameShock))
                        m_spells.shaman.pFlameShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Shock") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pFrostShock))
                        m_spells.shaman.pFrostShock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Purge") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pPurge))
                        m_spells.shaman.pPurge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stormstrike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pStormstrike))
                        m_spells.shaman.pStormstrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Elemental Mastery") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pElementalMastery))
                        m_spells.shaman.pElementalMastery = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Lightning Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pLightningShield))
                        m_spells.shaman.pLightningShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ghost Wolf") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pGhostWolf))
                        m_spells.shaman.pGhostWolf = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cure Disease") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pCureDisease))
                        m_spells.shaman.pCureDisease = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cure Poison") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pCurePoison))
                        m_spells.shaman.pCurePoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frostbrand Weapon") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostbrandWeapon))
                        pFrostbrandWeapon = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rockbiter Weapon") != std::string::npos)
                {
                    if (IsHigherRankSpell(pRockbiterWeapon))
                        pRockbiterWeapon = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Windfury Weapon") != std::string::npos)
                {
                    if (IsHigherRankSpell(pWindfuryWeapon))
                        pWindfuryWeapon = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Grace of Air Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pGraceOfAirTotem))
                        pGraceOfAirTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Nature Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pNatureResistanceTotem))
                        pNatureResistanceTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Windfury Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pWindfuryTotem))
                        pWindfuryTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Windwall Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pWindwallTotem))
                        pWindwallTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Tranquil Air Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pTranquilAirTotem))
                        pTranquilAirTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Earthbind Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pEarthbindTotem))
                        pEarthbindTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stoneclaw Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pStoneclawtotem))
                        pStoneclawtotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stoneskin Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pStoneskinTotem))
                        pStoneskinTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Strength of Earth Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pStrengthOfEarthTotem))
                        pStrengthOfEarthTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Tremor Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pTremorTotem))
                        pTremorTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Nova Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFireNovaTotem))
                        pFireNovaTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Magma Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pMagmaTotem))
                        pMagmaTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Searing Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pSearingTotem))
                        pSearingTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Flametongue Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFlametongueTotem))
                        pFlametongueTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostResistanceTotem))
                        pFrostResistanceTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Resistance Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFireResistanceTotem))
                        pFireResistanceTotem = pSpellEntry;
                }
                // No spell is named "Disease Resistance Totem"; the water totem that cures
                // disease is the cleansing one, so this slot was matching nothing at all.
                else if (pSpellEntry->SpellName[0].find("Disease Cleansing Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pDiseaseCleansingTotem))
                        pDiseaseCleansingTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Healing Stream Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pHealingStreamTotem))
                        pHealingStreamTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Spring Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pManaSpringTotem))
                        pManaSpringTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Poison Cleansing Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPoisonCleansingTotem))
                        pPoisonCleansingTotem = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Tide Totem") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.shaman.pManaTideTotem))
                        m_spells.shaman.pManaTideTotem = pSpellEntry;
                }
                break;
            }
            case CLASS_HUNTER:
            {
                if (pSpellEntry->SpellName[0].find("Aspect of the Cheetah") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAspectOfTheCheetah))
                        m_spells.hunter.pAspectOfTheCheetah = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Aspect of the Hawk") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAspectOfTheHawk))
                        m_spells.hunter.pAspectOfTheHawk = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Aspect of the Monkey") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAspectOfTheMonkey))
                        m_spells.hunter.pAspectOfTheMonkey = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Serpent Sting") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pSerpentSting))
                        m_spells.hunter.pSerpentSting = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pArcaneShot))
                        m_spells.hunter.pArcaneShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Aimed Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pAimedShot))
                        m_spells.hunter.pAimedShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Multi-Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pMultiShot))
                        m_spells.hunter.pMultiShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Concussive Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pConcussiveShot))
                        m_spells.hunter.pConcussiveShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Wing Clip") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pWingClip))
                        m_spells.hunter.pWingClip = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hunter's Mark") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pHuntersMark))
                        m_spells.hunter.pHuntersMark = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mongoose Bite") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pMongooseBite))
                        m_spells.hunter.pMongooseBite = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Raptor Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pRaptorStrike))
                        m_spells.hunter.pRaptorStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Disengage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pDisengage))
                        m_spells.hunter.pDisengage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Feign Death") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pFeignDeath))
                        m_spells.hunter.pFeignDeath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Scare Beast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pScareBeast))
                        m_spells.hunter.pScareBeast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Volley") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.hunter.pVolley))
                        m_spells.hunter.pVolley = pSpellEntry;
                }
                break;
            }
            case CLASS_MAGE:
            {
                if (pSpellEntry->SpellName[0].find("Ice Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pIceArmor))
                        m_spells.mage.pIceArmor = pSpellEntry;
                }
                if (pSpellEntry->SpellName[0].find("Frost Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(pFrostArmor))
                        pFrostArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ice Barrier") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pIceBarrier))
                        m_spells.mage.pIceBarrier = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pManaShield))
                        m_spells.mage.pManaShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Intellect") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcaneIntellect))
                        m_spells.mage.pArcaneIntellect = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Brilliance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcaneBrilliance))
                        m_spells.mage.pArcaneBrilliance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frostbolt") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFrostbolt))
                        m_spells.mage.pFrostbolt = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fire Blast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFireBlast))
                        m_spells.mage.pFireBlast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fireball") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFireball))
                        m_spells.mage.pFireball = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Explosion") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcaneExplosion))
                        m_spells.mage.pArcaneExplosion = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frost Nova") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pFrostNova))
                        m_spells.mage.pFrostNova = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cone of Cold") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pConeofCold))
                        m_spells.mage.pConeofCold = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blink") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pBlink))
                        m_spells.mage.pBlink = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0] == "Polymorph") // Sheep
                {
                    if (IsHigherRankSpell(pPolymorphSheep))
                        pPolymorphSheep = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Polymorph: Cow") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPolymorphCow))
                        pPolymorphCow = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Polymorph: Pig") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPolymorphPig))
                        pPolymorphPig = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Polymorph: Turtle") != std::string::npos)
                {
                    if (IsHigherRankSpell(pPolymorphTurtle))
                        pPolymorphTurtle = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Counterspell") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pCounterspell))
                        m_spells.mage.pCounterspell = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Presence of Mind") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pPresenceOfMind))
                        m_spells.mage.pPresenceOfMind = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Arcane Power") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pArcanePower))
                        m_spells.mage.pArcanePower = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Remove Lesser Curse") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pRemoveLesserCurse))
                        m_spells.mage.pRemoveLesserCurse = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Scorch") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pScorch))
                        m_spells.mage.pScorch = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Pyroblast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pPyroblast))
                        m_spells.mage.pPyroblast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Evocation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pEvocation))
                        m_spells.mage.pEvocation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ice Block") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pIceBlock))
                        m_spells.mage.pIceBlock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blizzard") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pBlizzard))
                        m_spells.mage.pBlizzard = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blast Wave") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pBlastWave))
                        m_spells.mage.pBlastWave = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Combustion") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.mage.pCombustion))
                        m_spells.mage.pCombustion = pSpellEntry;
                }
                break;
            }
            case CLASS_PRIEST:
            {
                if (pSpellEntry->SpellName[0].find("Power Word: Fortitude") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPowerWordFortitude))
                        m_spells.priest.pPowerWordFortitude = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Divine Spirit") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pDivineSpirit))
                        m_spells.priest.pDivineSpirit = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prayer of Spirit") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPrayerofSpirit))
                        m_spells.priest.pPrayerofSpirit = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prayer of Fortitude") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPrayerofFortitude))
                        m_spells.priest.pPrayerofFortitude = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prayer of Shadow Protection") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPrayerofShadowProtection))
                        m_spells.priest.pPrayerofShadowProtection = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Inner Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pInnerFire))
                        m_spells.priest.pInnerFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Protection") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShadowProtection))
                        m_spells.priest.pShadowProtection = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Power Word: Shield") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPowerWordShield))
                        m_spells.priest.pPowerWordShield = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Nova") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pHolyNova))
                        m_spells.priest.pHolyNova = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Holy Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pHolyFire))
                        m_spells.priest.pHolyFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mind Blast") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pMindBlast))
                        m_spells.priest.pMindBlast = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mind Flay") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pMindFlay))
                        m_spells.priest.pMindFlay = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Word: Pain") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShadowWordPain))
                        m_spells.priest.pShadowWordPain = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Inner Focus") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pInnerFocus))
                        m_spells.priest.pInnerFocus = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Abolish Disease") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pAbolishDisease))
                        m_spells.priest.pAbolishDisease = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Dispel Magic") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pDispelMagic))
                        m_spells.priest.pDispelMagic = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mana Burn") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pManaBurn))
                        m_spells.priest.pManaBurn = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Devouring Plague") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pDevouringPlague))
                        m_spells.priest.pDevouringPlague = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Psychic Scream") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pPsychicScream))
                        m_spells.priest.pPsychicScream = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadowform") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShadowform))
                        m_spells.priest.pShadowform = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Vampiric Embrace") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pVampiricEmbrace))
                        m_spells.priest.pVampiricEmbrace = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Silence") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pSilence))
                        m_spells.priest.pSilence = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fade") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pFade))
                        m_spells.priest.pFade = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shackle Undead") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pShackleUndead))
                        m_spells.priest.pShackleUndead = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Smite") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.priest.pSmite))
                        m_spells.priest.pSmite = pSpellEntry;
                }
                break;
            }
            case CLASS_WARLOCK:
            {
                if (pSpellEntry->SpellName[0].find("Demon Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDemonArmor))
                        m_spells.warlock.pDemonArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demon Skin") != std::string::npos)
                {
                    if (IsHigherRankSpell(pDemonSkin))
                        pDemonSkin = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Death Coil") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDeathCoil))
                        m_spells.warlock.pDeathCoil = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Detect Invisibility") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDetectInvisibility))
                        m_spells.warlock.pDetectInvisibility = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Ward") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pShadowWard))
                        m_spells.warlock.pShadowWard = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadow Bolt") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pShadowBolt))
                        m_spells.warlock.pShadowBolt = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Corruption") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCorruption))
                        m_spells.warlock.pCorruption = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Conflagrate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pConflagrate))
                        m_spells.warlock.pConflagrate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shadowburn") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pShadowburn))
                        m_spells.warlock.pShadowburn = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Searing Pain") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pSearingPain))
                        m_spells.warlock.pSearingPain = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Immolate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pImmolate))
                        m_spells.warlock.pImmolate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rain of Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pRainOfFire))
                        m_spells.warlock.pRainOfFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demonic Sacrifice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDemonicSacrifice))
                        m_spells.warlock.pDemonicSacrifice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Drain Life") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pDrainLife))
                        m_spells.warlock.pDrainLife = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Siphon Life") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pSiphonLife))
                        m_spells.warlock.pSiphonLife = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Banish") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pBanish))
                        m_spells.warlock.pBanish = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Fear") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pFear))
                        m_spells.warlock.pFear = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Howl of Terror") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pHowlofTerror))
                        m_spells.warlock.pHowlofTerror = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Agony") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofAgony))
                        m_spells.warlock.pCurseofAgony = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of the Elements") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseoftheElements))
                        m_spells.warlock.pCurseoftheElements = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Shadow") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofShadow))
                        m_spells.warlock.pCurseofShadow = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Recklessness") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofRecklessness))
                        m_spells.warlock.pCurseofRecklessness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Curse of Tongues") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pCurseofTongues))
                        m_spells.warlock.pCurseofTongues = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Life Tap") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warlock.pLifeTap))
                        m_spells.warlock.pLifeTap = pSpellEntry;
                }
                break;
            }
            case CLASS_WARRIOR:
            {
                if (pSpellEntry->SpellName[0].find("Battle Stance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBattleStance))
                        m_spells.warrior.pBattleStance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Berserker Stance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBerserkerStance))
                        m_spells.warrior.pBerserkerStance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Defensive Stance") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDefensiveStance))
                        m_spells.warrior.pDefensiveStance = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0] == "Charge")
                {
                    if (IsHigherRankSpell(m_spells.warrior.pCharge))
                        m_spells.warrior.pCharge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Intercept") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pIntercept))
                        m_spells.warrior.pIntercept = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Overpower") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pOverpower))
                        m_spells.warrior.pOverpower = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Heroic Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pHeroicStrike))
                        m_spells.warrior.pHeroicStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cleave") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pCleave))
                        m_spells.warrior.pCleave = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Execute") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pExecute))
                        m_spells.warrior.pExecute = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mortal Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pMortalStrike))
                        m_spells.warrior.pMortalStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Bloodthirst") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBloodthirst))
                        m_spells.warrior.pBloodthirst = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Bloodrage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBloodrage))
                        m_spells.warrior.pBloodrage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Berserker Rage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBerserkerRage))
                        m_spells.warrior.pBerserkerRage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Recklessness") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRecklessness))
                        m_spells.warrior.pRecklessness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Retaliation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRetaliation))
                        m_spells.warrior.pRetaliation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Death Wish") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDeathWish))
                        m_spells.warrior.pDeathWish = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Intimidating Shout") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pIntimidatingShout))
                        m_spells.warrior.pIntimidatingShout = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Pummel") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pPummel))
                        m_spells.warrior.pPummel = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rend") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pRend))
                        m_spells.warrior.pRend = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Disarm") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDisarm))
                        m_spells.warrior.pDisarm = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Whirlwind") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pWhirlwind))
                        m_spells.warrior.pWhirlwind = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Battle Shout") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pBattleShout))
                        m_spells.warrior.pBattleShout = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demoralizing Shout") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pDemoralizingShout))
                        m_spells.warrior.pDemoralizingShout = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hamstring") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pHamstring))
                        m_spells.warrior.pHamstring = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Thunder Clap") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pThunderClap))
                        m_spells.warrior.pThunderClap = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sweeping Strikes") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pSweepingStrikes))
                        m_spells.warrior.pSweepingStrikes = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Last Stand") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pLastStand))
                        m_spells.warrior.pLastStand = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Block") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldBlock))
                        m_spells.warrior.pShieldBlock = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Wall") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldWall))
                        m_spells.warrior.pShieldWall = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Bash") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldBash))
                        m_spells.warrior.pShieldBash = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shield Slam") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pShieldSlam))
                        m_spells.warrior.pShieldSlam = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sunder Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pSunderArmor))
                        m_spells.warrior.pSunderArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Concussion Blow") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pConcussionBlow))
                        m_spells.warrior.pConcussionBlow = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Piercing Howl") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.warrior.pPiercingHowl))
                        m_spells.warrior.pPiercingHowl = pSpellEntry;
                }
                break;
            }
            case CLASS_ROGUE:
            {
                if (pSpellEntry->SpellName[0].find("Slice and Dice") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pSliceAndDice))
                        m_spells.rogue.pSliceAndDice = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sinister Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pSinisterStrike))
                        m_spells.rogue.pSinisterStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Adrenaline Rush") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pAdrenalineRush))
                        m_spells.rogue.pAdrenalineRush = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Eviscerate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pEviscerate))
                        m_spells.rogue.pEviscerate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Stealth") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pStealth))
                        m_spells.rogue.pStealth = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Garrote") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pGarrote))
                        m_spells.rogue.pGarrote = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ambush") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pAmbush))
                        m_spells.rogue.pAmbush = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cheap Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pCheapShot))
                        m_spells.rogue.pCheapShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Premeditation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pPremeditation))
                        m_spells.rogue.pPremeditation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Backstab") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pBackstab))
                        m_spells.rogue.pBackstab = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hemorrhage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pHemorrhage))
                        m_spells.rogue.pHemorrhage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ghostly Strike") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pGhostlyStrike))
                        m_spells.rogue.pGhostlyStrike = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Gouge") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pGouge))
                        m_spells.rogue.pGouge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rupture") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pRupture))
                        m_spells.rogue.pRupture = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Expose Armor") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pExposeArmor))
                        m_spells.rogue.pExposeArmor = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Kidney Shot") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pKidneyShot))
                        m_spells.rogue.pKidneyShot = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cold Blood") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pColdBlood))
                        m_spells.rogue.pColdBlood = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Blade Flurry") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pBladeFlurry))
                        m_spells.rogue.pBladeFlurry = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Vanish") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pVanish))
                        m_spells.rogue.pVanish = pSpellEntry;
                }
                else if (pSpellEntry->IsFitToFamily<SPELLFAMILY_ROGUE, CF_ROGUE_BLIND>())
                {
                    if (IsHigherRankSpell(m_spells.rogue.pBlind))
                        m_spells.rogue.pBlind = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Preparation") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pPreparation))
                        m_spells.rogue.pPreparation = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Evasion") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pEvasion))
                        m_spells.rogue.pEvasion = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Riposte") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pRiposte))
                        m_spells.rogue.pRiposte = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Kick") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pKick))
                        m_spells.rogue.pKick = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Sprint") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.rogue.pSprint))
                        m_spells.rogue.pSprint = pSpellEntry;
                }
                // Ranked by level rather than by IsHigherRankSpell, because what the rank
                // text says is beside the point here: the enchant is found by name, and
                // the name of the highest level one the rogue can craft is the answer.
                else if (pSpellEntry->SpellName[0].find("Deadly Poison") != std::string::npos)
                {
                    if (!pKnownDeadlyPoison || pKnownDeadlyPoison->spellLevel < pSpellEntry->spellLevel)
                        pKnownDeadlyPoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Instant Poison") != std::string::npos)
                {
                    if (!pKnownInstantPoison || pKnownInstantPoison->spellLevel < pSpellEntry->spellLevel)
                        pKnownInstantPoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Crippling Poison") != std::string::npos)
                {
                    if (!pKnownCripplingPoison || pKnownCripplingPoison->spellLevel < pSpellEntry->spellLevel)
                        pKnownCripplingPoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Wound Poison") != std::string::npos)
                {
                    if (!pKnownWoundPoison || pKnownWoundPoison->spellLevel < pSpellEntry->spellLevel)
                        pKnownWoundPoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mind-numbing Poison") != std::string::npos)
                {
                    if (!pKnownMindNumbingPoison || pKnownMindNumbingPoison->spellLevel < pSpellEntry->spellLevel)
                        pKnownMindNumbingPoison = pSpellEntry;
                }
                break;
            }
            case CLASS_DRUID:
            {
                if (pSpellEntry->SpellName[0].find("Bear Form") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pBearForm))
                        m_spells.druid.pBearForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (768)) // Cat Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pCatForm))
                        m_spells.druid.pCatForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (783)) // Travel Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pTravelForm))
                        m_spells.druid.pTravelForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (1066)) // Aquatic Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pAquaticForm))
                        m_spells.druid.pAquaticForm = pSpellEntry;
                }
                else if (pSpellEntry->Id == (24858)) // Moonkin Form
                {
                    if (IsHigherRankSpell(m_spells.druid.pMoonkinForm))
                        m_spells.druid.pMoonkinForm = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Wrath") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pWrath))
                        m_spells.druid.pWrath = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Moonfire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pMoonfire))
                        m_spells.druid.pMoonfire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Starfire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pStarfire))
                        m_spells.druid.pStarfire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hurricane") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pHurricane))
                        m_spells.druid.pHurricane = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Insect Swarm") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pInsectSwarm))
                        m_spells.druid.pInsectSwarm = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Barkskin") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pBarkskin))
                        m_spells.druid.pBarkskin = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Nature's Grasp") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pNaturesGrasp))
                        m_spells.druid.pNaturesGrasp = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Mark of the Wild") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pMarkoftheWild))
                        m_spells.druid.pMarkoftheWild = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Gift of the Wild") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pGiftoftheWild))
                        m_spells.druid.pGiftoftheWild = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Thorns") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pThorns))
                        m_spells.druid.pThorns = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Remove Curse") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRemoveCurse))
                        m_spells.druid.pRemoveCurse = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cure Poison") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pCurePoison))
                        m_spells.druid.pCurePoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Abolish Poison") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pAbolishPoison))
                        m_spells.druid.pAbolishPoison = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rebirth") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRebirth))
                        m_spells.druid.pRebirth = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Innervate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pInnervate))
                        m_spells.druid.pInnervate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Nature's Swiftness") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pNaturesSwiftness))
                        m_spells.druid.pNaturesSwiftness = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Entangling Roots") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pEntanglingRoots))
                        m_spells.druid.pEntanglingRoots = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Hibernate") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pHibernate))
                        m_spells.druid.pHibernate = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Pounce") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pPounce))
                        m_spells.druid.pPounce = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ravage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRavage))
                        m_spells.druid.pRavage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Claw") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pClaw))
                        m_spells.druid.pClaw = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Shred") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pShred))
                        m_spells.druid.pShred = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rake") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRake))
                        m_spells.druid.pRake = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Rip") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pRip))
                        m_spells.druid.pRip = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Ferocious Bite") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFerociousBite))
                        m_spells.druid.pFerociousBite = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Tiger's Fury") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pTigersFury))
                        m_spells.druid.pTigersFury = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Dash") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pDash))
                        m_spells.druid.pDash = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Cower") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pCower))
                        m_spells.druid.pCower = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Faerie Fire (Feral)") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFaerieFireFeral))
                        m_spells.druid.pFaerieFireFeral = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Faerie Fire") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFaerieFire))
                        m_spells.druid.pFaerieFire = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Growl") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pGrowl))
                        m_spells.druid.pGrowl = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Challenging Roar") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pChallengingRoar))
                        m_spells.druid.pChallengingRoar = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Demoralizing Roar") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pDemoralizingRoar))
                        m_spells.druid.pDemoralizingRoar = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Enrage") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pEnrage))
                        m_spells.druid.pEnrage = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Frenzied Regeneration") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFrenziedRegeneration))
                        m_spells.druid.pFrenziedRegeneration = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Swipe") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pSwipe))
                        m_spells.druid.pSwipe = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Maul") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pMaul))
                        m_spells.druid.pMaul = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Bash") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pBash))
                        m_spells.druid.pBash = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Feral Charge") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pFeralCharge))
                        m_spells.druid.pFeralCharge = pSpellEntry;
                }
                else if (pSpellEntry->SpellName[0].find("Prowl") != std::string::npos)
                {
                    if (IsHigherRankSpell(m_spells.druid.pProwl))
                        m_spells.druid.pProwl = pSpellEntry;
                }
                break;
            }
        }

        for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
        {
            switch (pSpellEntry->Effect[i])
            {
                case SPELL_EFFECT_HEAL:
                    m_spellListDirectHeal.insert(pSpellEntry);
                    break;
                case SPELL_EFFECT_ATTACK_ME:
                    m_spellListTaunt.push_back(pSpellEntry);
                    break;
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_RESURRECT_NEW:
                    // The spell map is unordered, so taking the last one seen left the rank
                    // down to hash order rather than to level.
                    if (IsHigherRankSpell(m_resurrectionSpell))
                        m_resurrectionSpell = pSpellEntry;
                    break;
                case SPELL_EFFECT_APPLY_AURA:
                {
                    switch (pSpellEntry->EffectApplyAuraName[i])
                    {
                        case SPELL_AURA_PERIODIC_HEAL:
                            m_spellListPeriodicHeal.insert(pSpellEntry);
                            break;
                        case SPELL_AURA_MOD_TAUNT:
                            m_spellListTaunt.push_back(pSpellEntry);
                            break;
                    }
                    break;
                }
            }
        }
    }

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (pSealOfFury && m_role == ROLE_TANK)
                m_spells.paladin.pSeal = pSealOfFury;
            else if (pSealOfCommand)
                m_spells.paladin.pSeal = pSealOfCommand;
            else
                m_spells.paladin.pSeal = pSealOfRighteousness;

            m_blessings.pMight = pBlessingOfMight;
            m_blessings.pWisdom = pBlessingOfWisdom;
            m_blessings.pKings = pBlessingOfKings;
            m_blessings.pSanctuary = pBlessingOfSanctuary;
            m_blessings.pLight = pBlessingOfLight;

            // A blessing is single target, so which one to cast is a fact about the person
            // receiving it and not about the paladin. This slot is now only the paladin's own
            // blessing and the fallback for a target it cannot classify; SelectBlessingForTarget
            // decides the rest at cast time.
            m_spells.paladin.pBlessingBuff = SelectBlessingForTarget(me);

            // Only one aura runs at a time, so it follows the paladin's own job. The three
            // resistance auras are last resorts rather than choices: they are worth running on
            // specific fights, and drawing one at random is how a raid ended up with Fire
            // Resistance Aura where Devotion should have been.
            SpellEntry const* pPreferredAura = nullptr;
            switch (m_role)
            {
                case ROLE_TANK:
                    pPreferredAura = pDevotionAura;
                    break;
                case ROLE_HEALER:
                    pPreferredAura = pConcentrationAura;
                    break;
                case ROLE_MELEE_DPS:
                    pPreferredAura = pSanctityAura ? pSanctityAura : pRetributionAura;
                    break;
                default:
                    break;
            }

            SpellEntry const* const auraChoices[] = { pPreferredAura, pDevotionAura, pRetributionAura,
                pConcentrationAura, pFireResistanceAura, pFrostResistanceAura, pShadowResistanceAura };
            for (SpellEntry const* pAuraChoice : auraChoices)
            {
                if (pAuraChoice)
                {
                    m_spells.paladin.pAura = pAuraChoice;
                    break;
                }
            }

            if (!m_spells.paladin.pCleanse && pPurify)
                m_spells.paladin.pCleanse = pPurify;

            break;
        }
        case CLASS_SHAMAN:
        {
            m_totems.pWindfury = pWindfuryTotem;
            m_totems.pGraceOfAir = pGraceOfAirTotem;
            m_totems.pNatureResistance = pNatureResistanceTotem;
            m_totems.pWindwall = pWindwallTotem;
            m_totems.pTranquilAir = pTranquilAirTotem;
            m_totems.pStrengthOfEarth = pStrengthOfEarthTotem;
            m_totems.pStoneskin = pStoneskinTotem;
            m_totems.pStoneclaw = pStoneclawtotem;
            m_totems.pTremor = pTremorTotem;
            m_totems.pEarthbind = pEarthbindTotem;
            m_totems.pSearing = pSearingTotem;
            m_totems.pMagma = pMagmaTotem;
            m_totems.pFireNova = pFireNovaTotem;
            m_totems.pFlametongue = pFlametongueTotem;
            m_totems.pFrostResistance = pFrostResistanceTotem;
            m_totems.pManaSpring = pManaSpringTotem;
            m_totems.pHealingStream = pHealingStreamTotem;
            m_totems.pPoisonCleansing = pPoisonCleansingTotem;
            m_totems.pDiseaseCleansing = pDiseaseCleansingTotem;
            m_totems.pFireResistance = pFireResistanceTotem;

            // What the bot would drop knowing nothing about the situation. SummonShamanTotems asks
            // again per school every time a slot is empty, so this is the resting choice and the
            // diagnostic view rather than a decision frozen for the life of the bot.
            m_spells.shaman.pAirTotem = SelectTotemForSlot(TOTEM_SLOT_AIR);
            m_spells.shaman.pEarthTotem = SelectTotemForSlot(TOTEM_SLOT_EARTH);
            m_spells.shaman.pFireTotem = SelectTotemForSlot(TOTEM_SLOT_FIRE);
            m_spells.shaman.pWaterTotem = SelectTotemForSlot(TOTEM_SLOT_WATER);

            // Windfury for anyone who swings the weapon. For a caster the imbue barely matters,
            // so this mainly needs to stop being a coin toss.
            SpellEntry const* const weaponBuffChoices[] = {
                (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK) ? pWindfuryWeapon : nullptr,
                pRockbiterWeapon, pFrostbrandWeapon, pWindfuryWeapon };
            for (SpellEntry const* pWeaponBuffChoice : weaponBuffChoices)
            {
                if (pWeaponBuffChoice)
                {
                    m_spells.shaman.pWeaponBuff = pWeaponBuffChoice;
                    break;
                }
            }

            break;
        }
        case CLASS_MAGE:
        {
            if (!m_spells.mage.pIceArmor && pFrostArmor)
                m_spells.mage.pIceArmor = pFrostArmor;

            std::vector<SpellEntry const*> polymorph;
            if (pPolymorphSheep)
                polymorph.push_back(pPolymorphSheep);
            if (pPolymorphCow)
                polymorph.push_back(pPolymorphCow);
            if (pPolymorphPig)
                polymorph.push_back(pPolymorphPig);
            if (pPolymorphTurtle)
                polymorph.push_back(pPolymorphTurtle);
            if (!polymorph.empty())
                m_spells.mage.pPolymorph = SelectRandomContainerElement(polymorph);

            break;
        }
        case CLASS_WARLOCK:
        {
            if (!m_spells.warlock.pDemonArmor && pDemonSkin)
                m_spells.warlock.pDemonArmor = pDemonSkin;

            break;
        }
        case CLASS_ROGUE:
        {
            // Rogues can only craft an item that applies the poison, they don't know the actual poison
            // enchant. The two share a name exactly, so the crafting spell the rogue knows names the
            // enchant to look for.
            //
            // This used to search for the base name and take the highest level match, which sounds
            // equivalent and is not: ranks past the first are named "Deadly Poison II" and upwards, so an
            // exact match on "Deadly Poison" could only ever find rank 1. Every rogue bot in the game has
            // been applying the level 30 poison, and the level 20 Instant Poison, whatever its level.
            auto GetPoisonEnchant = [](SpellEntry const* pKnownPoison) -> SpellEntry const*
            {
                if (!pKnownPoison)
                    return nullptr;

                for (uint32 i = 0; i < sSpellMgr.GetMaxSpellId(); i++)
                {
                    if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(i))
                    {
                        if (pSpellEntry->Effect[0] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY &&
                            pSpellEntry->SpellName[0] == pKnownPoison->SpellName[0])
                        {
                            return pSpellEntry;
                        }
                    }
                }
                return nullptr;
            };

            std::vector<SpellEntry const*> vPoisons;
            for (SpellEntry const* pKnownPoison : { pKnownDeadlyPoison, pKnownInstantPoison,
                                                    pKnownCripplingPoison, pKnownWoundPoison,
                                                    pKnownMindNumbingPoison })
            {
                if (SpellEntry const* pPoisonSpell = GetPoisonEnchant(pKnownPoison))
                    vPoisons.push_back(pPoisonSpell);
            }

            if (!vPoisons.empty())
            {
                m_spells.rogue.pMainHandPoison = SelectRandomContainerElement(vPoisons);
                m_spells.rogue.pOffHandPoison = SelectRandomContainerElement(vPoisons);
            }

            break;
        }
    }
}

// Names every slot the function above fills, so a null one can be seen from the outside.
// An unfilled slot presents as "the bot never uses this spell", which is indistinguishable
// from a rotation bug, and several have sat null for the life of the file on that account.
std::vector<CombatBotBaseAI::SpellSlot> CombatBotBaseAI::GetSpellSlots() const
{
#define SLOT(cls, member) CombatBotBaseAI::SpellSlot{ #member, m_spells.cls.member }

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            return { SLOT(paladin, pAura), SLOT(paladin, pSeal), SLOT(paladin, pBlessingBuff),
                     SLOT(paladin, pBlessingOfProtection), SLOT(paladin, pBlessingOfFreedom),
                     SLOT(paladin, pBlessingOfSacrifice), SLOT(paladin, pHammerOfJustice),
                     SLOT(paladin, pJudgement), SLOT(paladin, pExorcism), SLOT(paladin, pConsecration),
                     SLOT(paladin, pHammerOfWrath), SLOT(paladin, pCleanse), SLOT(paladin, pDivineShield),
                     SLOT(paladin, pLayOnHands), SLOT(paladin, pRighteousFury), SLOT(paladin, pHolyShock),
                     SLOT(paladin, pDivineFavor), SLOT(paladin, pHolyWrath), SLOT(paladin, pTurnEvil),
                     SLOT(paladin, pHolyShield) };
        case CLASS_SHAMAN:
            return { SLOT(shaman, pLightningBolt), SLOT(shaman, pChainLightning), SLOT(shaman, pEarthShock),
                     SLOT(shaman, pFlameShock), SLOT(shaman, pFrostShock), SLOT(shaman, pPurge),
                     SLOT(shaman, pStormstrike), SLOT(shaman, pElementalMastery), SLOT(shaman, pLightningShield),
                     SLOT(shaman, pGhostWolf), SLOT(shaman, pCureDisease), SLOT(shaman, pCurePoison),
                     SLOT(shaman, pAirTotem), SLOT(shaman, pEarthTotem), SLOT(shaman, pFireTotem),
                     SLOT(shaman, pWaterTotem), SLOT(shaman, pManaTideTotem), SLOT(shaman, pWeaponBuff) };
        case CLASS_HUNTER:
            return { SLOT(hunter, pAspectOfTheCheetah), SLOT(hunter, pAspectOfTheMonkey),
                     SLOT(hunter, pAspectOfTheHawk), SLOT(hunter, pSerpentSting), SLOT(hunter, pArcaneShot),
                     SLOT(hunter, pAimedShot), SLOT(hunter, pMultiShot), SLOT(hunter, pConcussiveShot),
                     SLOT(hunter, pWingClip), SLOT(hunter, pHuntersMark), SLOT(hunter, pMongooseBite),
                     SLOT(hunter, pRaptorStrike), SLOT(hunter, pDisengage), SLOT(hunter, pFeignDeath),
                     SLOT(hunter, pScareBeast), SLOT(hunter, pVolley) };
        case CLASS_MAGE:
            return { SLOT(mage, pIceArmor), SLOT(mage, pArcaneIntellect), SLOT(mage, pArcaneBrilliance),
                     SLOT(mage, pIceBarrier), SLOT(mage, pManaShield), SLOT(mage, pPolymorph),
                     SLOT(mage, pFrostbolt), SLOT(mage, pFireBlast), SLOT(mage, pFireball),
                     SLOT(mage, pArcaneExplosion), SLOT(mage, pFrostNova), SLOT(mage, pConeofCold),
                     SLOT(mage, pBlink), SLOT(mage, pCounterspell), SLOT(mage, pPresenceOfMind),
                     SLOT(mage, pArcanePower), SLOT(mage, pRemoveLesserCurse), SLOT(mage, pScorch),
                     SLOT(mage, pPyroblast), SLOT(mage, pEvocation), SLOT(mage, pIceBlock),
                     SLOT(mage, pBlizzard), SLOT(mage, pBlastWave), SLOT(mage, pCombustion) };
        case CLASS_PRIEST:
            return { SLOT(priest, pPowerWordFortitude), SLOT(priest, pDivineSpirit),
                     SLOT(priest, pPrayerofSpirit), SLOT(priest, pPrayerofFortitude),
                     SLOT(priest, pPrayerofShadowProtection), SLOT(priest, pInnerFire),
                     SLOT(priest, pShadowProtection), SLOT(priest, pPowerWordShield), SLOT(priest, pHolyNova),
                     SLOT(priest, pHolyFire), SLOT(priest, pMindBlast), SLOT(priest, pMindFlay),
                     SLOT(priest, pShadowWordPain), SLOT(priest, pInnerFocus), SLOT(priest, pAbolishDisease),
                     SLOT(priest, pDispelMagic), SLOT(priest, pManaBurn), SLOT(priest, pDevouringPlague),
                     SLOT(priest, pPsychicScream), SLOT(priest, pShadowform), SLOT(priest, pVampiricEmbrace),
                     SLOT(priest, pSilence), SLOT(priest, pFade), SLOT(priest, pShackleUndead),
                     SLOT(priest, pSmite) };
        case CLASS_WARLOCK:
            return { SLOT(warlock, pDemonArmor), SLOT(warlock, pDeathCoil), SLOT(warlock, pDetectInvisibility),
                     SLOT(warlock, pShadowWard), SLOT(warlock, pShadowBolt), SLOT(warlock, pCorruption),
                     SLOT(warlock, pConflagrate), SLOT(warlock, pShadowburn), SLOT(warlock, pSearingPain),
                     SLOT(warlock, pImmolate), SLOT(warlock, pRainOfFire), SLOT(warlock, pDemonicSacrifice),
                     SLOT(warlock, pDrainLife), SLOT(warlock, pSiphonLife), SLOT(warlock, pBanish),
                     SLOT(warlock, pFear), SLOT(warlock, pHowlofTerror), SLOT(warlock, pCurseofAgony),
                     SLOT(warlock, pCurseofDoom), SLOT(warlock, pCurseoftheElements),
                     SLOT(warlock, pCurseofShadow), SLOT(warlock, pCurseofRecklessness),
                     SLOT(warlock, pCurseofTongues), SLOT(warlock, pCurseofExhaustion), SLOT(warlock, pLifeTap) };
        case CLASS_WARRIOR:
            return { SLOT(warrior, pBattleStance), SLOT(warrior, pBerserkerStance),
                     SLOT(warrior, pDefensiveStance), SLOT(warrior, pCharge), SLOT(warrior, pIntercept),
                     SLOT(warrior, pOverpower), SLOT(warrior, pHeroicStrike), SLOT(warrior, pCleave),
                     SLOT(warrior, pExecute), SLOT(warrior, pMortalStrike), SLOT(warrior, pBloodthirst),
                     SLOT(warrior, pBloodrage), SLOT(warrior, pBerserkerRage), SLOT(warrior, pRecklessness),
                     SLOT(warrior, pRetaliation), SLOT(warrior, pDeathWish), SLOT(warrior, pIntimidatingShout),
                     SLOT(warrior, pPummel), SLOT(warrior, pRend), SLOT(warrior, pDisarm),
                     SLOT(warrior, pWhirlwind), SLOT(warrior, pBattleShout), SLOT(warrior, pDemoralizingShout),
                     SLOT(warrior, pHamstring), SLOT(warrior, pThunderClap), SLOT(warrior, pSweepingStrikes),
                     SLOT(warrior, pLastStand), SLOT(warrior, pShieldBlock), SLOT(warrior, pShieldWall),
                     SLOT(warrior, pShieldBash), SLOT(warrior, pShieldSlam), SLOT(warrior, pSunderArmor),
                     SLOT(warrior, pConcussionBlow), SLOT(warrior, pPiercingHowl) };
        case CLASS_ROGUE:
            return { SLOT(rogue, pSliceAndDice), SLOT(rogue, pSinisterStrike), SLOT(rogue, pAdrenalineRush),
                     SLOT(rogue, pEviscerate), SLOT(rogue, pStealth), SLOT(rogue, pGarrote),
                     SLOT(rogue, pAmbush), SLOT(rogue, pCheapShot), SLOT(rogue, pPremeditation),
                     SLOT(rogue, pBackstab), SLOT(rogue, pHemorrhage), SLOT(rogue, pGhostlyStrike),
                     SLOT(rogue, pGouge), SLOT(rogue, pRupture), SLOT(rogue, pExposeArmor),
                     SLOT(rogue, pKidneyShot), SLOT(rogue, pColdBlood), SLOT(rogue, pBladeFlurry),
                     SLOT(rogue, pVanish), SLOT(rogue, pBlind), SLOT(rogue, pPreparation),
                     SLOT(rogue, pEvasion), SLOT(rogue, pRiposte), SLOT(rogue, pKick), SLOT(rogue, pSprint),
                     SLOT(rogue, pMainHandPoison), SLOT(rogue, pOffHandPoison) };
        case CLASS_DRUID:
            return { SLOT(druid, pBearForm), SLOT(druid, pCatForm), SLOT(druid, pTravelForm),
                     SLOT(druid, pAquaticForm), SLOT(druid, pMoonkinForm), SLOT(druid, pWrath),
                     SLOT(druid, pMoonfire), SLOT(druid, pStarfire), SLOT(druid, pHurricane),
                     SLOT(druid, pInsectSwarm), SLOT(druid, pBarkskin), SLOT(druid, pNaturesGrasp),
                     SLOT(druid, pMarkoftheWild), SLOT(druid, pGiftoftheWild), SLOT(druid, pThorns),
                     SLOT(druid, pRemoveCurse), SLOT(druid, pCurePoison), SLOT(druid, pAbolishPoison),
                     SLOT(druid, pRebirth), SLOT(druid, pFaerieFire), SLOT(druid, pInnervate),
                     SLOT(druid, pNaturesSwiftness), SLOT(druid, pEntanglingRoots), SLOT(druid, pHibernate),
                     SLOT(druid, pProwl), SLOT(druid, pPounce), SLOT(druid, pRavage), SLOT(druid, pClaw),
                     SLOT(druid, pShred), SLOT(druid, pRake), SLOT(druid, pRip), SLOT(druid, pFerociousBite),
                     SLOT(druid, pTigersFury), SLOT(druid, pDash), SLOT(druid, pFaerieFireFeral),
                     SLOT(druid, pCower), SLOT(druid, pGrowl), SLOT(druid, pChallengingRoar),
                     SLOT(druid, pDemoralizingRoar), SLOT(druid, pEnrage), SLOT(druid, pFrenziedRegeneration),
                     SLOT(druid, pSwipe), SLOT(druid, pMaul), SLOT(druid, pBash), SLOT(druid, pFeralCharge) };
    }

#undef SLOT

    return {};
}

void CombatBotBaseAI::AddAllSpellReagents()
{
    for (const auto& pSpell : m_spells.raw.spells)
    {
        if (pSpell)
        {
            for (const auto& reagent : pSpell->Reagent)
            {
                if (reagent && !me->HasItemCount(reagent, 1))
                    AddItemToInventory(reagent);
            }
            for (const auto& totem : pSpell->Totem)
            {
                if (totem && !me->HasItemCount(totem, 1))
                    AddItemToInventory(totem);
            }
        }
    }
}

bool CombatBotBaseAI::AreOthersOnSameTarget(ObjectGuid guid, bool checkMelee, bool checkSpells) const
{
    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            // Not self.
            if (pMember == me)
                continue;

            // Not the target itself.
            if (pMember->GetObjectGuid() == guid)
                continue;

            if (pMember->GetTargetGuid() == guid)
            {
                if (checkMelee && pMember->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    return true;

                if (checkSpells && pMember->IsNonMeleeSpellCasted())
                    return true;
            }
        }
    }
    return false;
}

bool CombatBotBaseAI::FindAndHealInjuredAlly(float selfHealPercent, float groupHealPercent)
{
    Unit* pTarget = SelectHealTarget(selfHealPercent, groupHealPercent);
    if (!pTarget)
        return false;

    return HealInjuredTarget(pTarget);
}

template <class T>
SpellEntry const* CombatBotBaseAI::SelectMostEfficientHealingSpell(Unit const* pTarget, std::set<SpellEntry const*, T>& spellList) const
{
    return SelectMostEfficientHealingSpell(pTarget, pTarget->GetMaxHealth() - pTarget->GetHealth(), spellList);
}

template <class T>
SpellEntry const* CombatBotBaseAI::SelectMostEfficientHealingSpell(Unit const* pTarget, int32 missingHealth, std::set<SpellEntry const*, T>& spellList) const
{
    SpellEntry const* pHealSpell = nullptr;
    int32 healthDiff = INT32_MAX;

    // Find most efficient healing spell.
    for (const auto pSpellEntry : spellList)
    {
        if (CanTryToCastSpell(pTarget, pSpellEntry))
        {
            int32 basePoints = 0;
            for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
            {
                switch (pSpellEntry->Effect[i])
                {
                    case SPELL_EFFECT_HEAL:
                        basePoints += pSpellEntry->EffectBasePoints[i];
                        break;
                    case SPELL_EFFECT_APPLY_AURA:
                    case SPELL_EFFECT_PERSISTENT_AREA_AURA:
                    case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                        if (pSpellEntry->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_HEAL)
                            basePoints += ((pSpellEntry->GetDuration() / pSpellEntry->EffectAmplitude[i]) * pSpellEntry->EffectBasePoints[i]);
                        break;
                }
            }

            int32 const diff = basePoints - missingHealth;
            if (std::abs(diff) < healthDiff)
            {
                healthDiff = diff;
                pHealSpell = pSpellEntry;
            }

            // Healing spells are sorted from strongest to weakest.
            if (diff < 0)
                break;
        }
    }

    return pHealSpell;
}

int32 CombatBotBaseAI::GetIncomingdamage(Unit const* pTarget) const
{
    int32 damage = 0;
    for (auto const& pAttacker : pTarget->GetAttackers())
        if (pAttacker->CanReachWithMeleeAutoAttack(pTarget))
            damage += int32((pAttacker->GetFloatValue(UNIT_FIELD_MINDAMAGE) + pAttacker->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2);
    return damage;
}

bool CombatBotBaseAI::HealInjuredTarget(Unit* pTarget)
{
    // Put a HoT on the target if only missing a little health.
    if (pTarget->GetHealthPercent() >= 80.0f &&
       !pTarget->HasAuraType(SPELL_AURA_PERIODIC_HEAL))
    {
        if (HealInjuredTargetPeriodic(pTarget))
            return true;
    }

    if (HealInjuredTargetDirect(pTarget))
        return true;

    return false;
}

bool CombatBotBaseAI::HealInjuredTargetPeriodic(Unit* pTarget)
{
    if (SpellEntry const* pHealSpell = SelectMostEfficientHealingSpell(pTarget, m_spellListPeriodicHeal))
    {
        if (CanTryToCastSpell(pTarget, pHealSpell))
        {
            if (DoCastSpell(pTarget, pHealSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

bool CombatBotBaseAI::HealInjuredTargetDirect(Unit* pTarget)
{
    if (SpellEntry const* pHealSpell = SelectMostEfficientHealingSpell(pTarget, m_spellListDirectHeal))
        if (DoCastSpell(pTarget, pHealSpell) == SPELL_CAST_OK)
            return true;

    return false;
}

bool CombatBotBaseAI::IsValidHealTarget(Unit const* pTarget, float healthPercent) const
{
    return (pTarget->GetHealthPercent() < healthPercent) &&
            me->IsValidHelpfulTarget(pTarget) &&
            me->IsWithinLOSInMap(pTarget) &&
            me->IsWithinDist(pTarget, 30.0f);
}

Unit* CombatBotBaseAI::SelectHealTarget(float selfHealPercent, float groupHealPercent) const
{
    if (me->GetHealthPercent() < selfHealPercent)
        return me;

    if (IsInDuel())
        return nullptr;

    Unit* pTarget = nullptr;
    float healthPercent = 100.0f;

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {
                // We already checked self.
                if (pMember == me)
                    continue;

                // Avoid all healers picking same target.
                if (pTarget && !IsTankClass(pMember->GetClass()) && AreOthersOnSameTarget(pMember->GetObjectGuid(), false, true))
                    continue;

                // Check if we should heal party member.
                if ((IsValidHealTarget(pMember, groupHealPercent) &&
                    healthPercent > pMember->GetHealthPercent()) ||
                    // Or a pet if there are no injured players.
                    (!pTarget && (pMember = pMember->GetPet()) &&
                      IsValidHealTarget(pMember, groupHealPercent)))
                {
                    healthPercent = pMember->GetHealthPercent();
                    pTarget = pMember;
                }
            }
        }
    }

    if (healthPercent == 100.0f)
        return nullptr;

    return pTarget;
}

Unit* CombatBotBaseAI::SelectPeriodicHealTarget(float selfHealPercent, float groupHealPercent) const
{
    if (me->GetHealthPercent() < selfHealPercent &&
       !me->HasAuraType(SPELL_AURA_PERIODIC_HEAL))
        return me;

    if (IsInDuel())
        return nullptr;

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {
                // We already checked self.
                if (pMember == me)
                    continue;

                // Check if we should heal party member.
                if (IsValidHealTarget(pMember, groupHealPercent) &&
                   !pMember->HasAuraType(SPELL_AURA_PERIODIC_HEAL))
                    return pMember;
            }
        }
    }

    return nullptr;
}

bool CombatBotBaseAI::FindAndPreHealTarget()
{
    Unit* pTarget = me;
    int32 maxIncomingDamage = GetIncomingdamage(me);

    if (!IsInDuel())
    {
        if (Group* pGroup = me->GetGroup())
        {
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Unit* pMember = itr->getSource())
                {
                    // We already checked self.
                    if (pMember == me)
                        continue;

                    // Avoid all healers picking same target.
                    if (pTarget && !IsTankClass(pMember->GetClass()) && AreOthersOnSameTarget(pMember->GetObjectGuid(), false, true))
                        continue;

                    int32 incomingDamage = GetIncomingdamage(pMember);
                    if (!incomingDamage)
                        continue;

                    // Check if we should heal party member.
                    if (incomingDamage > maxIncomingDamage &&
                        IsValidHealTarget(pMember))
                    {
                        maxIncomingDamage = incomingDamage;
                        pTarget = pMember;
                    }
                }
            }
        }
    }

    if (!maxIncomingDamage)
        return false;

    // Add currently missing health too.
    maxIncomingDamage += int32(pTarget->GetMaxHealth() - pTarget->GetHealth());
    if (maxIncomingDamage < int32(pTarget->GetMaxHealth() / 2))
        return false;

    if (SpellEntry const* pHealSpell = SelectMostEfficientHealingSpell(pTarget, maxIncomingDamage, m_spellListDirectHeal))
    {
        if (pHealSpell->GetCastTime(me) > 1000 && CanTryToCastSpell(pTarget, pHealSpell))
        {
            if (DoCastSpell(pTarget, pHealSpell) == SPELL_CAST_OK)
                return true;
        }
    }

    return pTarget;
}

bool CombatBotBaseAI::IsValidHostileTarget(Unit const* pTarget) const
{
    return me->IsValidAttackTarget(pTarget) &&
           pTarget->IsVisibleForOrDetect(me, me, false) &&
           !pTarget->HasBreakableByDamageCrowdControlAura() &&
           !pTarget->IsTotalImmune() &&
           pTarget->GetTransport() == me->GetTransport();
}

bool CombatBotBaseAI::IsValidDispelTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    uint32 dispelMask = 0;
    bool bFoundOneDispell = false;
    // Compute Dispel Mask
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (pSpellEntry->Effect[i] != SPELL_EFFECT_DISPEL)
            continue;

        // Create dispel mask by dispel type
        uint32 dispel_type = pSpellEntry->EffectMiscValue[i];
        dispelMask |= Spells::GetDispellMask(DispelType(dispel_type));
    }
    bool friendly_dispel = pTarget && pTarget->IsFriendlyTo(me);

    if (pTarget &&
        // Check immune for offensive dispel
        (!pTarget->IsImmuneToSchoolMask(pSpellEntry->GetSpellSchoolMask()) ||
            friendly_dispel))
    {
        if (!friendly_dispel && !me->IsValidAttackTarget(pTarget))
            return false;

        auto const& auras = pTarget->GetSpellAuraHolderMap();
        for (const auto& aura : auras)
        {
            SpellAuraHolder* holder = aura.second;
            if ((1 << holder->GetSpellProto()->Dispel) & dispelMask)
            {
                if (holder->GetSpellProto()->Dispel == DISPEL_MAGIC ||
                    holder->GetSpellProto()->Dispel == DISPEL_DISEASE ||
                    holder->GetSpellProto()->Dispel == DISPEL_POISON)
                {
                    bool positive = holder->IsPositive();
                    // do not remove positive auras if friendly target
                    // do not remove negative auras if non-friendly target
                    // when removing charm auras ignore hostile reaction from the charm
                    if (!friendly_dispel && !positive && holder->GetSpellProto()->IsCharmSpell())
                        if (CharmInfo *charm = pTarget->GetCharmInfo())
                            if (FactionTemplateEntry const* ft = charm->GetOriginalFactionTemplate())
                                if (FactionTemplateEntry const* ft2 = me->GetFactionTemplateEntry())
                                    if (charm->GetOriginalFactionTemplate()->IsFriendlyTo(*ft2))
                                        bFoundOneDispell = true;
                    if (positive == friendly_dispel)
                        continue;
                }
                bFoundOneDispell = true;
                break;
            }
        }
    }

    if (!bFoundOneDispell)
        return false;

    return true;
}

uint8 CombatBotBaseAI::GetAttackersInRangeCount(float range) const
{
    uint8 count = 0;
    for (const auto& pTarget : me->GetAttackers())
    {
        if (me->GetCombatDistance(pTarget) <= range)
            count++;
    }

    return count;
}

Unit* CombatBotBaseAI::SelectAttackerDifferentFrom(Unit const* pExcept) const
{
    for (const auto& pTarget : me->GetAttackers())
    {
        if (pTarget != pExcept)
            return pTarget;
    }

    return nullptr;
}

bool CombatBotBaseAI::IsValidBuffTarget(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    std::vector<uint32> morePowerfulSpells;
    sSpellMgr.ListMorePowerfulSpells(pSpellEntry->Id, morePowerfulSpells);

    for (const auto& i : pTarget->GetSpellAuraHolderMap())
    {
        if (i.first == pSpellEntry->Id)
            return false;

        if (sSpellMgr.IsRankSpellDueToSpell(pSpellEntry, i.first))
            return false;

        for (const auto& it : morePowerfulSpells)
            if (it == i.first)
                return false;
    }

    return true;
}

Player* CombatBotBaseAI::SelectBuffTarget(SpellEntry const* pSpellEntry) const
{
    Group* pGroup = me->GetGroup();
    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (me->IsValidHelpfulTarget(pMember) &&
                   !pMember->IsGameMaster() &&
                    IsValidBuffTarget(pMember, pSpellEntry) &&
                    me->IsWithinLOSInMap(pMember) &&
                    me->IsWithinDist(pMember, 30.0f))
                    return pMember;
            }
        }
    }

    return nullptr;
}

Player* CombatBotBaseAI::SelectBuffTarget(SpellEntry const* pSingleSpellEntry, SpellEntry const* pGroupSpellEntry, SpellEntry const*& pSelectedSpellEntry) const
{
    pSelectedSpellEntry = nullptr;

    if (!pSingleSpellEntry && !pGroupSpellEntry)
        return nullptr;

    if (!pSingleSpellEntry)
    {
        pSelectedSpellEntry = pGroupSpellEntry;
        return SelectBuffTarget(pGroupSpellEntry);
    }

    if (!pGroupSpellEntry)
    {
        pSelectedSpellEntry = pSingleSpellEntry;
        return SelectBuffTarget(pSingleSpellEntry);
    }

    Player* pFirstMissingMember = nullptr;
    uint8 missingMemberCount = 0;
    Group* pGroup = me->GetGroup();
    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (!me->IsValidHelpfulTarget(pMember) ||
                    pMember->IsGameMaster() ||
                    !me->IsWithinLOSInMap(pMember) ||
                    !me->IsWithinDist(pMember, 30.0f) ||
                    !IsValidBuffTarget(pMember, pSingleSpellEntry) ||
                    !IsValidBuffTarget(pMember, pGroupSpellEntry))
                    continue;

                if (!pFirstMissingMember)
                    pFirstMissingMember = pMember;

                ++missingMemberCount;
                if (missingMemberCount > 1)
                {
                    pSelectedSpellEntry = pGroupSpellEntry;
                    return pFirstMissingMember;
                }
            }
        }
    }

    if (missingMemberCount == 1)
        pSelectedSpellEntry = pSingleSpellEntry;

    return pFirstMissingMember;
}

Player* CombatBotBaseAI::SelectDispelTarget(SpellEntry const* pSpellEntry) const
{
    Group* pGroup = me->GetGroup();
    if (pGroup)
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* pMember = itr->getSource())
            {
                if (me->IsValidHelpfulTarget(pMember) &&
                   !pMember->IsGameMaster() &&
                    IsValidDispelTarget(pMember, pSpellEntry) &&
                    me->IsWithinLOSInMap(pMember) &&
                    me->IsWithinDist(pMember, 30.0f))
                    return pMember;
            }
        }
    }

    return nullptr;
}

CombatBotRoles CombatBotBaseAI::GetEffectiveRole(Player const* pTarget) const
{
    if (!pTarget)
        return ROLE_INVALID;

    if (pTarget == me)
        return m_role;

    if (WorldSession const* pSession = pTarget->GetSession())
        if (PlayerBotEntry const* pEntry = pSession->GetBot())
            if (CombatBotBaseAI const* pTargetAI = dynamic_cast<CombatBotBaseAI const*>(pEntry->ai.get()))
                if (pTargetAI->m_role != ROLE_INVALID)
                    return pTargetAI->m_role;

    // A human, or anything not running a combat bot AI, has no role to read. Fall back to what the
    // class alone can settle: whether there is a mana bar worth refilling.
    if (pTarget->GetPowerType() != POWER_MANA)
        return ROLE_MELEE_DPS;

    if (IsHealerClass(pTarget->GetClass()))
        return ROLE_HEALER;

    return IsMeleeWeaponClass(pTarget->GetClass()) ? ROLE_MELEE_DPS : ROLE_RANGE_DPS;
}

SpellEntry const* CombatBotBaseAI::SelectBlessingForTarget(Player const* pTarget) const
{
    if (!pTarget)
        return nullptr;

    CombatBotRoles const targetRole = GetEffectiveRole(pTarget);

    SpellEntry const* choices[5] = {};
    uint32 choiceCount = 0;
    auto const consider = [&](SpellEntry const* pSpellEntry)
    {
        if (!pSpellEntry || choiceCount >= 5)
            return;

        for (uint32 i = 0; i < choiceCount; ++i)
            if (choices[i] == pSpellEntry)
                return;

        choices[choiceCount++] = pSpellEntry;
    };

    switch (targetRole)
    {
        case ROLE_TANK:
            // Sanctuary is the tanking blessing and is close to wasted on anyone else.
            consider(m_blessings.pSanctuary);
            consider(m_blessings.pKings);
            consider(m_blessings.pMight);
            break;
        case ROLE_HEALER:
            consider(m_blessings.pWisdom);
            consider(m_blessings.pKings);
            break;
        case ROLE_RANGE_DPS:
            // Hunters are the ranged class whose damage comes off attack power rather than off a
            // mana bar they are trying to make last.
            consider(pTarget->GetClass() == CLASS_HUNTER ? m_blessings.pMight : m_blessings.pWisdom);
            consider(m_blessings.pKings);
            break;
        case ROLE_MELEE_DPS:
        default:
            consider(m_blessings.pMight);
            consider(m_blessings.pKings);
            break;
    }

    consider(m_blessings.pWisdom);
    consider(m_blessings.pMight);
    consider(m_blessings.pLight);

    return choiceCount ? choices[0] : nullptr;
}

Player* CombatBotBaseAI::SelectBlessingTarget(SpellEntry const*& pSelectedSpellEntry) const
{
    pSelectedSpellEntry = nullptr;

    // Unlike the other group buffs there is no single spell to look for, because the blessing a
    // member should be holding depends on that member. So the candidate is picked first and the
    // spell second, which is the reverse of SelectBuffTarget.
    Group* pGroup = me->GetGroup();
    if (!pGroup)
    {
        SpellEntry const* pBlessing = SelectBlessingForTarget(me);
        if (pBlessing && IsValidBuffTarget(me, pBlessing))
        {
            pSelectedSpellEntry = pBlessing;
            return me;
        }

        return nullptr;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember)
            continue;

        if (!me->IsValidHelpfulTarget(pMember) ||
            pMember->IsGameMaster() ||
            !me->IsWithinLOSInMap(pMember) ||
            !me->IsWithinDist(pMember, 30.0f))
            continue;

        SpellEntry const* pBlessing = SelectBlessingForTarget(pMember);
        if (pBlessing && IsValidBuffTarget(pMember, pBlessing))
        {
            pSelectedSpellEntry = pBlessing;
            return pMember;
        }
    }

    return nullptr;
}

void CombatBotBaseAI::SummonPetIfNeeded()
{
    if (me->GetClass() == CLASS_HUNTER)
    {
        if (me->GetCharmGuid())
            return;

        if (me->GetLevel() < 10)
            return;

        if (me->GetPetGuid() || sCharacterDatabaseCache.GetCharacterPetByOwner(me->GetGUIDLow()))
        {
            if (Pet* pPet = me->GetPet())
            {
                if (!pPet->IsAlive())
                    me->CastSpell(pPet, SPELL_REVIVE_PET, true);
            }
            else
                me->CastSpell(me, SPELL_CALL_PET, true);

            return;
        }

        uint32 petId = PickRandomValue( PET_WOLF, PET_CAT, PET_BEAR, PET_CRAB, PET_GORILLA, PET_BIRD,
                                        PET_BOAR, PET_BAT, PET_CROC, PET_SPIDER, PET_OWL, PET_STRIDER,
                                        PET_SCORPID, PET_SERPENT, PET_RAPTOR, PET_TURTLE, PET_HYENA );
        if (Creature* pCreature = me->SummonCreature(petId,
            me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 0.0f,
            TEMPSUMMON_TIMED_COMBAT_OR_DEAD_DESPAWN, 3000, false, 3000))
        {
            pCreature->SetLevel(me->GetLevel());
            me->CastSpell(pCreature, SPELL_TAME_BEAST, true);
        }
    }
    else if (me->GetClass() == CLASS_WARLOCK)
    {
        if (me->GetPetGuid() || me->GetCharmGuid())
            return;

        std::vector<uint32> vSummons;
        if (me->HasSpell(SPELL_SUMMON_IMP))
            vSummons.push_back(SPELL_SUMMON_IMP);
        if (me->HasSpell(SPELL_SUMMON_VOIDWALKER))
            vSummons.push_back(SPELL_SUMMON_VOIDWALKER);
        if (me->HasSpell(SPELL_SUMMON_FELHUNTER))
            vSummons.push_back(SPELL_SUMMON_FELHUNTER);
        if (me->HasSpell(SPELL_SUMMON_SUCCUBUS))
            vSummons.push_back(SPELL_SUMMON_SUCCUBUS);
        if (!vSummons.empty())
            me->CastSpell(me, SelectRandomContainerElement(vSummons), true);
    }
}

void CombatBotBaseAI::LearnArmorProficiencies()
{
    switch (me->GetClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        {
            if (me->GetLevel() >= 40 && !me->HasSpell(SPELL_PLATE_PROFICIENCY))
                me->LearnSpell(SPELL_PLATE_PROFICIENCY, false, false);
            break;
        }
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
        {
            if (me->GetLevel() >= 40 && !me->HasSpell(SPELL_MAIL_PROFICIENCY))
                me->LearnSpell(SPELL_MAIL_PROFICIENCY, false, false);
            break;
        }
    }
}

PlayerPremadeSpecTemplate const* CombatBotBaseAI::FindPremadeSpecByName(std::string const& name) const
{
    // Accept an entry id as well as a name, matching what `.character premade spec` takes.
    uint32 const entry = std::strtoul(name.c_str(), nullptr, 10);

    for (const auto& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
    {
        if (itr.second.requiredClass != me->GetClass())
            continue;

        if (itr.second.name == name || (entry && itr.second.entry == entry))
            return &itr.second;
    }

    return nullptr;
}

// Picks the spec template this bot should be built from, or nullptr to fall back to random
// talents. The ordering is total, so two bots asked for the same thing get the same build.
// That is what lets a roster describe a raid rather than hope the dice cooperate.
PlayerPremadeSpecTemplate const* CombatBotBaseAI::SelectPremadeSpecTemplate() const
{
    // A named spec wins outright. It is also the only way to ask for a build the role enum
    // cannot express, since that enum has no way to tell a fire mage from a frost one.
    if (!m_specName.empty())
    {
        if (PlayerPremadeSpecTemplate const* pNamed = FindPremadeSpecByName(m_specName))
            return pNamed;

        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "CombatBot: no premade spec '%s' for class %u, falling back",
                 m_specName.c_str(), uint32(me->GetClass()));
    }

    std::vector<PlayerPremadeSpecTemplate const*> vSpecs;
    for (const auto& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
    {
        if (itr.second.requiredClass == me->GetClass() &&
            itr.second.level == me->GetLevel())
            vSpecs.push_back(&itr.second);
    }
    // Use lower level spec template if there are no templates for the current level. Note that
    // this under-spends talents, because application only levels a character up to the template
    // level and never spends the difference. Templates must exist at every level actually used.
    if (vSpecs.empty())
    {
        for (const auto& itr : sObjectMgr.GetPlayerPremadeSpecTemplates())
        {
            if (itr.second.requiredClass == me->GetClass() &&
                itr.second.level < me->GetLevel())
                vSpecs.push_back(&itr.second);
        }
    }

    if (vSpecs.empty())
        return nullptr;

    // With no role yet, prefer a damage build. Init has to choose a spec before AutoAssignRole
    // can run, because that infers the role from the very talents this is about to grant, so
    // something has to break the tie -- and defaulting to damage beats healing by accident.
    auto isPreferredRole = [this](PlayerPremadeSpecTemplate const* pSpec)
    {
        if (m_role != ROLE_INVALID)
            return pSpec->role == m_role;

        return pSpec->role == ROLE_MELEE_DPS || pSpec->role == ROLE_RANGE_DPS;
    };

    // Sorted rather than scanned because the template map is unordered, so ties previously
    // resolved differently from one run to the next. Entry is the final tie break for that reason.
    std::sort(vSpecs.begin(), vSpecs.end(),
        [&isPreferredRole](PlayerPremadeSpecTemplate const* a, PlayerPremadeSpecTemplate const* b)
    {
        bool const aPreferred = isPreferredRole(a);
        if (aPreferred != isPreferredRole(b))
            return aPreferred;

        if (a->level != b->level)
            return a->level > b->level;

        return a->entry < b->entry;
    });

    return vSpecs.front();
}

void CombatBotBaseAI::LearnPremadeSpecForClass()
{
    if (PlayerPremadeSpecTemplate const* pSpec = SelectPremadeSpecTemplate())
    {
        sObjectMgr.ApplyPremadeSpecTemplateToPlayer(pSpec->entry, me);
        if (m_role == ROLE_INVALID)
            m_role = pSpec->role;
    }
    else
    {
        // Use gm command to learn spells on trainers and items.
        LearnRandomTalents();
        char trainerArgs[] = "";
        char itemArgs[] = "";
        ChatHandler(me).HandleLearnAllTrainerCommand(trainerArgs);
        ChatHandler(me).HandleLearnAllItemsCommand(itemArgs);
    }
}

void CombatBotBaseAI::LearnRandomTalents()
{
    if (!me->GetFreeTalentPoints())
        return;

    std::vector<uint32> talentTabsForClass;
    for (uint32 talentTab = 0; talentTab < sTalentTabStore.GetNumRows(); ++talentTab)
    {
        TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentTab);
        if (!talentTabInfo)
            continue;

        if ((me->GetClassMask() & talentTabInfo->ClassMask) == 0)
            continue;

        talentTabsForClass.push_back(talentTab);
    }

    if (talentTabsForClass.empty())
        return;

    uint32 chosenTab = SelectRandomContainerElement(talentTabsForClass);

    std::map<uint32 /*row*/, std::vector<std::pair<uint32 /*talent id*/, uint32 /*ranks*/>>> possibleTalents;
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talentId);
        if (!talentInfo)
            continue;

        if (talentInfo->TalentTab != chosenTab)
            continue;

        uint32 ranks;
        for (ranks = 0; ranks < MAX_TALENT_RANK && talentInfo->RankID[ranks]; ++ranks);
        possibleTalents[talentInfo->Row].push_back({ talentId, ranks });
    }

    if (possibleTalents.empty())
        return;

    auto seed = std::chrono::system_clock::now().time_since_epoch().count();
    for (auto& itrRow : possibleTalents)
    {
        std::shuffle(itrRow.second.begin(), itrRow.second.end(), std::default_random_engine(seed));
        for (auto const& itrTalent: itrRow.second)
        {
            for (uint32 rank = 0; rank < itrTalent.second; ++rank)
            {
                if (me->LearnTalent(itrTalent.first, rank))
                {
                    if (!me->GetFreeTalentPoints())
                        return;
                }
                else
                    break;
            }
        }
    }
}

void CombatBotBaseAI::EquipPremadeGearTemplate()
{
    std::vector<PlayerPremadeGearTemplate const*> vGear;
    for (const auto& itr : sObjectMgr.GetPlayerPremadeGearTemplates())
    {
        if (itr.second.requiredClass == me->GetClass() &&
            itr.second.level <= me->GetLevel())
        {
            if (!vGear.empty())
            {
                if (vGear.front()->level < itr.second.level)
                    vGear.clear();
                else if (vGear.front()->level > itr.second.level)
                    continue;
            }
            vGear.push_back(&itr.second);
        }
    }

    if (!vGear.empty())
    {
        std::vector<PlayerPremadeGearTemplate const*> vGear2;

        // Try to find a role appropriate gear template.
        if (m_role != ROLE_INVALID)
        {
            for (const auto itr : vGear)
            {
                if (itr->role == m_role)
                    vGear2.push_back(itr);
            }
        }

        PlayerPremadeGearTemplate const* pGear;
        if (vGear2.empty())
            pGear = SelectRandomContainerElement(vGear);
        else
            pGear = SelectRandomContainerElement(vGear2);

        sObjectMgr.ApplyPremadeGearTemplateToPlayer(pGear->entry, me);
    }
}

inline uint32 GetPrimaryItemStatForClassAndRole(uint8 playerClass, uint8 role)
{
    switch (playerClass)
    {
        case CLASS_WARRIOR:
        {
            return ITEM_MOD_STRENGTH;
        }
        case CLASS_PALADIN:
        {
            return ((role == ROLE_HEALER) ? ITEM_MOD_INTELLECT : ITEM_MOD_STRENGTH);
        }
        case CLASS_HUNTER:
        case CLASS_ROGUE:
        {
            return ITEM_MOD_AGILITY;
        }
        case CLASS_SHAMAN:
        case CLASS_DRUID:
        {
            return ((role == ROLE_MELEE_DPS || role == ROLE_TANK) ? ITEM_MOD_AGILITY : ITEM_MOD_INTELLECT);
        }
        case CLASS_PRIEST:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
        {
            return ITEM_MOD_INTELLECT;
        }
    }
    return ITEM_MOD_STAMINA;
}

void CombatBotBaseAI::EquipRandomGearInEmptySlots()
{
    LearnArmorProficiencies();

    bool const onlyPvE = urand(0, 1) != 0;
    uint8 const honorRank = onlyPvE ? 0 : urand(5, 18);

    std::map<uint32 /*slot*/, std::vector<ItemPrototype const*>> itemsPerSlot;
    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* pProto = &itr.second;

        // Only items that have already been discovered by someone
        if (!pProto->Discovered)
            continue;

        // Skip unobtainable items
        if (pProto->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            continue;

        // Only gear and weapons
        if (pProto->Class != ITEM_CLASS_WEAPON && pProto->Class != ITEM_CLASS_ARMOR)
            continue;

        // No tabards and shirts
        if (pProto->InventoryType == INVTYPE_TABARD || pProto->InventoryType == INVTYPE_BODY)
            continue;

        if (pProto->SourceQuestRaces && !(pProto->SourceQuestRaces & me->GetRaceMask()))
            continue;

        if (pProto->SourceQuestClasses && !(pProto->SourceQuestClasses & me->GetClassMask()))
            continue;

        if (pProto->SourceQuestLevel < 0)
        {
            // Avoid higher level items with no level requirement
            if (!pProto->RequiredLevel && pProto->ItemLevel > me->GetLevel())
                continue;
        }
        else
        {
            // Item is from a high level quest
            if (uint32(pProto->SourceQuestLevel) > me->GetLevel())
                continue;
        }

        // Avoid low level items
        if ((pProto->ItemLevel + sWorld.getConfig(CONFIG_UINT32_PARTY_BOT_RANDOM_GEAR_LEVEL_DIFFERENCE)) < me->GetLevel())
            continue;

        if (me->CanUseItem(pProto, onlyPvE) != EQUIP_ERR_OK)
            continue;

        if (pProto->RequiredHonorRank > honorRank)
            continue;

        if (pProto->RequiredReputationFaction && uint32(me->GetReputationRank(pProto->RequiredReputationFaction)) < pProto->RequiredReputationRank)
            continue;

        if (uint32 skill = pProto->GetProficiencySkill())
        {
            // Don't equip cloth items on warriors, etc unless bot is a healer
            if (pProto->Class == ITEM_CLASS_ARMOR &&
                pProto->InventoryType != INVTYPE_CLOAK &&
                pProto->InventoryType != INVTYPE_SHIELD &&
                skill != me->GetHighestKnownArmorProficiency() &&
                m_role != ROLE_HEALER)
                continue;

            // Fist weapons use unarmed skill calculations, but we must query fist weapon skill presence to use this item
            if (pProto->SubClass == ITEM_SUBCLASS_WEAPON_FIST)
                skill = SKILL_FIST_WEAPONS;
            if (!me->GetSkillValue(skill))
                continue;
        }

        uint8 slots[4];
        pProto->GetAllowedEquipSlots(slots, me->GetClass(), me->CanDualWield());

        for (uint8 slot : slots)
        {
            if (slot >= EQUIPMENT_SLOT_START && slot < EQUIPMENT_SLOT_END &&
                !me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                // Offhand checks
                if (slot == EQUIPMENT_SLOT_OFFHAND)
                {
                    // Only allow shield in offhand for tanks
                    if (pProto->InventoryType != INVTYPE_SHIELD &&
                        m_role == ROLE_TANK && IsShieldClass(me->GetClass()))
                        continue;

                    // Only equip holdables on mana users
                    if (pProto->InventoryType == INVTYPE_HOLDABLE &&
                        m_role != ROLE_HEALER && m_role != ROLE_RANGE_DPS)
                        continue;
                }

                itemsPerSlot[slot].push_back(pProto);

                // Unique item
                if (pProto->MaxCount == 1)
                    break;
            }
        }
    }

    // 1. Remove items that don't have our primary stat from the list
    // 2. Remove non-pvp items if we have a pvp item available
    uint32 const primaryStat = GetPrimaryItemStatForClassAndRole(me->GetClass(), m_role);
    for (auto& itr : itemsPerSlot)
    {
        bool hasPrimaryStatItem = false;

        for (auto const& pItem : itr.second)
        {
            for (auto const& stat : pItem->ItemStat)
            {
                if (stat.ItemStatType == primaryStat && stat.ItemStatValue > 0)
                {
                    hasPrimaryStatItem = true;
                    break;
                }
            }

            if (hasPrimaryStatItem)
                break;
        }

        if (hasPrimaryStatItem)
        {
            itr.second.erase(std::remove_if(itr.second.begin(), itr.second.end(),
            [primaryStat](ItemPrototype const* & pItem)
            {
                bool itemHasPrimaryStat = false;
                for (auto const& stat : pItem->ItemStat)
                {
                    if (stat.ItemStatType == primaryStat && stat.ItemStatValue > 0)
                    {
                        itemHasPrimaryStat = true;
                        break;
                    }
                }

                return !itemHasPrimaryStat;
            }),
                itr.second.end());
        }

        bool hasPvpItem = false;

        for (auto const& pItem : itr.second)
        {
            if (pItem->RequiredHonorRank)
            {
                hasPvpItem = true;
                break;
            }
        }

        if (hasPvpItem)
        {
            itr.second.erase(std::remove_if(itr.second.begin(), itr.second.end(),
                [](ItemPrototype const* & pItem)
            {
                return pItem->RequiredHonorRank == 0;
            }),
                itr.second.end());
        }
    }

    for (auto const& itr : itemsPerSlot)
    {
        // Don't equip offhand if using 2 handed weapon
        if (itr.first == EQUIPMENT_SLOT_OFFHAND)
        {
            if (Item* pMainHandItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                if (pMainHandItem->GetProto()->InventoryType == INVTYPE_2HWEAPON)
                    continue;
        }

        if (itr.second.empty())
            continue;

        ItemPrototype const* pProto = SelectRandomContainerElement(itr.second);
        if (!pProto)
            continue;

        me->SatisfyItemRequirements(pProto);
        me->StoreNewItemInBestSlots(pProto->ItemId, 1);
    }
}

void CombatBotBaseAI::AutoEquipGear(uint32 option)
{
    switch (option)
    {
        case PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR:
            me->AddStartingItems();
            break;
        case PLAYER_BOT_AUTO_EQUIP_RANDOM_GEAR:
            EquipRandomGearInEmptySlots();
            break;
        case PLAYER_BOT_AUTO_EQUIP_PREMADE_GEAR:
            EquipPremadeGearTemplate();
            break;
    }

    UpdateVisualHonorRankBasedOnItems();
}

bool CombatBotBaseAI::CanTryToCastSpell(Unit const* pTarget, SpellEntry const* pSpellEntry) const
{
    if (m_preventCasting)
        return false;

    if (!me->IsSpellReady(pSpellEntry))
        return false;

    if (me->HasGCD(pSpellEntry))
        return false;

    if (pSpellEntry->TargetAuraState &&
       !pTarget->HasAuraState(AuraState(pSpellEntry->TargetAuraState)))
        return false;

    if (pSpellEntry->CasterAuraState &&
        !me->HasAuraState(AuraState(pSpellEntry->CasterAuraState)))
        return false;

    uint32 const powerCost = Spell::CalculatePowerCost(pSpellEntry, me);
    Powers const powerType = Powers(pSpellEntry->powerType);

    if (powerType == POWER_HEALTH)
    {
        if (me->GetHealth() <= powerCost)
            return false;
        return true;
    }

    if (me->GetPower(powerType) < powerCost)
        return false;

    if (pTarget->IsImmuneToSpell(pSpellEntry, false))
        return false;

    if (pSpellEntry->GetErrorAtShapeshiftedCast(me->GetShapeshiftForm()) != SPELL_CAST_OK)
        return false;

    if (pSpellEntry->IsSpellAppliesAura() && pTarget->HasAura(pSpellEntry->Id))
        return false;

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(pSpellEntry->rangeIndex);
    if (me != pTarget && pSpellEntry->EffectImplicitTargetA[0] != TARGET_UNIT_CASTER)
    {
        float const dist = me->GetCombatDistance(pTarget);

        if (dist > srange->maxRange)
            return false;
        if (srange->minRange && dist < srange->minRange)
            return false;
    }

    return true;
}

SpellCastResult CombatBotBaseAI::DoCastSpell(Unit* pTarget, SpellEntry const* pSpellEntry)
{
    if (m_preventCasting)
        return SPELL_FAILED_DONT_REPORT;

    if (me != pTarget)
        me->SetFacingToObject(pTarget);

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    me->SetTargetGuid(pTarget->GetObjectGuid());
    auto result = me->CastSpell(pTarget, pSpellEntry, false);

    //printf("cast %s result %u\n", pSpellEntry->SpellName[0].c_str(), result);

    if ((result == SPELL_FAILED_MOVING ||
        result == SPELL_CAST_OK) &&
        (pSpellEntry->GetCastTime(me) > 0) &&
        (me->IsMoving() || !me->IsStopped()))
        me->StopMoving();

    if ((result == SPELL_FAILED_NEED_AMMO_POUCH ||
        result == SPELL_FAILED_ITEM_NOT_READY) &&
        pSpellEntry->Reagent[0])
    {
        if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START))
            me->DestroyItem(INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START, true);

        AddItemToInventory(pSpellEntry->Reagent[0]);
    }

    return result;
}

void CombatBotBaseAI::AddItemToInventory(uint32 itemId, uint32 count)
{
    ItemPosCountVec dest;
    uint8 msg = me->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count);
    if (msg == EQUIP_ERR_OK)
    {
        if (Item* pItem = me->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId)))
            pItem->SetCount(count);
    }
}

void CombatBotBaseAI::AddHunterAmmo()
{
    if (Item* pWeapon = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
    {
        if (ItemPrototype const* pWeaponProto = pWeapon->GetProto())
        {
            if (pWeaponProto->Class == ITEM_CLASS_WEAPON)
            {
                uint32 ammoType;
                switch (pWeaponProto->SubClass)
                {
                    case ITEM_SUBCLASS_WEAPON_GUN:
                        ammoType = ITEM_SUBCLASS_BULLET;
                        break;
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                        ammoType = ITEM_SUBCLASS_ARROW;
                        break;
                    default:
                        return;
                }

                ItemPrototype const* pAmmoProto = nullptr;
                for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
                {
                    ItemPrototype const* pProto = &itr.second;

                    if (pProto->Class == ITEM_CLASS_PROJECTILE &&
                        pProto->SubClass == ammoType &&
                        pProto->RequiredLevel <= me->GetLevel() &&
                        (!pAmmoProto || pAmmoProto->ItemLevel < pProto->ItemLevel) &&
                        me->CanUseAmmo(pProto->ItemId) == EQUIP_ERR_OK)
                    {
                        pAmmoProto = pProto;
                    }
                }

                if (pAmmoProto)
                {
                    // Already carrying the right thing, so leave it alone. This is called once
                    // when a bot spawns now instead of every time a shot runs dry, and a
                    // summoned roster member arrives with a quiver it has been saving.
                    if (me->HasItemCount(pAmmoProto->ItemId, 1))
                    {
                        me->SetAmmo(pAmmoProto->ItemId);
                        return;
                    }

                    // Only ever clears out ammo it is replacing. Emptying the first bag slot
                    // whatever sits in it is survivable for a bot conjured seconds ago and is
                    // not for one that keeps what it earns.
                    Item* pFirstSlot = me->GetItemByPos(INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START);
                    if (pFirstSlot && pFirstSlot->GetProto()->Class == ITEM_CLASS_PROJECTILE)
                        me->DestroyItem(INVENTORY_SLOT_BAG_0, INVENTORY_SLOT_ITEM_START, true);

                    AddItemToInventory(pAmmoProto->ItemId, pAmmoProto->GetMaxStackSize());
                    me->SetAmmo(pAmmoProto->ItemId);
                }
            }
        }
    }
}

void CombatBotBaseAI::EquipOrUseNewItem()
{
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (pItem && !pItem->IsEquipped())
        {
            switch (pItem->GetProto()->Class)
            {
                case ITEM_CLASS_CONSUMABLE:
                {
                    SpellCastTargets targets;
                    targets.setUnitTarget(me);
                    me->CastItemUseSpell(pItem, targets);
                    break;
                }
                case ITEM_CLASS_WEAPON:
                case ITEM_CLASS_ARMOR:
                {
                    uint32 slot = me->FindEquipSlot(pItem->GetProto(), NULL_SLOT, true);
                    if (slot != NULL_SLOT)
                    {
                        if (Item* pItem2 = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                            me->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

                        // Learn required proficiency
                        if (uint32 proficiencySpellId = pItem->GetProto()->GetProficiencySpell())
                            if (!me->HasSpell(proficiencySpellId))
                                me->LearnSpell(proficiencySpellId, false, false);

                        me->RemoveItem(INVENTORY_SLOT_BAG_0, i, false);
                        me->EquipItem(slot, pItem, true);
                    }
                    break;
                }
            }
        }
    }
}

uint8 CombatBotBaseAI::GetHighestHonorRankFromEquippedItems() const
{
    uint8 maxRank = 0;
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->RequiredHonorRank > maxRank)
                maxRank = pItem->GetProto()->RequiredHonorRank;
        }
    }
    return maxRank;
}

void CombatBotBaseAI::UpdateVisualHonorRankBasedOnItems()
{
    uint8 rank = GetHighestHonorRankFromEquippedItems();
    if (rank > m_visualHonorRank)
        m_visualHonorRank = rank;

    // This is purely visual.
    me->SetByteValue(PLAYER_BYTES_3, PLAYER_BYTES_3_OFFSET_HONOR_RANK, m_visualHonorRank);
    me->SetByteValue(PLAYER_FIELD_BYTES, PLAYER_FIELD_BYTES_OFFSET_HIGHEST_HONOR_RANK, m_visualHonorRank);
}

void CombatBotBaseAI::BeginChasing(Unit* pVictim) const
{
    if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
        IsRangedDamageClass(me->GetClass()) &&
       !IsAttackSpeedOverridenForm(me->GetShapeshiftForm()) &&
       (me->GetPowerPercent(POWER_MANA) > 10.0f || me->GetWeaponForAttack(RANGED_ATTACK, true, true)))
        me->SetCasterChaseDistance(25.0f);
    else if (me->HasDistanceCasterMovement())
        me->SetCasterChaseDistance(0.0f);

    // we use dist = 1 always so we can specify angle, instead of spreading around target like mobs
    me->GetMotionMaster()->MoveChase(pVictim, 1.0f, m_role == ROLE_MELEE_DPS ? M_PI_F : 0.0f);
}

// A totem benefits the shaman's own party within this range of where it was planted, which is
// what makes the choice a question about the people standing nearby rather than about the shaman.
static float const TOTEM_AURA_RADIUS = 20.0f;

// Magma Totem only reaches enemies packed in close, so it is worth the slot over Searing Totem
// only when several of them are.
static float const MAGMA_TOTEM_RADIUS = 8.0f;

CombatBotBaseAI::TotemAudience CombatBotBaseAI::SurveyTotemAudience() const
{
    TotemAudience audience;

    // Counted by role rather than by class, because the class alone cannot tell a shaman that
    // stands in the front rank from one that heals from the back, and Windfury Totem is worth
    // nothing to the second. Counting by class would also have every shaman count itself as
    // melee and so make the answer the same for every group.
    auto const account = [&](Player const* pMember)
    {
        audience.members++;

        CombatBotRoles const role = GetEffectiveRole(pMember);
        if ((role == ROLE_MELEE_DPS || role == ROLE_TANK) && IsMeleeWeaponClass(pMember->GetClass()))
            audience.melee++;

        if (pMember->GetPowerType() == POWER_MANA)
            audience.manaUsers++;
    };

    Group* pGroup = me->GetGroup();
    if (!pGroup)
    {
        account(me);
        return audience;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || !pMember->IsAlive() || !me->IsWithinDist(pMember, TOTEM_AURA_RADIUS))
            continue;

        account(pMember);
    }

    return audience;
}

// The summon spell names a creature and the creature names the aura it pulses. Comparing on that
// aura is what lets two shamans notice they are running the same totem even at different ranks,
// which does not stack and so wastes one of them.
static uint32 GetTotemAuraSpellId(SpellEntry const* pSummonSpell)
{
    if (!pSummonSpell)
        return 0;

    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (pSummonSpell->Effect[i] < SPELL_EFFECT_SUMMON_TOTEM_SLOT1 ||
            pSummonSpell->Effect[i] > SPELL_EFFECT_SUMMON_TOTEM_SLOT4)
            continue;

        if (CreatureInfo const* pInfo = sObjectMgr.GetCreatureTemplate(pSummonSpell->EffectMiscValue[i]))
            return pInfo->totem_spell_id;
    }

    return 0;
}

bool CombatBotBaseAI::IsTotemSpellCoveredByAnotherShaman(TotemSlot slot, SpellEntry const* pSpellEntry) const
{
    uint32 const auraSpellId = GetTotemAuraSpellId(pSpellEntry);
    if (!auraSpellId)
        return false;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return false;

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me || pMember->GetClass() != CLASS_SHAMAN)
            continue;

        if (!me->IsWithinDist(pMember, TOTEM_AURA_RADIUS))
            continue;

        Totem* pTotem = pMember->GetTotem(slot);
        if (!pTotem)
            continue;

        uint32 const otherAuraSpellId = pTotem->GetSpell();
        if (otherAuraSpellId == auraSpellId)
            return true;

        if (SpellEntry const* pAuraSpell = sSpellMgr.GetSpellEntry(auraSpellId))
            if (sSpellMgr.IsRankSpellDueToSpell(pAuraSpell, otherAuraSpellId))
                return true;
    }

    return false;
}

SpellEntry const* CombatBotBaseAI::SelectTotemForSlot(TotemSlot slot) const
{
    TotemAudience const audience = SurveyTotemAudience();

    SpellEntry const* choices[6] = {};
    uint32 choiceCount = 0;
    auto const consider = [&](SpellEntry const* pSpellEntry)
    {
        if (!pSpellEntry || choiceCount >= 6)
            return;

        for (uint32 i = 0; i < choiceCount; ++i)
            if (choices[i] == pSpellEntry)
                return;

        choices[choiceCount++] = pSpellEntry;
    };

    // Anything missing from these lists is a totem that only earns its slot as a deliberate call
    // for a particular fight. Leaving them out is the point: Fire Resistance and Disease Cleansing
    // sat in the water pool next to Mana Spring and were exactly as likely to be picked.
    switch (slot)
    {
        case TOTEM_SLOT_AIR:
            if (audience.melee)
                consider(m_totems.pWindfury);
            consider(m_totems.pGraceOfAir);
            consider(m_totems.pWindfury);
            consider(m_totems.pTranquilAir);
            consider(m_totems.pNatureResistance);
            consider(m_totems.pWindwall);
            break;
        case TOTEM_SLOT_EARTH:
            if (audience.melee)
                consider(m_totems.pStrengthOfEarth);
            consider(m_totems.pStoneskin);
            consider(m_totems.pStrengthOfEarth);
            consider(m_totems.pTremor);
            consider(m_totems.pStoneclaw);
            break;
        case TOTEM_SLOT_FIRE:
            // Searing and Magma are the shaman's own damage, so they only earn the slot when
            // there is something to shoot at.
            if (me->GetVictim())
            {
                if (GetAttackersInRangeCount(MAGMA_TOTEM_RADIUS) >= 3)
                    consider(m_totems.pMagma);
                consider(m_totems.pSearing);
                consider(m_totems.pMagma);
            }
            consider(m_totems.pFlametongue);
            break;
        case TOTEM_SLOT_WATER:
            if (audience.manaUsers)
                consider(m_totems.pManaSpring);
            consider(m_totems.pHealingStream);
            consider(m_totems.pManaSpring);
            break;
    }

    for (uint32 i = 0; i < choiceCount; ++i)
        if (!IsTotemSpellCoveredByAnotherShaman(slot, choices[i]))
            return choices[i];

    // Everything preferred is already covered by another shaman, so the slot is better spent on
    // the second choice than left empty.
    return choiceCount ? choices[0] : nullptr;
}

bool CombatBotBaseAI::SummonShamanTotems()
{
    // Asked per school at the moment a totem is dropped rather than reused from a choice made when
    // the bot learned its spells, because by now there is a group standing around it.
    static TotemSlot const totemSlots[] = { TOTEM_SLOT_AIR, TOTEM_SLOT_EARTH, TOTEM_SLOT_FIRE, TOTEM_SLOT_WATER };
    for (TotemSlot slot : totemSlots)
    {
        if (me->GetTotem(slot))
            continue;

        SpellEntry const* pTotemSpell = SelectTotemForSlot(slot);
        if (!pTotemSpell || !CanTryToCastSpell(me, pTotemSpell))
            continue;

        if (DoCastSpell(me, pTotemSpell) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

SpellCastResult CombatBotBaseAI::CastWeaponBuff(SpellEntry const* pSpellEntry, EquipmentSlots slot)
{
    Item* pWeapon = me->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!pWeapon)
        return SPELL_FAILED_ITEM_NOT_FOUND;
    if (pWeapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        return SPELL_FAILED_ITEM_ALREADY_ENCHANTED;

    // Cast for real rather than triggered. Triggered skips the mana cost, the global cooldown,
    // range and line of sight, silence and school lockouts, and reagent consumption, so a shaman
    // imbued its weapon for nothing and instantly. Enhancement throughput measured against that
    // is measured against a shaman with a mana cost fewer than it has.
    Spell* spell = new Spell(me, pSpellEntry, false, ObjectGuid(), nullptr, nullptr, nullptr);
    SpellCastTargets targets;
    targets.setItemTarget(pWeapon);
    return spell->prepare(std::move(targets), nullptr);
}

bool CombatBotBaseAI::UseTrinketEffects(bool onlyToBreakCC)
{
    if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1))
        if (UseItemEffect(pItem, onlyToBreakCC))
            return true;
    if (Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2))
        if (UseItemEffect(pItem, onlyToBreakCC))
            return true;

    return false;
}

bool CombatBotBaseAI::UseItemEffect(Item* pItem, bool onlyToBreakCC)
{
    ItemPrototype const* pProto = pItem->GetProto();
    for (auto const& itr : pProto->Spells)
    {
        if (itr.SpellId && itr.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
        {
            if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(itr.SpellId))
            {
                if (me->IsSpellReady(pSpellEntry, pProto))
                {
                    if (onlyToBreakCC && !pSpellEntry->HasAttribute(SPELL_ATTR_EX_IMMUNITY_PURGES_EFFECT))
                        continue;

                    if (pSpellEntry->IsPositiveSpell())
                        return me->CastSpell(me, pSpellEntry, false, pItem) == SPELL_CAST_OK;
                    else if (me->GetVictim())
                        return me->CastSpell(me->GetVictim(), pSpellEntry, false, pItem) == SPELL_CAST_OK;
                }
            }
        }
    }
    return false;
}

void CombatBotBaseAI::BreakCrowdControlEffects()
{
    if (UseTrinketEffects(true))
        return;

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pDivineShield &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
            {
                if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
                {
                    if (m_role != ROLE_HEALER)
                    {
                        me->m_Events.AddLambdaEventAtOffset([player = me, spellId = m_spells.paladin.pDivineShield->Id]()
                        {
                            if (player->GetHealthPercent() > 75.0f && player->GetAttackers().size() < 3)
                                player->RemoveAurasDueToSpellByCancel(spellId);
                        }, 1 * IN_MILLISECONDS);
                    }
                    return;
                }
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (me->HasUnitState(UNIT_STATE_STUNNED) && m_spells.mage.pBlink &&
                CanTryToCastSpell(me, m_spells.mage.pBlink))
            {
                if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return;
            }
            if (m_spells.mage.pIceBlock &&
                CanTryToCastSpell(me, m_spells.mage.pIceBlock))
            {
                if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                {
                    me->m_Events.AddLambdaEventAtOffset([player = me, spellId = m_spells.mage.pIceBlock->Id]()
                    {
                        if (player->GetHealthPercent() > 75.0f && player->GetAttackers().size() < 3)
                            player->RemoveAurasDueToSpellByCancel(spellId);
                    }, 1 * IN_MILLISECONDS);
                    return;
                }
            }
            break;
        }
        case CLASS_DRUID:
        {
            bool polymorphed = false;
            auto const& auraList = me->GetAurasByType(SPELL_AURA_MOD_CONFUSE);
            for (auto const& pAura : auraList)
            {
                if (pAura->GetSpellProto()->Mechanic == MECHANIC_POLYMORPH)
                {
                    polymorphed = true;
                    break;
                }
            }

            if (polymorphed)
            {
                SpellEntry const* pShapeshift = nullptr;

                if (m_role == ROLE_TANK && m_spells.druid.pBearForm && CanTryToCastSpell(me, m_spells.druid.pBearForm))
                    pShapeshift = m_spells.druid.pBearForm;
                else if (m_role == ROLE_MELEE_DPS && m_spells.druid.pCatForm && CanTryToCastSpell(me, m_spells.druid.pCatForm))
                    pShapeshift = m_spells.druid.pCatForm;
                else if (m_role == ROLE_RANGE_DPS && m_spells.druid.pMoonkinForm && CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
                    pShapeshift = m_spells.druid.pMoonkinForm;
                else
                {
                    for (auto const& pSpell : m_spells.raw.spells)
                    {
                        if (pSpell && pSpell->HasAura(SPELL_AURA_MOD_SHAPESHIFT) && CanTryToCastSpell(me, pSpell))
                        {
                            pShapeshift = pSpell;
                            break;
                        }
                    }
                }

                if (pShapeshift && DoCastSpell(me, pShapeshift) == SPELL_CAST_OK)
                {
                    if (m_role == ROLE_HEALER)
                    {
                        me->m_Events.AddLambdaEventAtOffset([player = me, spellId = pShapeshift->Id]()
                        {
                            player->RemoveAurasDueToSpellByCancel(spellId);
                        }, 1 * IN_MILLISECONDS);
                    }
                    return;
                }
            }
            break;
        }
    }
}

bool CombatBotBaseAI::IsWearingShield(Player* pPlayer) const
{
    Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!pItem)
        return false;

    if (pItem->GetProto()->InventoryType == INVTYPE_SHIELD)
        return true;

    return false;
}

bool CombatBotBaseAI::IsInDuel() const
{
    return me->m_duel && me->m_duel->startTime != 0;
}

CombatBotRoles CombatBotBaseAI::GetRole() const
{
    if (m_role == ROLE_HEALER && IsInDuel())
    {
        if (IsMeleeDamageClass(me->GetClass()))
            return ROLE_MELEE_DPS;
        else
            return ROLE_RANGE_DPS;
    }

    return m_role;
}

void CombatBotBaseAI::SendBattlefieldPortPacket()
{
    for (uint32 i = BATTLEGROUND_QUEUE_AV; i <= BATTLEGROUND_QUEUE_AB; i++)
    {
        if (me->IsInvitedForBattleGroundQueueType(static_cast<BattleGroundQueueTypeId>(i)))
        {
            WorldPackets::Battleground::BattleFieldPort packet;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
            packet.mapId = GetBattleGrounMapIdByTypeId(static_cast<BattleGroundTypeId>(i));
#endif
            packet.action = 1;
            me->GetSession()->HandleBattleFieldPortOpcode(packet);
            break;
        }
    }
}

void CombatBotBaseAI::SendBattlemasterJoinPacket(uint8 battlegroundId)
{
    uint32 instanceId = 0; // first available
    uint32 mapId;
    switch (battlegroundId)
    {
        case BATTLEGROUND_QUEUE_AV:
            mapId = MAP_ALTERAC_VALLEY;
            break;
        case BATTLEGROUND_QUEUE_WS:
            mapId = MAP_WARSONG_GULCH;
            break;
        case BATTLEGROUND_QUEUE_AB:
            mapId = MAP_ARATHI_BASIN;
            break;
        default:
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "BattleBot: Invalid BG queue type!");
            botEntry->requestRemoval = true;
            return;
    }

    me->GetSession()->RequestBgJoinQueue(me->GetObjectGuid(), instanceId, mapId, false);
}

void CombatBotBaseAI::SendAreaTriggerPacket(uint32 areaTriggerId)
{
    WorldPackets::Misc::AreaTrigger packet;
    packet.triggerId = areaTriggerId;
    me->GetSession()->HandleAreaTriggerOpcode(packet);
}

void CombatBotBaseAI::ActivateNearbyAreaTrigger()
{
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* pTrigger = &itr.second;
        if (!pTrigger)
            continue;

        if (!IsPointInAreaTriggerZone(pTrigger, me->GetMapId(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 5.0f))
            continue;

        // Every one of them, not the lowest numbered. Triggers overlap, and at the Orb of
        // Command two sit within a foot of each other: the lower is inert and the higher is the
        // only way a ghost gets back into Blackwing Lair. Stopping at the first found means the
        // bot reports the wrong one and walks away from a run it had already completed.
        //
        // Sending several is safe and is what a client does. The handler re-tests range when it
        // processes each packet, so once one of them moves the bot the rest are ignored.
        SendAreaTriggerPacket(pTrigger->id);
    }
}

void CombatBotBaseAI::OnPacketReceived(WorldPacket const* packet)
{
    // Must always check "me" player pointer here!
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
        case SMSG_LOGIN_SETTIMESPEED:
        {
            if (!me)
                return;

            UpdateVisualHonorRankBasedOnItems();
            break;
        }
        case SMSG_TRADE_STATUS:
        {
            if (!me)
                return;

            uint32 status = *((uint32*)(*packet).contents());
            if (status == TRADE_STATUS_BEGIN_TRADE)
            {
                auto data = std::make_unique<NullClientPacket>(CMSG_BEGIN_TRADE);
                me->GetSession()->QueuePacket(std::move(data));
            }
            else if (status == TRADE_STATUS_TRADE_ACCEPT)
            {
                auto data = std::make_unique<WorldPackets::Trade::AcceptTrade>();
                me->GetSession()->QueuePacket(std::move(data));
            }
            else if (status == TRADE_STATUS_TRADE_COMPLETE)
            {
                EquipOrUseNewItem();
                UpdateVisualHonorRankBasedOnItems();
            }
            break;
        }
        case SMSG_RESURRECT_REQUEST:
        {
            if (!me)
                return;

            auto data = std::make_unique<WorldPackets::Misc::ResurrectResponse>();
            data->resurrectorGuid = me->GetResurrector();
            data->accept = true;
            me->GetSession()->QueuePacket(std::move(data));
            break;
        }
        case SMSG_BATTLEFIELD_STATUS:
        {
            if (!me)
                return;

            if (me->IsBeingTeleported() || me->InBattleGround())
                m_receivedBgInvite = false;
            else
            {
                for (uint32 i = BATTLEGROUND_QUEUE_AV; i <= BATTLEGROUND_QUEUE_AB; i++)
                {
                    if (me->IsInvitedForBattleGroundQueueType(BattleGroundQueueTypeId(i)))
                    {
                        m_receivedBgInvite = true;
                        break;
                    }
                }
            }
            return;
        }
        case SMSG_LOOT_START_ROLL:
        {
            if (!me)
                return;

            uint64 guid = *((uint64*)(*packet).contents());
            uint32 slot = *(((uint32*)(*packet).contents()) + 2);

            auto data = std::make_unique<WorldPackets::Loot::LootRoll>();
            data->lootedTarget = ObjectGuid(guid);
            data->itemSlot = slot;
            data->rollType = ROLL_PASS;
            me->GetSession()->QueuePacket(std::move(data));
            return;
        }
    }

    // Teleport acknowledgement lives there, so that every headless session can change maps
    // rather than only the ones that also fight.
    PlayerBotAI::OnPacketReceived(packet);
}
