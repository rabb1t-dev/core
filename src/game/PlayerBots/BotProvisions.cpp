#include "BotProvisions.h"
#include "ItemPrototype.h"
#include "Player.h"

// Enchant ids are SpellItemEnchantment entries, taken from EffectMiscValue of the enchanting spell
// that applies them. They are recorded here rather than looked up by spell so that applying one
// costs nothing and needs no enchanter: the id goes straight into the item.
enum BotWeaponEnchants
{
    ENCHANT_WEAPON_MINOR_STRIKING = 250,        // +1 damage
    ENCHANT_WEAPON_LESSER_STRIKING = 241,       // +2 damage
    ENCHANT_WEAPON_STRIKING = 943,              // +3 damage
    ENCHANT_WEAPON_FIERY = 803,                 // chance of additional fire damage on hit
    ENCHANT_2H_LESSER_INTELLECT = 723,          // +3 intellect
};

enum BotArmorEnchants
{
    ENCHANT_CHEST_MINOR_STATS = 847,            // +1 all stats
    ENCHANT_CHEST_LESSER_STATS = 866,           // +2 all stats
    ENCHANT_CHEST_LESSER_MANA = 246,            // +20 mana
    ENCHANT_BRACER_LESSER_STRENGTH = 823,       // +3 strength
    ENCHANT_BRACER_LESSER_STAMINA = 724,        // +3 stamina
    ENCHANT_BRACER_LESSER_INTELLECT = 723,      // +3 intellect
    ENCHANT_BOOTS_LESSER_AGILITY = 849,         // +3 agility
    ENCHANT_BOOTS_LESSER_STAMINA = 724,         // +3 stamina
    ENCHANT_BOOTS_LESSER_SPIRIT = 255,          // +3 spirit
    ENCHANT_CLOAK_LESSER_PROTECTION = 744,      // +20 armor
    ENCHANT_CLOAK_MINOR_PROTECTION = 783,       // +10 armor
    ENCHANT_SHIELD_LESSER_PROTECTION = 848,     // +30 armor
};

enum BotConsumableItems
{
    ITEM_ELIXIR_OGRES_STRENGTH = 3391,          // level 20, +8 strength, one hour
    ITEM_ELIXIR_LESSER_AGILITY = 3390,          // level 18, +8 agility, one hour
    ITEM_ELIXIR_DEFENSE = 3389,                 // level 16, +50 armor, one hour
    ITEM_ELIXIR_FIREPOWER = 6373,               // level 18, +10 fire damage, one hour
    ITEM_ELIXIR_MINOR_FORTITUDE = 2458,         // level 2, +27 health

    ITEM_SCROLL_STRENGTH = 954,                 // level 10
    ITEM_SCROLL_AGILITY = 3012,                 // level 10
    ITEM_SCROLL_STAMINA_II = 1711,              // level 20
    ITEM_SCROLL_INTELLECT_II = 2290,            // level 20
    ITEM_SCROLL_SPIRIT_II = 1712,               // level 15
    ITEM_SCROLL_PROTECTION_II = 1478,           // level 15

    ITEM_ROUGH_SHARPENING_STONE = 2862,         // level 1, +2 damage
    ITEM_COARSE_SHARPENING_STONE = 2863,        // level 5, +4 damage
    ITEM_HEAVY_SHARPENING_STONE = 2871,         // level 15, +6 damage
    ITEM_ROUGH_WEIGHTSTONE = 3239,              // level 1, +2 damage
    ITEM_COARSE_WEIGHTSTONE = 3240,             // level 5, +4 damage
    ITEM_HEAVY_WEIGHTSTONE = 3241,              // level 15, +6 damage

    ITEM_MINOR_WIZARD_OIL = 20744,              // level 5, +8 spell damage
    ITEM_MINOR_MANA_OIL = 20745,                // level 20, mana regeneration
};

// Weapon enchants, best first. Fiery Weapon is the one a min-maxing guild puts on everything it
// swings, and it is what this was asked for. Worth being clear that a genuine level twenty weapon
// could not take it: the enchanting recipe requires an item of level thirty or better, and the
// client enforces that when a real enchanter applies it. Writing the id into the item bypasses
// that check, so this is deliberately a step past what a player at this level could buy.
static BotEnchantChoice const s_meleeWeaponEnchants[] =
{
    { 20, ENCHANT_WEAPON_FIERY,          "Fiery Weapon" },
    { 15, ENCHANT_WEAPON_LESSER_STRIKING, "Lesser Striking" },
    { 5,  ENCHANT_WEAPON_MINOR_STRIKING,  "Minor Striking" },
};

uint32 GetBotWeaponEnchant(uint32 level, bool casterWeapon)
{
    // A caster's weapon is a stat stick, and vanilla has almost nothing for it below the forties.
    // What a caster actually wants on it is an oil, which is a consumable and handled there.
    if (casterWeapon)
        return level >= 15 ? ENCHANT_2H_LESSER_INTELLECT : 0;

    for (auto const& choice : s_meleeWeaponEnchants)
        if (level >= choice.minLevel)
            return choice.enchantId;

    return 0;
}

uint32 GetBotArmorEnchant(uint8 equipmentSlot, uint8 classId, CombatBotRoles role, uint32 level)
{
    // Everything below is a Lesser enchant, and those become available in the middle teens. Under
    // that only the Minor tier exists, and a single point of a stat is not worth the bookkeeping.
    if (level < 15)
        return equipmentSlot == EQUIPMENT_SLOT_BACK ? ENCHANT_CLOAK_MINOR_PROTECTION : 0;

    bool const isCaster = role == ROLE_HEALER ||
                         (role == ROLE_RANGE_DPS && !CombatBotBaseAI::IsMeleeWeaponClass(classId));
    bool const wantsAgility = classId == CLASS_ROGUE || classId == CLASS_HUNTER;

    switch (equipmentSlot)
    {
        case EQUIPMENT_SLOT_CHEST:
            // Stats across the board beat any single one of them, except on a healer, where the
            // extra casts out of twenty more mana are worth more than two of each stat.
            if (isCaster && level < 20)
                return ENCHANT_CHEST_LESSER_MANA;
            return level >= 20 ? ENCHANT_CHEST_LESSER_STATS : ENCHANT_CHEST_MINOR_STATS;

        case EQUIPMENT_SLOT_WRISTS:
            if (isCaster)
                return ENCHANT_BRACER_LESSER_INTELLECT;
            if (role == ROLE_TANK)
                return ENCHANT_BRACER_LESSER_STAMINA;
            // No Lesser Agility exists for bracers, only Minor, so an agility class does better
            // out of the stamina than out of one point of what it actually wants.
            return wantsAgility ? ENCHANT_BRACER_LESSER_STAMINA : ENCHANT_BRACER_LESSER_STRENGTH;

        case EQUIPMENT_SLOT_FEET:
            if (role == ROLE_TANK)
                return ENCHANT_BOOTS_LESSER_STAMINA;
            if (role == ROLE_HEALER)
                return ENCHANT_BOOTS_LESSER_SPIRIT;
            return ENCHANT_BOOTS_LESSER_AGILITY;

        case EQUIPMENT_SLOT_BACK:
            // Armour on the cloak for everyone. Agility is the alternative and is worth less than
            // twenty armour to anything that is not already stacking it.
            return ENCHANT_CLOAK_LESSER_PROTECTION;

        case EQUIPMENT_SLOT_OFFHAND:
            // Only meaningful on a shield, and the caller is responsible for checking that.
            return ENCHANT_SHIELD_LESSER_PROTECTION;
    }

    return 0;
}

uint32 GetBotWeaponStone(uint32 weaponSubclass, uint32 level)
{
    bool bladed;

    switch (weaponSubclass)
    {
        case ITEM_SUBCLASS_WEAPON_AXE:
        case ITEM_SUBCLASS_WEAPON_AXE2:
        case ITEM_SUBCLASS_WEAPON_SWORD:
        case ITEM_SUBCLASS_WEAPON_SWORD2:
        case ITEM_SUBCLASS_WEAPON_DAGGER:
        case ITEM_SUBCLASS_WEAPON_POLEARM:
            bladed = true;
            break;
        case ITEM_SUBCLASS_WEAPON_MACE:
        case ITEM_SUBCLASS_WEAPON_MACE2:
        case ITEM_SUBCLASS_WEAPON_STAFF:
        case ITEM_SUBCLASS_WEAPON_FIST:
            bladed = false;
            break;
        default:
            // Bows, guns, wands and thrown take neither.
            return 0;
    }

    if (level >= 15)
        return bladed ? ITEM_HEAVY_SHARPENING_STONE : ITEM_HEAVY_WEIGHTSTONE;
    if (level >= 5)
        return bladed ? ITEM_COARSE_SHARPENING_STONE : ITEM_COARSE_WEIGHTSTONE;

    return bladed ? ITEM_ROUGH_SHARPENING_STONE : ITEM_ROUGH_WEIGHTSTONE;
}

// The lists below deliberately avoid stacking two things that grant the same stat. A scroll and an
// elixir of strength do not add up in this expansion, the stronger simply wins, so handing a bot
// both wastes one of them and half the time it drinks them in the order that wastes the better.
static std::vector<BotConsumableChoice> const s_tankConsumables =
{
    { ITEM_ELIXIR_OGRES_STRENGTH, 20, "Elixir of Ogre's Strength", false },
    { ITEM_ELIXIR_DEFENSE,        16, "Elixir of Defense",         false },
    { ITEM_SCROLL_STAMINA_II,     20, "Scroll of Stamina II",      false },
    { ITEM_SCROLL_STRENGTH,       10, "Scroll of Strength",        false },
    { ITEM_ELIXIR_MINOR_FORTITUDE, 2, "Elixir of Minor Fortitude", false },
};

static std::vector<BotConsumableChoice> const s_strengthMeleeConsumables =
{
    { ITEM_ELIXIR_OGRES_STRENGTH, 20, "Elixir of Ogre's Strength", false },
    { ITEM_SCROLL_STAMINA_II,     20, "Scroll of Stamina II",      false },
    { ITEM_SCROLL_PROTECTION_II,  15, "Scroll of Protection II",   false },
    { ITEM_SCROLL_STRENGTH,       10, "Scroll of Strength",        false },
};

static std::vector<BotConsumableChoice> const s_agilityMeleeConsumables =
{
    { ITEM_ELIXIR_LESSER_AGILITY, 18, "Elixir of Lesser Agility", false },
    { ITEM_SCROLL_STAMINA_II,     20, "Scroll of Stamina II",     false },
    { ITEM_SCROLL_PROTECTION_II,  15, "Scroll of Protection II",  false },
    { ITEM_SCROLL_AGILITY,        10, "Scroll of Agility",        false },
};

static std::vector<BotConsumableChoice> const s_casterDpsConsumables =
{
    { ITEM_ELIXIR_FIREPOWER,      18, "Elixir of Firepower",     false },
    { ITEM_SCROLL_INTELLECT_II,   20, "Scroll of Intellect II",  false },
    { ITEM_SCROLL_PROTECTION_II,  15, "Scroll of Protection II", false },
    { ITEM_MINOR_WIZARD_OIL,       5, "Minor Wizard Oil",        true  },
};

static std::vector<BotConsumableChoice> const s_healerConsumables =
{
    { ITEM_SCROLL_INTELLECT_II,   20, "Scroll of Intellect II",  false },
    { ITEM_SCROLL_SPIRIT_II,      15, "Scroll of Spirit II",     false },
    { ITEM_SCROLL_PROTECTION_II,  15, "Scroll of Protection II", false },
    { ITEM_MINOR_MANA_OIL,        20, "Minor Mana Oil",          true  },
};

static std::vector<BotConsumableChoice> const s_noConsumables = {};

std::vector<BotConsumableChoice> const& GetBotConsumables(uint8 classId, CombatBotRoles role, uint32 level)
{
    // Nothing worth carrying before the first elixirs and scrolls exist.
    if (level < 10)
        return s_noConsumables;

    switch (role)
    {
        case ROLE_TANK:
            return s_tankConsumables;

        case ROLE_HEALER:
            return s_healerConsumables;

        case ROLE_MELEE_DPS:
            return (classId == CLASS_ROGUE) ? s_agilityMeleeConsumables
                                            : s_strengthMeleeConsumables;

        case ROLE_RANGE_DPS:
            // A hunter is a physical damage dealer that happens to stand at range, and wants what
            // the rogue wants rather than what the mage wants.
            if (classId == CLASS_HUNTER)
                return s_agilityMeleeConsumables;
            return s_casterDpsConsumables;

        default:
            break;
    }

    return s_noConsumables;
}
