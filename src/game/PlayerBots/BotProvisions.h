#ifndef MANGOS_BOT_PROVISIONS_H
#define MANGOS_BOT_PROVISIONS_H

#include "Common.h"
#include "CombatBotBaseAI.h"

// What a bot arrives at a dungeon carrying, beyond the gear on its back.
//
// Gearing a bot to its level turned out to be only half of what a real group brings. A hardcore
// guild clearing Wailing Caverns at the same levels does noticeably more damage than five
// correctly geared characters, and the difference is not gear: it is a permanent enchant on every
// weapon, an hour-long elixir on every character, a sharpening stone on every blade. None of that
// is level inappropriate. All of it is what a player who cares about not dying actually does, and
// none of it existed here.
//
// Two kinds of thing, deliberately kept apart:
//
//   Enchants are permanent, applied once when the bot's gear is settled, and cost nothing at
//   runtime. They go in the item's permanent enchantment slot directly rather than by casting an
//   enchanting spell, so no skill requirement or item level requirement applies.
//
//   Consumables are items the bot is handed and then drinks or applies out of combat. Elixirs and
//   scrolls buff the character; stones and oils are temporary weapon enchants and so compete for
//   the temporary enchantment slot, which is the same slot a rogue's poison uses. A rogue
//   therefore gets no stone: the poison is worth more.

struct BotEnchantChoice
{
    uint32 minLevel;
    uint32 enchantId;
    char const* name;
};

struct BotConsumableChoice
{
    uint32 itemId;
    uint32 minLevel;
    char const* name;

    // Set when the item's effect is a temporary weapon enchant rather than a buff on the drinker,
    // which changes how it has to be used: the weapon is the target, not the bot.
    bool appliesToWeapon;
};

// The permanent enchant for a weapon, given the wearer's level and whether the weapon is being
// swung or cast with. Zero when nothing is worth applying yet.
uint32 GetBotWeaponEnchant(uint32 level, bool casterWeapon);

// The permanent enchant for an armour slot, chosen for the role that has to wear it. Zero when
// that slot has nothing worth applying at this level.
uint32 GetBotArmorEnchant(uint8 equipmentSlot, uint8 classId, CombatBotRoles role, uint32 level);

// Everything a bot of this class, role and level should be carrying, in the order it should be
// used. Empty for a bot too low for any of it.
std::vector<BotConsumableChoice> const& GetBotConsumables(uint8 classId, CombatBotRoles role, uint32 level);

// The weapon stone that suits a weapon of this subclass: sharpening for bladed, weightstone for
// blunt. Zero for a weapon that takes neither, such as a bow.
uint32 GetBotWeaponStone(uint32 weaponSubclass, uint32 level);

#endif
