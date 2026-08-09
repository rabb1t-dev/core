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

#ifndef MANGOS_ITEMEVALUATOR_H
#define MANGOS_ITEMEVALUATOR_H

#include "Common.h"
#include "Policies/Singleton.h"

#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;
struct ItemPrototype;
struct SpellEntry;
struct SpellItemEnchantmentEntry;

// Flat resolved contribution of an item. Vanilla puts most of what decides BiS into
// equip-trigger spells rather than ItemStat, so the interesting work is collapsing those
// layers into one vector before anyone asks whether A is better than B.
struct ResolvedStats
{
    int32 armor = 0;
    int32 stam = 0;
    int32 spi = 0;
    int32 intellect = 0;
    int32 str = 0;
    int32 agi = 0;

    int32 ap = 0;
    int32 hit = 0;
    int32 crit = 0;
    // Skill with the weapon actually being used, melee or ranged: they share a column
    // because no character benefits from two at once, and gear that raises several is worth
    // the largest, not their sum. Ranged attack power is a different and much cheaper stat,
    // so it stays in its own field.
    int32 weapon_skill = 0;
    int32 defense = 0;
    int32 dodge = 0;
    int32 parry = 0;
    int32 block = 0;
    // Includes the base block printed on a shield, not just what equip effects add.
    int32 block_value = 0;
    int32 ranged_ap = 0;

    int32 spdmg = 0;
    int32 sppen = 0;
    int32 sphit = 0;
    int32 spcrit = 0;
    int32 spheal = 0;
    int32 mp5 = 0;

    int32 fire_res = 0;
    int32 nat_res = 0;
    int32 frost_res = 0;
    int32 shadow_res = 0;
    int32 arcane_res = 0;
    int32 holy_res = 0;

    // Weapon shape. avg_hit is average damage per swing, dps is damage per second, speed
    // is swing time in seconds. They are meaningful only for weapons and are left at zero
    // for everything else so a weight that ignores them cannot accidentally credit them.
    float avg_hit = 0.0f;
    float dps = 0.0f;
    float speed = 0.0f;
};

// Per-class per-spec priorities. Tunable without a recompile: the values live in
// raidguild_stat_weight and are reloaded with the rest of the world tables.
struct StatWeights
{
    uint8 classId = 0;
    std::string spec;

    float avg_hit = 0.0f;
    float dps = 0.0f;
    float speed = 0.0f;
    float armor = 0.0f;
    float stam = 0.0f;
    float spi = 0.0f;
    float intellect = 0.0f;
    float str = 0.0f;
    float agi = 0.0f;
    float ap = 0.0f;
    float hit = 0.0f;
    float crit = 0.0f;
    float weapon_skill = 0.0f;
    float defense = 0.0f;
    float dodge = 0.0f;
    float parry = 0.0f;
    float block = 0.0f;
    float block_value = 0.0f;
    float ranged_ap = 0.0f;
    float spdmg = 0.0f;
    float sppen = 0.0f;
    float sphit = 0.0f;
    float spcrit = 0.0f;
    float spheal = 0.0f;
    float mp5 = 0.0f;
    float fire_res = 0.0f;
    float nat_res = 0.0f;
    float frost_res = 0.0f;

    // Hard caps, in the same gear-derived units the resolver accumulates: the total past
    // which another point is worth nothing. Zero means uncapped.
    //
    // Hit is capped because a miss chance cannot go below zero, and weapon skill because
    // its miss and dodge reduction stops at the target's defense. Both caps depend on
    // talents and racials that gear knows nothing about — a rogue with Precision needs 5%
    // less hit from gear than one without — so the authored number is the gear total for a
    // member already carrying that spec's talents, which is what makes it a per-spec row
    // rather than a constant.
    //
    // Defense deliberately has no cap. Past crit immunity it stops being decisive but keeps
    // giving dodge and miss, so it diminishes rather than ending, and a hard cap would
    // model it worse than the linear weight does.
    float hit_cap = 0.0f;
    float sphit_cap = 0.0f;
    float weapon_skill_cap = 0.0f;
};

class ItemEvaluator
{
    public:
        void Load();

        // Sum of ItemStat, Armor, Block, resistances and weapon shape, plus every
        // ITEM_SPELLTRIGGER_ON_EQUIP aura resolved through spell_template. When an Item
        // instance is supplied its random-property enchantment slots are folded in too;
        // evaluating a prototype alone (a drop before it is rolled) skips them.
        ResolvedStats ResolveItem(ItemPrototype const* pProto, Item const* pItem = nullptr) const;

        // The same resolution for a bare spell id, which the differential test uses to
        // cross-check the sheet's 498-row hand table against spell_template.
        ResolvedStats ResolveSpell(uint32 spellId) const;

        StatWeights const* GetWeights(uint8 classId, std::string const& spec) const;

        // Exact, with no fallback, so a command that has just been handed a spec name can say
        // whether it will be scored on its own weights or on somebody else's.
        bool HasWeights(uint8 classId, std::string const& spec) const;

        // One item against one weight row, linear and uncapped. Correct for comparing two
        // candidates for the same slot in isolation, and the form the differential test
        // checks, but it cannot see anything that belongs to a combination.
        float Score(ResolvedStats const& stats, StatWeights const& weights) const;

        // A whole worn set: the sum of its pieces plus every set bonus the combination
        // triggers. Set bonuses are why this cannot be derived from per-item scores — an
        // individually worse piece can be the right answer when it completes a tier set.
        ResolvedStats ResolveLoadout(std::vector<Item*> const& worn, Player const* pPlayer = nullptr) const;

        // The same for a hypothetical loadout that nobody is wearing, which is what a test
        // asking whether the arithmetic is right needs. Prototypes carry no random-property
        // enchantments, so unlike the overload above this sees only what the item is before
        // it is rolled.
        ResolvedStats ResolveLoadout(std::vector<ItemPrototype const*> const& worn,
            Player const* pPlayer = nullptr) const;

        // Score a total with the weight row's caps applied. Only meaningful on a total: a
        // cap is a property of everything worn at once, so asking whether one item is past
        // the hit cap has no answer.
        float ScoreLoadout(ResolvedStats const& total, StatWeights const& weights) const;

        // Candidate versus what currently occupies its target slot(s). Returns how much
        // better the candidate is (positive means wear it). Handles one-hand versus
        // two-hand swaps, where both hands have to leave together, and the paired ring
        // and trinket slots, where the worse of the two occupants is the right baseline.
        float UpgradeDelta(Player* pPlayer, ItemPrototype const* pProto,
            Item const* pItem, StatWeights const& weights) const;

        // Assigns the best combination available into the equipment slots, from everything
        // the character is wearing or carrying, bags included. Judged on the loadout score,
        // so set bonuses and caps decide swaps that per-slot scoring would get wrong.
        // Never settles for a loadout worse than the one already worn. Displaced gear goes
        // to bags or mail through AutoUnequipItemFromSlot; nothing is destroyed.
        uint32 OptimizeEquipment(Player* pPlayer, StatWeights const& weights) const;

    private:
        void ApplySpell(ResolvedStats& stats, SpellEntry const* pSpell) const;
        void ApplyEnchantment(ResolvedStats& stats, SpellItemEnchantmentEntry const* pEnchant) const;
        void ApplyWeaponShape(ResolvedStats& stats, ItemPrototype const* pProto) const;

        // What holding `count` pieces of `setId` is worth. Every set spell whose threshold
        // the count reaches, resolved the same way an equip-trigger spell is. The player is
        // consulted only for the profession-set skill requirement, and may be null.
        ResolvedStats SetBonus(uint32 setId, uint32 count, Player const* pPlayer) const;

        // classId << 8 | hash of Spec is overkill; keyed as "classId:spec".
        std::unordered_map<std::string, StatWeights> m_weights;
};

#define sItemEvaluator MaNGOS::Singleton<ItemEvaluator>::Instance()

#endif
