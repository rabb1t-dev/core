/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "ItemEvaluator.h"
#include "ItemPrototype.h"
#include "Item.h"
#include "Bag.h"
#include "ItemDefines.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "SpellEntry.h"
#include "SpellAuraDefines.h"
#include "SpellDefines.h"
#include "Database/DBCStores.h"
#include "Database/DBCEnums.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Policies/SingletonImp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace
{
    // Defense is SkillLine 95 in SkillLine.dbc. Weapon skills for "Weapon" sheet rows are
    // every other skill the spell happens to touch; collapsing them into one column is what
    // the Classic Gear Ranker does and what our Weight schema therefore expects.
    uint32 const DEFENSE_SKILL_LINE = 95;

    uint32 SkillFromSpell(uint32 miscValue)
    {
        return uint32(miscValue);
    }

    bool IsRangedWeaponSkill(uint32 skill)
    {
        switch (skill)
        {
            case 45:  // bows
            case 46:  // guns
            case 176: // thrown
            case 226: // crossbows
            case 228: // wands
                return true;
            default:
                return false;
        }
    }

    bool IsMeleeWeaponSkill(uint32 skill)
    {
        switch (skill)
        {
            case 43:  // swords
            case 44:  // axes
            case 54:  // maces
            case 55:  // two-handed swords
            case 136: // staves
            case 160: // two-handed maces
            case 162: // unarmed
            case 172: // two-handed axes
            case 173: // daggers
            case 229: // polearms
            case 473: // fist weapons
                return true;
            default:
                return false;
        }
    }

    // All-schools spell damage (Classic Gear Ranker's SpDmg). Single-school bonuses are
    // deliberately left out of that column; the sheet does not record them and a frost
    // mage's preference for frost damage is a later refinement.
    bool IsAllSchoolsMagic(int32 misc)
    {
        if (misc < 0)
            return true;
        uint32 const mask = uint32(misc);
        return (mask & SPELL_SCHOOL_MASK_SPELL) == SPELL_SCHOOL_MASK_SPELL
            || (mask & SPELL_SCHOOL_MASK_MAGIC) == SPELL_SCHOOL_MASK_MAGIC;
    }

    std::string WeightKey(uint8 classId, std::string const& spec)
    {
        return std::to_string(uint32(classId)) + ":" + spec;
    }
}

INSTANTIATE_SINGLETON_1(ItemEvaluator);

void ItemEvaluator::Load()
{
    m_weights.clear();

    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT `class`, `spec`,"
        " `avg_hit`, `dps`, `speed`, `armor`, `stam`, `spi`, `intellect`, `str`, `agi`,"
        " `ap`, `hit`, `crit`, `weapon_skill`, `defense`, `dodge`, `parry`, `block`,"
        " `block_value`, `ranged_ap`, `spdmg`, `sppen`, `sphit`, `spcrit`, `spheal`, `mp5`,"
        " `fire_res`, `nat_res`, `frost_res`,"
        " `hit_cap`, `sphit_cap`, `weapon_skill_cap`"
        " FROM `raidguild_stat_weight`"));

    if (!result)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded 0 raidguild_stat_weight rows. Scoring will return zero.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        StatWeights w;
        w.classId = fields[0].GetUInt8();
        w.spec = fields[1].GetCppString();
        w.avg_hit = fields[2].GetFloat();
        w.dps = fields[3].GetFloat();
        w.speed = fields[4].GetFloat();
        w.armor = fields[5].GetFloat();
        w.stam = fields[6].GetFloat();
        w.spi = fields[7].GetFloat();
        w.intellect = fields[8].GetFloat();
        w.str = fields[9].GetFloat();
        w.agi = fields[10].GetFloat();
        w.ap = fields[11].GetFloat();
        w.hit = fields[12].GetFloat();
        w.crit = fields[13].GetFloat();
        w.weapon_skill = fields[14].GetFloat();
        w.defense = fields[15].GetFloat();
        w.dodge = fields[16].GetFloat();
        w.parry = fields[17].GetFloat();
        w.block = fields[18].GetFloat();
        w.block_value = fields[19].GetFloat();
        w.ranged_ap = fields[20].GetFloat();
        w.spdmg = fields[21].GetFloat();
        w.sppen = fields[22].GetFloat();
        w.sphit = fields[23].GetFloat();
        w.spcrit = fields[24].GetFloat();
        w.spheal = fields[25].GetFloat();
        w.mp5 = fields[26].GetFloat();
        w.fire_res = fields[27].GetFloat();
        w.nat_res = fields[28].GetFloat();
        w.frost_res = fields[29].GetFloat();
        w.hit_cap = fields[30].GetFloat();
        w.sphit_cap = fields[31].GetFloat();
        w.weapon_skill_cap = fields[32].GetFloat();

        m_weights[WeightKey(w.classId, w.spec)] = w;
        ++count;
    }
    while (result->NextRow());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u raidguild_stat_weight rows", count);
}

bool ItemEvaluator::HasWeights(uint8 classId, std::string const& spec) const
{
    return m_weights.find(WeightKey(classId, spec)) != m_weights.end();
}

StatWeights const* ItemEvaluator::GetWeights(uint8 classId, std::string const& spec) const
{
    auto itr = m_weights.find(WeightKey(classId, spec));
    if (itr != m_weights.end())
        return &itr->second;

    // A roster member whose authored spec has no row yet still needs a score so that
    // OptimizeEquipment has something to do. Any weight for the same class is closer to
    // right than equating every item to zero.
    //
    // Which row, though, has to be decided rather than left to the hash order: two members
    // of the same class with an unauthored spec picking different fallbacks, and picking
    // differently again after a restart, is a bug that only shows up as gear drifting for no
    // stated reason. Lowest spec name wins, and the miss is logged so the gap gets an
    // authored row rather than living on as a silent default.
    StatWeights const* fallback = nullptr;
    for (auto const& kv : m_weights)
    {
        if (kv.second.classId != classId)
            continue;
        if (!fallback || kv.second.spec < fallback->spec)
            fallback = &kv.second;
    }

    if (fallback)
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "ItemEvaluator: no raidguild_stat_weight row for class %u spec '%s', scoring as '%s'",
            uint32(classId), spec.c_str(), fallback->spec.c_str());

    return fallback;
}

float ItemEvaluator::Score(ResolvedStats const& s, StatWeights const& w) const
{
    return s.avg_hit * w.avg_hit
         + s.dps * w.dps
         + s.speed * w.speed
         + float(s.armor) * w.armor
         + float(s.stam) * w.stam
         + float(s.spi) * w.spi
         + float(s.intellect) * w.intellect
         + float(s.str) * w.str
         + float(s.agi) * w.agi
         + float(s.ap) * w.ap
         + float(s.hit) * w.hit
         + float(s.crit) * w.crit
         + float(s.weapon_skill) * w.weapon_skill
         + float(s.defense) * w.defense
         + float(s.dodge) * w.dodge
         + float(s.parry) * w.parry
         + float(s.block) * w.block
         + float(s.block_value) * w.block_value
         + float(s.ranged_ap) * w.ranged_ap
         + float(s.spdmg) * w.spdmg
         + float(s.sppen) * w.sppen
         + float(s.sphit) * w.sphit
         + float(s.spcrit) * w.spcrit
         + float(s.spheal) * w.spheal
         + float(s.mp5) * w.mp5
         + float(s.fire_res) * w.fire_res
         + float(s.nat_res) * w.nat_res
         + float(s.frost_res) * w.frost_res;
}

namespace
{
    // Field by field, with one exception. Weapon skill is capped to the largest single
    // contribution *within* an item, because one item granting +7 to axes, daggers and
    // swords is worth 7 rather than 21. Across items it genuinely stacks: Edgemaster's +7 to
    // swords and a sword with +5 sword skill is +12 to the weapon in hand. So the within-item
    // rule lives in ApplySpell and this adds, which is why set bonuses have to be resolved
    // into their own vector before being added rather than applied onto a running total.
    void AddStats(ResolvedStats& dest, ResolvedStats const& src)
    {
        dest.armor += src.armor;
        dest.stam += src.stam;
        dest.spi += src.spi;
        dest.intellect += src.intellect;
        dest.str += src.str;
        dest.agi += src.agi;

        dest.ap += src.ap;
        dest.hit += src.hit;
        dest.crit += src.crit;
        dest.weapon_skill += src.weapon_skill;
        dest.defense += src.defense;
        dest.dodge += src.dodge;
        dest.parry += src.parry;
        dest.block += src.block;
        dest.block_value += src.block_value;
        dest.ranged_ap += src.ranged_ap;

        dest.spdmg += src.spdmg;
        dest.sppen += src.sppen;
        dest.sphit += src.sphit;
        dest.spcrit += src.spcrit;
        dest.spheal += src.spheal;
        dest.mp5 += src.mp5;

        dest.fire_res += src.fire_res;
        dest.nat_res += src.nat_res;
        dest.frost_res += src.frost_res;
        dest.shadow_res += src.shadow_res;
        dest.arcane_res += src.arcane_res;
        dest.holy_res += src.holy_res;

        dest.avg_hit += src.avg_hit;
        dest.dps += src.dps;
        dest.speed += src.speed;
    }

    void ApplyCaps(ResolvedStats& total, StatWeights const& w)
    {
        if (w.hit_cap > 0.0f && float(total.hit) > w.hit_cap)
            total.hit = int32(w.hit_cap);
        if (w.sphit_cap > 0.0f && float(total.sphit) > w.sphit_cap)
            total.sphit = int32(w.sphit_cap);
        if (w.weapon_skill_cap > 0.0f && float(total.weapon_skill) > w.weapon_skill_cap)
            total.weapon_skill = int32(w.weapon_skill_cap);
    }

    void CountSetPieces(std::vector<Item*> const& worn, std::unordered_map<uint32, uint32>& counts)
    {
        for (Item* pItem : worn)
        {
            if (!pItem || !pItem->GetProto())
                continue;
            if (uint32 const setId = pItem->GetProto()->ItemSet)
                ++counts[setId];
        }
    }

    void CountSetPieces(std::vector<ItemPrototype const*> const& worn,
        std::unordered_map<uint32, uint32>& counts)
    {
        for (ItemPrototype const* pProto : worn)
        {
            if (!pProto)
                continue;
            if (uint32 const setId = pProto->ItemSet)
                ++counts[setId];
        }
    }
}

ResolvedStats ItemEvaluator::SetBonus(uint32 setId, uint32 count, Player const* pPlayer) const
{
    ResolvedStats bonus;
    if (!setId || !count)
        return bonus;

    ItemSetEntry const* pSet = sItemSetStore.LookupEntry(setId);
    if (!pSet)
        return bonus;

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    // The same gate AddItemsSetItem applies. A profession set whose skill the member does
    // not have grants nothing, and scoring it as though it did would have the bot assemble
    // a set it cannot benefit from.
    if (pSet->required_skill_id && pPlayer
        && pPlayer->GetSkillValue(pSet->required_skill_id) < pSet->required_skill_value)
        return bonus;
#endif

    for (uint32 i = 0; i < 8; ++i)
    {
        if (!pSet->spells[i])
            continue;
        if (pSet->items_to_triggerspell[i] > count)
            continue;

        // Into its own vector and then added, so the within-item weapon skill rule in
        // ApplySpell cannot swallow a set bonus that stacks with what the pieces already
        // give.
        ResolvedStats one;
        ApplySpell(one, sSpellMgr.GetSpellEntry(pSet->spells[i]));
        AddStats(bonus, one);
    }

    return bonus;
}

ResolvedStats ItemEvaluator::ResolveLoadout(std::vector<Item*> const& worn, Player const* pPlayer) const
{
    ResolvedStats total;
    for (Item* pItem : worn)
    {
        if (!pItem)
            continue;
        ResolvedStats const one = ResolveItem(pItem->GetProto(), pItem);
        AddStats(total, one);
    }

    std::unordered_map<uint32, uint32> counts;
    CountSetPieces(worn, counts);
    for (auto const& kv : counts)
        AddStats(total, SetBonus(kv.first, kv.second, pPlayer));

    return total;
}

ResolvedStats ItemEvaluator::ResolveLoadout(std::vector<ItemPrototype const*> const& worn,
    Player const* pPlayer) const
{
    ResolvedStats total;
    for (ItemPrototype const* pProto : worn)
    {
        if (!pProto)
            continue;
        ResolvedStats const one = ResolveItem(pProto);
        AddStats(total, one);
    }

    std::unordered_map<uint32, uint32> counts;
    CountSetPieces(worn, counts);
    for (auto const& kv : counts)
        AddStats(total, SetBonus(kv.first, kv.second, pPlayer));

    return total;
}

float ItemEvaluator::ScoreLoadout(ResolvedStats const& total, StatWeights const& w) const
{
    ResolvedStats capped = total;
    ApplyCaps(capped, w);
    return Score(capped, w);
}

void ItemEvaluator::ApplyWeaponShape(ResolvedStats& stats, ItemPrototype const* pProto) const
{
    if (pProto->Class != ITEM_CLASS_WEAPON)
        return;

    float dmgMin = 0.0f;
    float dmgMax = 0.0f;
    for (uint32 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
    {
        dmgMin += pProto->Damage[i].DamageMin;
        dmgMax += pProto->Damage[i].DamageMax;
    }

    if (dmgMax <= 0.0f)
        return;

    stats.avg_hit = (dmgMin + dmgMax) * 0.5f;
    stats.speed = pProto->Delay > 0 ? (float(pProto->Delay) / 1000.0f) : 0.0f;
    if (stats.speed > 0.0f)
        stats.dps = stats.avg_hit / stats.speed;
}

void ItemEvaluator::ApplySpell(ResolvedStats& stats, SpellEntry const* pSpell) const
{
    if (!pSpell)
        return;

    // Classic Gear Ranker's convention, and the one every vanilla BiS list uses: a single
    // equip spell that grants both spell damage and healing of the same amount is "+N spell
    // damage and healing" and is counted once as spell damage. Crediting it as both would
    // double its value under any weight that cares about both columns. The same for attack
    // power that also grants ranged attack power.
    bool hasSpellDamage = false;
    bool hasHealing = false;
    bool hasMeleeAP = false;
    bool hasRangedAP = false;
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (!pSpell->Effect[i])
            continue;
        switch (pSpell->EffectApplyAuraName[i])
        {
            case SPELL_AURA_MOD_DAMAGE_DONE:
            {
                int32 const misc = pSpell->EffectMiscValue[i];
                if (IsAllSchoolsMagic(misc))
                    hasSpellDamage = true;
                break;
            }
            case SPELL_AURA_MOD_HEALING_DONE:
                hasHealing = true;
                break;
            case SPELL_AURA_MOD_ATTACK_POWER:
                hasMeleeAP = true;
                break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                hasRangedAP = true;
                break;
            default:
                break;
        }
    }
    bool const dualSpellPower = hasSpellDamage && hasHealing;
    bool const dualAttackPower = hasMeleeAP && hasRangedAP;

    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (!pSpell->Effect[i])
            continue;

        int32 const amount = pSpell->CalculateSimpleValue(SpellEffectIndex(i));
        uint32 const aura = pSpell->EffectApplyAuraName[i];
        int32 const misc = pSpell->EffectMiscValue[i];

        switch (aura)
        {
            case SPELL_AURA_MOD_DAMAGE_DONE:
            {
                if (IsAllSchoolsMagic(misc))
                    stats.spdmg += amount;
                break;
            }
            case SPELL_AURA_MOD_HEALING_DONE:
                if (!dualSpellPower)
                    stats.spheal += amount;
                break;
            case SPELL_AURA_MOD_ATTACK_POWER:
                stats.ap += amount;
                break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                if (!dualAttackPower)
                    stats.ranged_ap += amount;
                break;
            case SPELL_AURA_MOD_CRIT_PERCENT:
                stats.crit += amount;
                break;
            case SPELL_AURA_MOD_HIT_CHANCE:
                stats.hit += amount;
                break;
            case SPELL_AURA_MOD_SPELL_HIT_CHANCE:
                stats.sphit += amount;
                break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL:
                stats.spcrit += amount;
                break;
            case SPELL_AURA_MOD_DODGE_PERCENT:
                stats.dodge += amount;
                break;
            case SPELL_AURA_MOD_PARRY_PERCENT:
                stats.parry += amount;
                break;
            case SPELL_AURA_MOD_BLOCK_PERCENT:
                stats.block += amount;
                break;
            case SPELL_AURA_MOD_SHIELD_BLOCKVALUE:
                stats.block_value += amount;
                break;
            case SPELL_AURA_MOD_BLOCK_SKILL:
                // Classic Gear Ranker files "Block Value" for aura 50 on some patch-old
                // rows that were later rewritten as SHIELD_BLOCKVALUE. Crediting the
                // amount as block value matches the oracle and is harmless: the skill
                // form is tiny and the replacement is exactly block value.
                stats.block_value += amount;
                break;
            case SPELL_AURA_MOD_POWER_REGEN:
            case SPELL_AURA_PERIODIC_ENERGIZE:
                if (misc == 0)
                    stats.mp5 += amount;
                break;
            case SPELL_AURA_MOD_TARGET_RESISTANCE:
            {
                uint32 const mask = misc < 0 ? SPELL_SCHOOL_MASK_MAGIC : uint32(misc);
                if ((mask & SPELL_SCHOOL_MASK_MAGIC) && amount < 0)
                    stats.sppen += -amount;
                break;
            }
            case SPELL_AURA_MOD_RESISTANCE:
            {
                auto addRes = [&](int32& dest)
                {
                    dest += amount;
                };

                if (misc < 0)
                {
                    addRes(stats.fire_res);
                    addRes(stats.nat_res);
                    addRes(stats.frost_res);
                    addRes(stats.shadow_res);
                    addRes(stats.arcane_res);
                    addRes(stats.holy_res);
                }
                else
                {
                    switch (misc)
                    {
                        case SPELL_SCHOOL_HOLY:   addRes(stats.holy_res); break;
                        case SPELL_SCHOOL_FIRE:   addRes(stats.fire_res); break;
                        case SPELL_SCHOOL_NATURE: addRes(stats.nat_res); break;
                        case SPELL_SCHOOL_FROST:  addRes(stats.frost_res); break;
                        case SPELL_SCHOOL_SHADOW: addRes(stats.shadow_res); break;
                        case SPELL_SCHOOL_ARCANE: addRes(stats.arcane_res); break;
                        default: break;
                    }
                }
                break;
            }
            case SPELL_AURA_MOD_SKILL:
            case SPELL_AURA_MOD_SKILL_TALENT:
            {
                uint32 const skill = SkillFromSpell(uint32(misc));
                if (skill == DEFENSE_SKILL_LINE)
                    stats.defense += amount;
                else if (IsMeleeWeaponSkill(skill) || IsRangedWeaponSkill(skill))
                    // Highest wins rather than the sum: a character uses one weapon, so
                    // Edgemaster's Handguards granting +7 to axes, daggers and swords is
                    // worth +7, not +21. Bow skill sits in the same column as axe skill
                    // because it is the same stat pointed at a different weapon; it is
                    // emphatically not ranged attack power, which is worth far less per
                    // point and lives in its own field.
                    stats.weapon_skill = std::max(stats.weapon_skill, amount);
                break;
            }
            default:
                break;
        }
    }
}

void ItemEvaluator::ApplyEnchantment(ResolvedStats& stats, SpellItemEnchantmentEntry const* pEnchant) const
{
    if (!pEnchant)
        return;

    for (uint32 s = 0; s < 3; ++s)
    {
        switch (pEnchant->type[s])
        {
            case ITEM_ENCHANTMENT_TYPE_NONE:
            case ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL:
            case ITEM_ENCHANTMENT_TYPE_DAMAGE:
            case ITEM_ENCHANTMENT_TYPE_TOTEM:
                break;
            case ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL:
                if (pEnchant->spellid[s])
                    ApplySpell(stats, sSpellMgr.GetSpellEntry(pEnchant->spellid[s]));
                break;
            case ITEM_ENCHANTMENT_TYPE_RESISTANCE:
            {
                int32 const amount = int32(pEnchant->amount[s]);
                switch (pEnchant->spellid[s])
                {
                    case SPELL_SCHOOL_HOLY:   stats.holy_res += amount; break;
                    case SPELL_SCHOOL_FIRE:   stats.fire_res += amount; break;
                    case SPELL_SCHOOL_NATURE: stats.nat_res += amount; break;
                    case SPELL_SCHOOL_FROST:  stats.frost_res += amount; break;
                    case SPELL_SCHOOL_SHADOW: stats.shadow_res += amount; break;
                    case SPELL_SCHOOL_ARCANE: stats.arcane_res += amount; break;
                    default: break;
                }
                break;
            }
            case ITEM_ENCHANTMENT_TYPE_STAT:
            {
                int32 const amount = int32(pEnchant->amount[s]);
                switch (pEnchant->spellid[s])
                {
                    case ITEM_MOD_AGILITY:   stats.agi += amount; break;
                    case ITEM_MOD_STRENGTH:  stats.str += amount; break;
                    case ITEM_MOD_INTELLECT: stats.intellect += amount; break;
                    case ITEM_MOD_SPIRIT:    stats.spi += amount; break;
                    case ITEM_MOD_STAMINA:   stats.stam += amount; break;
                    default: break;
                }
                break;
            }
            default:
                break;
        }
    }
}

ResolvedStats ItemEvaluator::ResolveSpell(uint32 spellId) const
{
    ResolvedStats stats;
    ApplySpell(stats, sSpellMgr.GetSpellEntry(spellId));
    return stats;
}

ResolvedStats ItemEvaluator::ResolveItem(ItemPrototype const* pProto, Item const* pItem) const
{
    ResolvedStats stats;
    if (!pProto)
        return stats;

    for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        int32 const value = pProto->ItemStat[i].ItemStatValue;
        switch (pProto->ItemStat[i].ItemStatType)
        {
            case ITEM_MOD_AGILITY:   stats.agi += value; break;
            case ITEM_MOD_STRENGTH:  stats.str += value; break;
            case ITEM_MOD_INTELLECT: stats.intellect += value; break;
            case ITEM_MOD_SPIRIT:    stats.spi += value; break;
            case ITEM_MOD_STAMINA:   stats.stam += value; break;
            default: break;
        }
    }

    stats.armor += pProto->Armor;
    stats.block_value += int32(pProto->Block);
    stats.holy_res += pProto->HolyRes;
    stats.fire_res += pProto->FireRes;
    stats.nat_res += pProto->NatureRes;
    stats.frost_res += pProto->FrostRes;
    stats.shadow_res += pProto->ShadowRes;
    stats.arcane_res += pProto->ArcaneRes;

    ApplyWeaponShape(stats, pProto);

    for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (pProto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_EQUIP)
            continue;
        if (!pProto->Spells[i].SpellId)
            continue;
        ApplySpell(stats, sSpellMgr.GetSpellEntry(pProto->Spells[i].SpellId));
    }

    if (pItem)
    {
        for (uint32 slot = PROP_ENCHANTMENT_SLOT_0; slot <= PROP_ENCHANTMENT_SLOT_2; ++slot)
        {
            uint32 const enchantId = pItem->GetEnchantmentId(EnchantmentSlot(slot));
            if (!enchantId)
                continue;
            ApplyEnchantment(stats, sSpellItemEnchantmentStore.LookupEntry(enchantId));
        }
    }

    return stats;
}

float ItemEvaluator::UpgradeDelta(Player* pPlayer, ItemPrototype const* pProto,
    Item const* pItem, StatWeights const& weights) const
{
    if (!pPlayer || !pProto)
        return 0.0f;

    uint8 slots[4] = { NULL_SLOT, NULL_SLOT, NULL_SLOT, NULL_SLOT };
    pProto->GetAllowedEquipSlots(slots, pPlayer->GetClass(), pPlayer->CanDualWield());

    ResolvedStats const candidate = ResolveItem(pProto, pItem);
    float const candidateScore = Score(candidate, weights);

    // Two-hander displaces both hands. The baseline is therefore main-hand plus off-hand
    // together, not main-hand alone.
    if (pProto->InventoryType == INVTYPE_2HWEAPON)
    {
        float baseline = 0.0f;
        if (Item* pMain = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            baseline += Score(ResolveItem(pMain->GetProto(), pMain), weights);
        if (Item* pOff = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            baseline += Score(ResolveItem(pOff->GetProto(), pOff), weights);
        return candidateScore - baseline;
    }

    // Rings and trinkets share two slots. The right baseline is the worse of the two
    // occupants, since that is the one the candidate would replace.
    if (pProto->InventoryType == INVTYPE_FINGER || pProto->InventoryType == INVTYPE_TRINKET)
    {
        uint8 const first = (pProto->InventoryType == INVTYPE_FINGER)
            ? EQUIPMENT_SLOT_FINGER1 : EQUIPMENT_SLOT_TRINKET1;
        uint8 const second = first + 1;

        float worst = 0.0f;
        bool any = false;
        for (uint8 slot : { first, second })
        {
            Item* pOcc = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            float score = pOcc ? Score(ResolveItem(pOcc->GetProto(), pOcc), weights) : 0.0f;
            if (!any || score < worst)
            {
                worst = score;
                any = true;
            }
        }
        return candidateScore - worst;
    }

    // Everything else has a primary slot. Prefer the first non-null allowed slot.
    float baseline = 0.0f;
    for (uint8 slot : slots)
    {
        if (slot == NULL_SLOT)
            continue;
        if (Item* pOcc = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            baseline = Score(ResolveItem(pOcc->GetProto(), pOcc), weights);
        break;
    }
    return candidateScore - baseline;
}

namespace
{
    bool IsUniqueEquipped(ItemPrototype const* pProto)
    {
        return pProto && (pProto->Flags & ITEM_FLAG_UNIQUE_EQUIPPED);
    }

    bool ConflictsUnique(Item* candidate, Item* assigned)
    {
        if (!candidate || !assigned)
            return false;
        if (candidate == assigned)
            return false;
        ItemPrototype const* a = candidate->GetProto();
        ItemPrototype const* b = assigned->GetProto();
        if (!a || !b)
            return false;
        if (a->ItemId != b->ItemId)
            return false;
        return IsUniqueEquipped(a) || IsUniqueEquipped(b) || a->MaxCount == 1;
    }

    bool CanWear(Player* pPlayer, Item* pItem, uint8 slot)
    {
        if (!pItem)
            return false;

        uint16 dest = 0;
        InventoryResult const result = pPlayer->CanEquipItem(slot, dest, pItem, true);
        if (result == EQUIP_ERR_OK)
            return true;

        // Holding a two-hander right now is not a reason to rule an off-hand item out. Both
        // hands are decided together, so an off-hand is only ever chosen beside a main hand
        // that leaves room for it, and taking this answer at face value would mean a member
        // wielding a two-hander could never trade it for a weapon and shield.
        return slot == EQUIPMENT_SLOT_OFFHAND && result == EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED;
    }

    bool IsTwoHand(Item* pItem)
    {
        return pItem && pItem->GetProto()
            && pItem->GetProto()->InventoryType == INVTYPE_2HWEAPON;
    }

    typedef std::array<Item*, EQUIPMENT_SLOT_END> Assignment;

    // Every slot decided one at a time. The hands are absent because they are decided
    // together: a two-hander and an off-hand exclude each other, so no single-slot move can
    // trade one arrangement for the other.
    uint8 const SINGLE_SLOTS[] = {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_NECK, EQUIPMENT_SLOT_SHOULDERS,
        EQUIPMENT_SLOT_BODY, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_WAIST,
        EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_WRISTS,
        EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_BACK, EQUIPMENT_SLOT_TABARD,
        EQUIPMENT_SLOT_RANGED, EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2,
        EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2
    };

    // How many items are considered per slot. A bot with a full set of bags can carry more
    // spare gear than is worth searching over, and the ones past this point are the ones
    // that scored worst on their own. Set pieces are exempt, because a piece that is weak
    // alone is exactly what a set bonus is there to redeem.
    size_t const MAX_CANDIDATES_PER_SLOT = 8;

    // Improvement rounds. Each round improves at least one slot or stops, and gear does not
    // interact deeply enough for the climb to need many: the bound exists so a pathological
    // bag cannot spin, not because convergence is expected to be slow.
    uint32 const MAX_OPTIMIZE_ROUNDS = 8;
}

uint32 ItemEvaluator::OptimizeEquipment(Player* pPlayer, StatWeights const& weights,
                                        bool requireShield) const
{
    if (!pPlayer)
        return 0;

    // Everything worn or carried, the contents of equipped bags included. A member handed a
    // real bag keeps earned gear in it, and gear this pass cannot see is gear it will never
    // wear.
    std::vector<Item*> available;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            available.push_back(pItem);
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            available.push_back(pItem);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Item* pContainer = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (!pContainer || !pContainer->IsBag())
            continue;

        Bag* pBag = (Bag*)pContainer;
        for (uint32 i = 0; i < pBag->GetBagSize(); ++i)
            if (Item* pItem = pPlayer->GetItemByPos(bag, uint8(i)))
                available.push_back(pItem);
    }

    // Resolved once each. The search asks for the same item's contribution many times over
    // and resolution is the expensive half of the work: item stats, every equip-trigger
    // spell, and the random-property enchantments on the instance.
    std::unordered_map<Item*, ResolvedStats> resolved;
    for (Item* pItem : available)
        resolved.emplace(pItem, ResolveItem(pItem->GetProto(), pItem));

    auto soloScore = [&](Item* pItem) -> float
    {
        if (!pItem)
            return 0.0f;
        return Score(resolved[pItem], weights);
    };

    // Set bonuses are looked up by (set, count) and there are only a handful of distinct
    // pairs across the whole search, so they are worth remembering rather than resolving
    // again for every candidate move.
    std::map<uint64, ResolvedStats> bonusCache;
    auto bonusFor = [&](uint32 setId, uint32 count) -> ResolvedStats const&
    {
        uint64 const key = (uint64(setId) << 32) | count;
        auto itr = bonusCache.find(key);
        if (itr == bonusCache.end())
            itr = bonusCache.emplace(key, SetBonus(setId, count, pPlayer)).first;
        return itr->second;
    };

    // The number this pass maximises. Not the sum of per-slot scores: caps mean the total is
    // worth less than its parts once a stat runs past the point of usefulness, and set
    // bonuses mean it can be worth more.
    auto loadoutScore = [&](Assignment const& a) -> float
    {
        ResolvedStats total;

        // Counted into a stack array rather than a map. This runs thousands of times per pass
        // and a loadout can contain at most nineteen distinct sets, in practice one or two, so
        // a linear scan over a few entries beats allocating a hash table each time.
        uint32 setIds[EQUIPMENT_SLOT_END] = {};
        uint32 setCounts[EQUIPMENT_SLOT_END] = {};
        uint32 setsSeen = 0;

        for (Item* pItem : a)
        {
            if (!pItem)
                continue;

            AddStats(total, resolved[pItem]);

            uint32 const setId = pItem->GetProto()->ItemSet;
            if (!setId)
                continue;

            uint32 at = 0;
            while (at < setsSeen && setIds[at] != setId)
                ++at;
            if (at == setsSeen)
            {
                setIds[at] = setId;
                ++setsSeen;
            }
            ++setCounts[at];
        }

        for (uint32 i = 0; i < setsSeen; ++i)
            AddStats(total, bonusFor(setIds[i], setCounts[i]));

        return ScoreLoadout(total, weights);
    };

    // Whether this item may go in this slot given everything else already assigned. Wearing
    // one item twice and unique-equipped conflicts are the two ways an otherwise sensible
    // assignment can be impossible.
    auto placeable = [&](Assignment const& a, uint8 slot, Item* pItem) -> bool
    {
        if (!pItem)
            return true;
        for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
        {
            if (s == slot)
                continue;
            if (a[s] == pItem || ConflictsUnique(pItem, a[s]))
                return false;
        }
        return true;
    };

    std::vector<Item*> candidates[EQUIPMENT_SLOT_END];
    std::unordered_map<uint32, uint32> availableSets;
    CountSetPieces(available, availableSets);

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        for (Item* pItem : available)
        {
            if (!CanWear(pPlayer, pItem, slot))
                continue;

            // A two-hander in the main hand is what empties the off-hand, so refusing it here is
            // what leaves room for the shield. Refused as a candidate rather than corrected
            // afterwards, because the hands are scored as a pair and a two-hander that reached the
            // scoring could still win it.
            if (requireShield && slot == EQUIPMENT_SLOT_MAINHAND && IsTwoHand(pItem))
                continue;

            // And nothing but a shield in the off-hand, so the slot cannot be spent on a holdable
            // or an off-hand weapon that would keep the shield out.
            if (requireShield && slot == EQUIPMENT_SLOT_OFFHAND &&
                (!pItem->GetProto() || pItem->GetProto()->InventoryType != INVTYPE_SHIELD))
                continue;

            candidates[slot].push_back(pItem);
        }

        // Best first, and ties broken on guid rather than left to whatever order the bags
        // happened to be walked in. Two members offered the same gear have to reach the same
        // answer, and the same member has to reach it again after a restart.
        std::sort(candidates[slot].begin(), candidates[slot].end(),
            [&](Item* a, Item* b)
            {
                float const sa = soloScore(a);
                float const sb = soloScore(b);
                if (sa != sb)
                    return sa > sb;
                return a->GetGUIDLow() < b->GetGUIDLow();
            });

        if (candidates[slot].size() > MAX_CANDIDATES_PER_SLOT)
        {
            std::vector<Item*> kept(candidates[slot].begin(),
                candidates[slot].begin() + MAX_CANDIDATES_PER_SLOT);
            for (size_t i = MAX_CANDIDATES_PER_SLOT; i < candidates[slot].size(); ++i)
            {
                uint32 const setId = candidates[slot][i]->GetProto()->ItemSet;
                if (setId && availableSets[setId] > 1)
                    kept.push_back(candidates[slot][i]);
            }
            candidates[slot].swap(kept);
        }
    }

    // Both hands at once, including the empty option on each side, so that trading a
    // two-hander for a one-hand and shield is a single move.
    std::vector<Item*> mainOptions(1, nullptr);
    mainOptions.insert(mainOptions.end(),
        candidates[EQUIPMENT_SLOT_MAINHAND].begin(), candidates[EQUIPMENT_SLOT_MAINHAND].end());
    std::vector<Item*> offOptions(1, nullptr);
    offOptions.insert(offOptions.end(),
        candidates[EQUIPMENT_SLOT_OFFHAND].begin(), candidates[EQUIPMENT_SLOT_OFFHAND].end());

    auto climb = [&](Assignment& a)
    {
        for (uint32 round = 0; round < MAX_OPTIMIZE_ROUNDS; ++round)
        {
            bool improved = false;

            for (uint8 slot : SINGLE_SLOTS)
            {
                Item* const worn = a[slot];
                Item* best = worn;
                float bestScore = loadoutScore(a);

                // Empty is a candidate as well as every item. A piece whose only stats are
                // ones the spec scores negatively is worth less than a bare slot, and the
                // strict comparison means an equal choice never displaces what is worn.
                a[slot] = nullptr;
                if (worn)
                {
                    float const bare = loadoutScore(a);
                    if (bare > bestScore)
                    {
                        bestScore = bare;
                        best = nullptr;
                    }
                }

                for (Item* pItem : candidates[slot])
                {
                    if (pItem == worn || !placeable(a, slot, pItem))
                        continue;

                    a[slot] = pItem;
                    float const score = loadoutScore(a);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = pItem;
                    }
                    a[slot] = nullptr;
                }

                a[slot] = best;
                if (best != worn)
                    improved = true;
            }

            {
                Item* const wornMain = a[EQUIPMENT_SLOT_MAINHAND];
                Item* const wornOff = a[EQUIPMENT_SLOT_OFFHAND];
                Item* bestMain = wornMain;
                Item* bestOff = wornOff;
                float bestScore = loadoutScore(a);

                a[EQUIPMENT_SLOT_MAINHAND] = nullptr;
                a[EQUIPMENT_SLOT_OFFHAND] = nullptr;

                for (Item* main : mainOptions)
                {
                    if (!placeable(a, EQUIPMENT_SLOT_MAINHAND, main))
                        continue;

                    for (Item* off : offOptions)
                    {
                        if (IsTwoHand(main) && off)
                            continue;
                        if (main && off && (main == off || ConflictsUnique(main, off)))
                            continue;
                        if (!placeable(a, EQUIPMENT_SLOT_OFFHAND, off))
                            continue;
                        if (main == wornMain && off == wornOff)
                            continue;

                        a[EQUIPMENT_SLOT_MAINHAND] = main;
                        a[EQUIPMENT_SLOT_OFFHAND] = off;
                        float const score = loadoutScore(a);
                        if (score > bestScore)
                        {
                            bestScore = score;
                            bestMain = main;
                            bestOff = off;
                        }
                        a[EQUIPMENT_SLOT_MAINHAND] = nullptr;
                        a[EQUIPMENT_SLOT_OFFHAND] = nullptr;
                    }
                }

                a[EQUIPMENT_SLOT_MAINHAND] = bestMain;
                a[EQUIPMENT_SLOT_OFFHAND] = bestOff;
                if (bestMain != wornMain || bestOff != wornOff)
                    improved = true;
            }

            if (!improved)
                break;
        }
    };

    // Where the climb starts from decides what it can find, because improving one slot at a
    // time cannot cross a set threshold that is more than one piece away. Three kinds of
    // start, and the best finish wins.
    std::vector<Assignment> seeds;

    // What the member is already wearing. This is the seed that matters most: because a tie
    // never displaces the incumbent, starting here is what guarantees the pass can only ever
    // raise the loadout score. An eight-piece set is kept not by a rule about sets but
    // because every single-piece swap out of it scores lower than staying.
    Assignment current{};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        current[slot] = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    seeds.push_back(current);

    // Nothing worn, which builds a loadout from the best each slot has to offer and is the
    // seed that dresses a member arriving with gear in its bags and empty slots.
    seeds.push_back(Assignment{});

    // One per set the member holds more than one piece of, with those pieces forced in.
    // Assembling a set from scratch is the case single-piece moves cannot reach: each piece
    // on its own may be a downgrade right up until the bonus lands.
    for (auto const& kv : availableSets)
    {
        if (kv.second < 2)
            continue;

        Assignment seed{};
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            for (Item* pItem : candidates[slot])
            {
                if (pItem->GetProto()->ItemSet != kv.first)
                    continue;
                if (!placeable(seed, slot, pItem))
                    continue;
                seed[slot] = pItem;
                break;
            }
        }
        if (IsTwoHand(seed[EQUIPMENT_SLOT_MAINHAND]))
            seed[EQUIPMENT_SLOT_OFFHAND] = nullptr;

        seeds.push_back(seed);
    }

    Assignment assigned = current;
    float bestScore = loadoutScore(current);
    for (Assignment seed : seeds)
    {
        climb(seed);
        float const score = loadoutScore(seed);
        if (score > bestScore)
        {
            bestScore = score;
            assigned = seed;
        }
    }

    // A two-hander leaves the off-hand slot empty rather than merely unused, and the apply
    // loop below would otherwise try to fill it.
    if (IsTwoHand(assigned[EQUIPMENT_SLOT_MAINHAND]))
        assigned[EQUIPMENT_SLOT_OFFHAND] = nullptr;

    // Apply. For every slot whose desired item differs from what is worn, put the current
    // occupant into bags or mail and equip the desired one. Declining when AutoUnequip
    // cannot make room is the property that keeps Thunderfury from disappearing: a swap
    // that would destroy something simply does not happen.
    uint32 changed = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* want = assigned[slot];
        Item* have = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (want == have)
            continue;

        if (have)
        {
            pPlayer->AutoUnequipItemFromSlot(slot);
            if (pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;
        }

        if (!want)
            continue;

        // It may still be equipped in another slot from this same pass.
        if (want->IsEquipped())
        {
            uint8 const from = want->GetSlot();
            pPlayer->AutoUnequipItemFromSlot(from);
            if (want->IsEquipped())
                continue;
        }

        uint16 dest = 0;
        if (pPlayer->CanEquipItem(slot, dest, want, true) != EQUIP_ERR_OK)
            continue;

        uint8 const bag = want->GetBagSlot();
        uint8 const bagSlot = want->GetSlot();
        pPlayer->RemoveItem(bag, bagSlot, false);
        pPlayer->EquipItem(dest & 255, want, true);
        ++changed;
    }

    pPlayer->AutoUnequipOffhandIfNeed();
    return changed;
}
