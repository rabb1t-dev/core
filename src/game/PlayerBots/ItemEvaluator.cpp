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
#include <cmath>
#include <functional>
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
        " `fire_res`, `nat_res`, `frost_res`"
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

        m_weights[WeightKey(w.classId, w.spec)] = w;
        ++count;
    }
    while (result->NextRow());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u raidguild_stat_weight rows", count);
}

StatWeights const* ItemEvaluator::GetWeights(uint8 classId, std::string const& spec) const
{
    auto itr = m_weights.find(WeightKey(classId, spec));
    if (itr != m_weights.end())
        return &itr->second;

    // A roster member whose authored spec has no row yet still needs a score so that
    // OptimizeEquipment has something to do. Any weight for the same class is closer to
    // right than equating every item to zero.
    for (auto const& kv : m_weights)
        if (kv.second.classId == classId)
            return &kv.second;

    return nullptr;
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
        return pPlayer->CanEquipItem(slot, dest, pItem, true) == EQUIP_ERR_OK;
    }
}

uint32 ItemEvaluator::OptimizeEquipment(Player* pPlayer, StatWeights const& weights) const
{
    if (!pPlayer)
        return 0;

    // Everything worn or sitting in the backpack. Bags of bags are ignored for now: bots
    // keep earned gear in the backpack, and walking nested bags is a later refinement.
    std::vector<Item*> available;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            available.push_back(pItem);
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* pItem = pPlayer->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            available.push_back(pItem);

    auto itemScore = [&](Item* pItem) -> float
    {
        if (!pItem)
            return 0.0f;
        return Score(ResolveItem(pItem->GetProto(), pItem), weights);
    };

    Item* assigned[EQUIPMENT_SLOT_END] = {};

    auto bestForSlot = [&](uint8 slot, std::function<bool(Item*)> const& accept) -> Item*
    {
        Item* best = nullptr;
        float bestScore = -1e30f;
        for (Item* pItem : available)
        {
            if (!accept(pItem))
                continue;
            if (!CanWear(pPlayer, pItem, slot))
                continue;

            bool taken = false;
            for (uint8 s = EQUIPMENT_SLOT_START; s < EQUIPMENT_SLOT_END; ++s)
            {
                if (assigned[s] == pItem || ConflictsUnique(pItem, assigned[s]))
                {
                    taken = true;
                    break;
                }
            }
            if (taken)
                continue;

            float const score = itemScore(pItem);
            if (!best || score > bestScore)
            {
                best = pItem;
                bestScore = score;
            }
        }
        return best;
    };

    // Non-weapon, non-paired armour slots first. Order does not matter because each item
    // can only fill one of them.
    static uint8 const singleSlots[] = {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_NECK, EQUIPMENT_SLOT_SHOULDERS,
        EQUIPMENT_SLOT_BODY, EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_WAIST,
        EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_WRISTS,
        EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_BACK, EQUIPMENT_SLOT_TABARD,
        EQUIPMENT_SLOT_RANGED
    };
    for (uint8 slot : singleSlots)
    {
        assigned[slot] = bestForSlot(slot, [](Item*) { return true; });
    }

    // Rings, then trinkets: two best that do not conflict.
    for (uint8 slot : { uint8(EQUIPMENT_SLOT_FINGER1), uint8(EQUIPMENT_SLOT_FINGER2) })
        assigned[slot] = bestForSlot(slot, [](Item* p) {
            return p->GetProto()->InventoryType == INVTYPE_FINGER;
        });
    for (uint8 slot : { uint8(EQUIPMENT_SLOT_TRINKET1), uint8(EQUIPMENT_SLOT_TRINKET2) })
        assigned[slot] = bestForSlot(slot, [](Item* p) {
            return p->GetProto()->InventoryType == INVTYPE_TRINKET;
        });

    // Weapons. Compare a two-hander against the best one-hand plus off-hand combination and
    // take whichever scores higher.
    Item* bestTwoHand = bestForSlot(EQUIPMENT_SLOT_MAINHAND, [](Item* p) {
        return p->GetProto()->InventoryType == INVTYPE_2HWEAPON;
    });
    Item* bestMain = bestForSlot(EQUIPMENT_SLOT_MAINHAND, [](Item* p) {
        InventoryType t = InventoryType(p->GetProto()->InventoryType);
        return t == INVTYPE_WEAPON || t == INVTYPE_WEAPONMAINHAND;
    });
    // Temporarily claim the mainhand so the offhand picker cannot re-use it.
    Item* savedMain = assigned[EQUIPMENT_SLOT_MAINHAND];
    assigned[EQUIPMENT_SLOT_MAINHAND] = bestMain;
    Item* bestOff = bestForSlot(EQUIPMENT_SLOT_OFFHAND, [](Item* p) {
        InventoryType t = InventoryType(p->GetProto()->InventoryType);
        return t == INVTYPE_WEAPON || t == INVTYPE_WEAPONOFFHAND || t == INVTYPE_HOLDABLE
            || t == INVTYPE_SHIELD;
    });
    assigned[EQUIPMENT_SLOT_MAINHAND] = savedMain;

    float twoHandScore = bestTwoHand ? itemScore(bestTwoHand) : -1e30f;
    float oneHandScore = (bestMain ? itemScore(bestMain) : 0.0f)
                       + (bestOff ? itemScore(bestOff) : 0.0f);

    if (bestTwoHand && twoHandScore >= oneHandScore)
    {
        assigned[EQUIPMENT_SLOT_MAINHAND] = bestTwoHand;
        assigned[EQUIPMENT_SLOT_OFFHAND] = nullptr;
    }
    else
    {
        assigned[EQUIPMENT_SLOT_MAINHAND] = bestMain;
        assigned[EQUIPMENT_SLOT_OFFHAND] = bestOff;
    }

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
