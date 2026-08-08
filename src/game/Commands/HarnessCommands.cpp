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

#include "Common.h"
#include "Chat.h"
#include "Player.h"
#include "Group.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "AccountMgr.h"
#include "Database/DBCStores.h"
#include "WorldSession.h"
#include "SharedDefines.h"
#include "Util.h"
#include "PlayerBotMgr.h"
#include "PlayerBotAI.h"
#include "CombatBotBaseAI.h"
#include "Totem.h"
#include "MasterPlayer.h"
#include "Mail/Mail.h"
#include "Maps/PathFinder.h"
#include "Maps/MoveMap.h"
#include "MotionMaster.h"

namespace
{
    // Runs a command against a live session while sending the output somewhere else, so a
    // test driver on the console or SOAP can drive a character that has no game client.
    class HarnessChatHandler : public ChatHandler
    {
        public:
            HarnessChatHandler(WorldSession* session, ChatHandler* output, AccountTypes accessLevel)
                : ChatHandler(session), m_output(output), m_accessLevel(accessLevel) {}

            void Run(char const* command) { ExecuteCommand(command); }

            void SendSysMessage(char const* str) override { m_output->SendSysMessage(str); }

            // The caller has already been authorized, so the puppet character does not
            // also need to sit on a game master account.
            AccountTypes GetAccessLevel() const override { return m_accessLevel; }

        private:
            ChatHandler* m_output;
            AccountTypes m_accessLevel;
    };
}

Player* ChatHandler::GetHarnessTarget(char** args)
{
    if (!sWorld.getConfig(CONFIG_BOOL_HARNESS_ENABLE))
    {
        SendSysMessage("Harness: disabled. Set Harness.Enable = 1 to use this command.");
        return nullptr;
    }

    std::string name = ExtractPlayerNameFromLink(args);
    if (name.empty())
    {
        SendSysMessage("Harness: expected a character name.");
        return nullptr;
    }

    Player* pTarget = ObjectAccessor::FindPlayerByName(name.c_str());
    if (!pTarget || !pTarget->IsInWorld() || !pTarget->GetSession())
    {
        PSendSysMessage("Harness: '%s' is not in the world.", name.c_str());
        return nullptr;
    }

    return pTarget;
}

bool ChatHandler::HandleHarnessExecCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    char* command = args;
    while (command && *command == ' ')
        ++command;

    // A leading dot is how a human would type it, so accept and discard it.
    if (command && *command == '.')
        ++command;

    if (!command || !*command)
    {
        SendSysMessage("Harness: expected a command to run.");
        SetSentErrorMessage(true);
        return false;
    }

    HarnessChatHandler handler(pTarget->GetSession(), this, GetAccessLevel());
    handler.Run(command);

    if (handler.HasSentErrorMessage())
    {
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

// .harness login <name>
// Brings a character into the world with no client attached. This passes an explicit AI so
// the entry is flagged as a custom bot, which is what keeps PlayerBotMgr::Update from
// skipping it while random bots are disabled.
bool ChatHandler::HandleHarnessLoginCommand(char* args)
{
    if (!sWorld.getConfig(CONFIG_BOOL_HARNESS_ENABLE))
    {
        SendSysMessage("Harness: disabled. Set Harness.Enable = 1 to use this command.");
        SetSentErrorMessage(true);
        return false;
    }

    std::string name = ExtractPlayerNameFromLink(&args);
    if (name.empty())
    {
        SendSysMessage("Harness: expected a character name.");
        SetSentErrorMessage(true);
        return false;
    }

    if (Player* pOnline = ObjectAccessor::FindPlayerByName(name.c_str()))
    {
        PSendSysMessage("already online name=%s guid=%u", pOnline->GetName(), pOnline->GetGUIDLow());
        return true;
    }

    uint32 const guid = sObjectMgr.GetPlayerGuidByName(name).GetCounter();
    if (!guid)
    {
        PSendSysMessage("Harness: no character named '%s'.", name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    if (!sPlayerBotMgr.AddBot(guid, false, new PlayerBotAI(nullptr)))
    {
        PSendSysMessage("Harness: could not log in '%s'.", name.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("logging in name=%s guid=%u", name.c_str(), guid);
    return true;
}

// .harness createchar <account> <name> <race> <class> [gender]
// Writes a real character row so a driver can provision puppets without a game client.
bool ChatHandler::HandleHarnessCreateCharCommand(char* args)
{
    if (!sWorld.getConfig(CONFIG_BOOL_HARNESS_ENABLE))
    {
        SendSysMessage("Harness: disabled. Set Harness.Enable = 1 to use this command.");
        SetSentErrorMessage(true);
        return false;
    }

    char* accountStr = ExtractArg(&args);
    char* nameStr = ExtractArg(&args);
    uint32 raceId = 0;
    uint32 classId = 0;
    uint32 gender = GENDER_MALE;

    if (!accountStr || !nameStr || !ExtractUInt32(&args, raceId) || !ExtractUInt32(&args, classId))
    {
        SendSysMessage("Syntax: .harness createchar <account> <name> <race> <class> [gender]");
        SetSentErrorMessage(true);
        return false;
    }

    ExtractUInt32(&args, gender);

    uint32 const accountId = sAccountMgr.GetId(accountStr);
    if (!accountId)
    {
        PSendSysMessage("Harness: no such account '%s'.", accountStr);
        SetSentErrorMessage(true);
        return false;
    }

    if (!sChrRacesStore.LookupEntry(raceId) || !sChrClassesStore.LookupEntry(classId))
    {
        PSendSysMessage("Harness: invalid race %u or class %u.", raceId, classId);
        SetSentErrorMessage(true);
        return false;
    }

    if (!sObjectMgr.GetPlayerInfo(raceId, classId))
    {
        PSendSysMessage("Harness: race %u and class %u is not a valid combination.", raceId, classId);
        SetSentErrorMessage(true);
        return false;
    }

    std::string safeName = nameStr;
    if (!normalizePlayerName(safeName) || ObjectMgr::CheckPlayerName(safeName, true) != CHAR_NAME_SUCCESS)
    {
        PSendSysMessage("Harness: '%s' is not a usable character name.", nameStr);
        SetSentErrorMessage(true);
        return false;
    }

    if (sObjectMgr.GetPlayerGuidByName(safeName))
    {
        PSendSysMessage("Harness: the name '%s' is already taken.", safeName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    if (!Player::ValidateAppearance(raceId, gender, 0, 0, 0, 0, 0))
    {
        PSendSysMessage("Harness: default appearance is not valid for race %u gender %u.", raceId, gender);
        SetSentErrorMessage(true);
        return false;
    }

    // The session only supplies the account, security and locale that SaveNewPlayer records.
    // It is never registered with the world, and its destructor is a no-op without a player.
    WorldSession provisioningSession(accountId, nullptr, sAccountMgr.GetSecurity(accountId), 0, LOCALE_enUS);

    uint32 const guidLow = sObjectMgr.GeneratePlayerLowGuid();
    if (!Player::SaveNewPlayer(&provisioningSession, guidLow, safeName, raceId, classId, gender, 0, 0, 0, 0, 0))
    {
        PSendSysMessage("Harness: failed to create '%s'.", safeName.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("created name=%s guid=%u account=%u race=%u class=%u",
        safeName.c_str(), guidLow, accountId, raceId, classId);
    return true;
}

// .harness path <character> <x> <y> <z>
// Reports what the navigation mesh answers for a route the character would have to walk.
// Detour degrades quietly, returning a partial route or a straight-line shortcut rather than
// failing, so without this a bot that cannot reach somewhere is indistinguishable from a bot
// that is merely slow.
bool ChatHandler::HandleHarnessPathCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    float x, y, z;
    if (!ExtractFloat(&args, x) || !ExtractFloat(&args, y) || !ExtractFloat(&args, z))
    {
        SendSysMessage("Syntax: .harness path <character> <x> <y> <z> [trigger]");
        SetSentErrorMessage(true);
        return false;
    }

    // Optional, and the only honest way to ask whether a route arrives. A dungeon portal is
    // a volume, not a point, and some are tall: measuring the distance to the trigger's
    // centre calls Ragefire Chasm a failure while the ghost is standing inside the doorway.
    uint32 triggerId = 0;
    ExtractUInt32(&args, triggerId);

    PathInfo path(pTarget);
    path.calculate(x, y, z);

    PointsArray const& points = path.getFullPath();
    Vector3 const actualEnd = path.getActualEndPosition();

    // Decoded as flags rather than matched as a value. The interesting results are
    // combinations: NORMAL|NOT_USING_PATH is a straight line drawn because the mesh was
    // unavailable, and reporting that as an unhelpful "OTHER" once hid a whole sweep's
    // worth of false passes.
    static struct { uint32 flag; char const* name; } const pathFlags[] =
    {
        { PATHFIND_NORMAL,         "NORMAL" },
        { PATHFIND_SHORTCUT,       "SHORTCUT" },
        { PATHFIND_INCOMPLETE,     "INCOMPLETE" },
        { PATHFIND_NOPATH,         "NOPATH" },
        { PATHFIND_NOT_USING_PATH, "NOT_USING_PATH" },
        { PATHFIND_DEST_FORCED,    "DEST_FORCED" },
        { PATHFIND_FLYPATH,        "FLYPATH" },
        { PATHFIND_UNDERWATER,     "UNDERWATER" },
        { PATHFIND_CASTER,         "CASTER" },
    };

    uint32 const pathType = uint32(path.getPathType());
    std::string typeNames;
    for (auto const& entry : pathFlags)
    {
        if (!(pathType & entry.flag))
            continue;
        if (!typeNames.empty())
            typeNames += "|";
        typeNames += entry.name;
    }
    char const* typeName = typeNames.empty() ? "BLANK" : typeNames.c_str();

    // The same test, with the same tolerance, that UpdateCorpseRun uses to decide whether to
    // step through the portal, so a survey and the bot cannot disagree about arriving.
    int32 arrived = -1;
    if (triggerId)
    {
        AreaTriggerEntry const* pTrigger = sObjectMgr.GetAreaTrigger(triggerId);
        if (!pTrigger)
        {
            PSendSysMessage("Area trigger %u does not exist.", triggerId);
            SetSentErrorMessage(true);
            return false;
        }

        arrived = IsPointInAreaTriggerZone(pTrigger, pTarget->GetMapId(),
            actualEnd.x, actualEnd.y, actualEnd.z, 5.0f) ? 1 : 0;
    }

    PSendSysMessage("path type=%s(0x%x) points=%u length=%.1f from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f reached=%.1f,%.1f,%.1f shortfall=%.1f arrived=%d",
        typeName, uint32(path.getPathType()), uint32(points.size()), path.Length(),
        pTarget->GetPositionX(), pTarget->GetPositionY(), pTarget->GetPositionZ(),
        x, y, z, actualEnd.x, actualEnd.y, actualEnd.z,
        std::sqrt((actualEnd.x - x) * (actualEnd.x - x) +
                  (actualEnd.y - y) * (actualEnd.y - y) +
                  (actualEnd.z - z) * (actualEnd.z - z)),
        arrived);

    return true;
}

// .harness rewardquest <character> <quest>
// Grants a quest as though the character had walked up to the ender and handed it in.
//
// Attunements are checked with GetQuestRewardStatus, and .quest complete does not satisfy it:
// that stops at COMPLETE, and only a quest flagged auto-rewarded goes the rest of the way. So
// there is otherwise no way to attune a bot to anything, and no way for a headless character to
// reach a quest ender to do it honestly.
bool ChatHandler::HandleHarnessRewardQuestCommand(char* args)
{
    if (!sWorld.getConfig(CONFIG_BOOL_HARNESS_ENABLE))
    {
        SendSysMessage("Harness: disabled. Set Harness.Enable = 1 to use this command.");
        SetSentErrorMessage(true);
        return false;
    }

    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    uint32 questId = 0;
    if (!ExtractUInt32(&args, questId))
    {
        SendSysMessage("Syntax: .harness rewardquest <character> <quest>");
        SetSentErrorMessage(true);
        return false;
    }

    Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);
    if (!pQuest)
    {
        PSendSysMessage("Quest %u does not exist.", questId);
        SetSentErrorMessage(true);
        return false;
    }

    if (pTarget->GetQuestStatus(questId) == QUEST_STATUS_NONE)
    {
        // AddQuest asserts on a full log rather than refusing, which would take the server
        // down with it.
        if (!pTarget->CanAddQuest(pQuest, false))
        {
            PSendSysMessage("Harness: %s cannot take quest %u.", pTarget->GetName(), questId);
            SetSentErrorMessage(true);
            return false;
        }
        pTarget->AddQuest(pQuest, nullptr);
    }

    // Objectives first, since RewardQuest takes the required items back off the player.
    pTarget->FullQuestComplete(questId);
    pTarget->RewardQuest(pQuest, 0, pTarget, false);

    PSendSysMessage("quest=%u rewarded=%u", questId,
        pTarget->GetQuestRewardStatus(questId) ? 1 : 0);
    return true;
}

// .harness loadmmaps <map>
// Pulls every navigation tile of a map into memory at once.
//
// Tiles are normally loaded alongside the grids around a player, and a path query whose
// destination sits in an unloaded tile does not fail: it silently abandons the mesh and
// answers with a straight line. That makes any attempt to survey routes from a distance
// report success everywhere. Loading the map up front is what makes such a survey mean
// anything. A continent costs well under a gigabyte, which is a fine trade for a test
// server and no reason to do this on a live one.
bool ChatHandler::HandleHarnessLoadMmapsCommand(char* args)
{
    if (!sWorld.getConfig(CONFIG_BOOL_HARNESS_ENABLE))
    {
        SendSysMessage("Harness: disabled. Set Harness.Enable = 1 to use this command.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 mapId = 0;
    if (!ExtractUInt32(&args, mapId))
    {
        SendSysMessage("Syntax: .harness loadmmaps <map>");
        SetSentErrorMessage(true);
        return false;
    }

    MMAP::MMapManager* pManager = MMAP::MMapFactory::createOrGetMMapManager();

    uint32 loaded = 0;
    for (int32 x = 0; x < 64; ++x)
        for (int32 y = 0; y < 64; ++y)
            if (pManager->loadMap(mapId, x, y))
                ++loaded;

    PSendSysMessage("mmaps map=%u newly_loaded=%u total_tiles=%u",
        mapId, loaded, pManager->getLoadedTilesCount());
    return true;
}

// .harness graveyard <map> <x> <y> <z> [team]
// Where a ghost dying at that spot would be sent. Asking the engine beats assuming, because
// for a death inside an instance the answer is a graveyard out on the entrance map, chosen by
// faction, and that release point is where any corpse run actually begins.
bool ChatHandler::HandleHarnessGraveyardCommand(char* args)
{
    if (!sWorld.getConfig(CONFIG_BOOL_HARNESS_ENABLE))
    {
        SendSysMessage("Harness: disabled. Set Harness.Enable = 1 to use this command.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 mapId = 0;
    float x, y, z;
    uint32 team = uint32(HORDE);

    if (!ExtractUInt32(&args, mapId) || !ExtractFloat(&args, x) ||
        !ExtractFloat(&args, y) || !ExtractFloat(&args, z))
    {
        SendSysMessage("Syntax: .harness graveyard <map> <x> <y> <z> [team]");
        SetSentErrorMessage(true);
        return false;
    }

    ExtractUInt32(&args, team);

    WorldSafeLocsEntry const* pGraveyard = sObjectMgr.GetClosestGraveYard(x, y, z, mapId, Team(team));
    if (!pGraveyard)
    {
        PSendSysMessage("graveyard none map=%u team=%u", mapId, team);
        return true;
    }

    PSendSysMessage("graveyard id=%u map=%u x=%.2f y=%.2f z=%.2f",
        pGraveyard->ID, pGraveyard->map_id, pGraveyard->x, pGraveyard->y, pGraveyard->z);
    return true;
}

// .harness spells <character>
// What the bot's spell population actually produced, slot by slot.
//
// Population matches spells by name into named struct slots, and a slot that never matches
// stays null forever. From the outside that is indistinguishable from a rotation that
// declines to cast: the shaman cure slots had no matcher at all and simply read as Horde
// never dispelling. Naming the empty slots is the difference between the two.
bool ChatHandler::HandleHarnessSpellsCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    PlayerBotEntry const* pEntry = pTarget->GetSession()->GetBot();
    CombatBotBaseAI const* pAI = pEntry ? dynamic_cast<CombatBotBaseAI const*>(pEntry->ai.get()) : nullptr;
    if (!pAI)
    {
        PSendSysMessage("Harness: '%s' is not running a combat bot AI.", pTarget->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    std::vector<CombatBotBaseAI::SpellSlot> const slots = pAI->GetSpellSlots();

    uint32 filled = 0;
    for (auto const& slot : slots)
        if (slot.spell)
            ++filled;

    SpellEntry const* pResurrection = pAI->m_resurrectionSpell;
    PSendSysMessage("spells character=%s class=%u level=%u role=%u slots=%u filled=%u resurrection=%u",
        pTarget->GetName(), pTarget->GetClass(), pTarget->GetLevel(), uint32(pAI->GetRole()),
        uint32(slots.size()), filled, pResurrection ? pResurrection->Id : 0);

    // The spell name goes last because it contains spaces, so a reader can take the rest of
    // the line without having to quote anything.
    for (auto const& slot : slots)
    {
        if (!slot.spell)
        {
            PSendSysMessage("slot %s id=0 rank=0 level=0 known=0 name=", slot.name);
            continue;
        }

        // level is the spell's own level rather than the bot's, because a slot holding a
        // rank far below the bot is the quiet version of this failure: rogue poisons were
        // resolving to the level 30 rank on a level 60 bot and nothing said so.
        PSendSysMessage("slot %s id=%u rank=%u level=%u known=%u name=%s",
            slot.name, slot.spell->Id, slot.spell->GetRank(), slot.spell->spellLevel,
            pTarget->HasSpell(slot.spell->Id) ? 1 : 0, slot.spell->SpellName[0].c_str());
    }

    // What is actually planted right now, which for a shaman is the answer the slots above
    // cannot give. A totem slot is re-chosen every time it comes up empty, from the group
    // standing in range at that moment, so the resting choice recorded at spawn and the totem
    // on the ground are allowed to differ and the difference is the whole point.
    static char const* const totemSlotNames[MAX_TOTEM_SLOT] = { "fire", "earth", "water", "air" };
    for (uint32 i = 0; i < MAX_TOTEM_SLOT; ++i)
    {
        Totem const* pTotem = pTarget->GetTotem(TotemSlot(i));
        if (!pTotem)
        {
            PSendSysMessage("totem %s id=0 name=", totemSlotNames[i]);
            continue;
        }

        PSendSysMessage("totem %s id=%u name=%s", totemSlotNames[i],
            pTotem->GetSpell(), pTotem->GetName());
    }

    return true;
}

// .harness items <character>
// Everything the character is wearing, carrying in the backpack, or has waiting in the mail.
//
// The mail count is the part that matters and the reason this is not just a convenience. Gear
// that a swap displaces goes to the bags, and to the mail when the bags will not take it, so a
// test that only looked at bags and equipment could not tell an item that was mailed away from
// one that was destroyed. Those are the two outcomes it most needs to distinguish.
bool ChatHandler::HandleHarnessItemsCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    uint32 equipped = 0;
    for (uint32 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        if (pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            equipped++;

    uint32 bag = 0;
    for (uint32 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            bag++;

    // Mail lives on the MasterPlayer rather than the Player.
    MasterPlayer* pMaster = pTarget->GetSession()->GetMasterPlayer();

    uint32 mailed = 0;
    if (pMaster)
        for (auto itr = pMaster->GetMailBegin(); itr != pMaster->GetMailEnd(); ++itr)
            mailed += uint32((*itr)->items.size());

    PSendSysMessage("items character=%s equipped=%u bag=%u mailed=%u",
        pTarget->GetName(), equipped, bag, mailed);

    for (uint32 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            PSendSysMessage("equipped slot=%u entry=%u count=%u", i, pItem->GetEntry(), pItem->GetCount());
    }

    for (uint32 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            PSendSysMessage("bag slot=%u entry=%u count=%u", i, pItem->GetEntry(), pItem->GetCount());
    }

    if (pMaster)
        for (auto itr = pMaster->GetMailBegin(); itr != pMaster->GetMailEnd(); ++itr)
            for (auto const& item : (*itr)->items)
                PSendSysMessage("mailed entry=%u", item.itemId);

    return true;
}

// .harness equipnew <character>
// Runs the bot's own "something new turned up in my bags" pass.
//
// It exists because that pass is otherwise reachable only from trade completion, and there is
// no way to conduct a trade over SOAP. That left the one code path that decides what happens
// to a bot's current gear when better gear arrives as the only part of this work with no test
// at all, which is a poor place for a gap: until recently it freed the slot by destroying
// whatever was in it.
bool ChatHandler::HandleHarnessEquipNewCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    PlayerBotEntry const* pEntry = pTarget->GetSession()->GetBot();
    CombatBotBaseAI* pAI = pEntry ? dynamic_cast<CombatBotBaseAI*>(pEntry->ai.get()) : nullptr;
    if (!pAI)
    {
        PSendSysMessage("Harness: '%s' is not running a combat bot AI.", pTarget->GetName());
        SetSentErrorMessage(true);
        return false;
    }

    pAI->EquipOrUseNewItem();

    PSendSysMessage("equipnew character=%s", pTarget->GetName());
    return true;
}

// TalentTab.dbc carries tree names but the server's format string discards the name column, and
// an unnamed tree makes a build report unreadable at the moment you are trying to tell fire from
// frost. Keyed by tab id rather than by TalentTabEntry::tabpage, because that ordering column is
// wrong for mage: tabs 41 (Fire) and 81 (Arcane) both claim page 0.
static char const* GetTalentTabName(uint32 tabId)
{
    switch (tabId)
    {
        case 161: return "Arms";
        case 164: return "Fury";
        case 163: return "Protection";
        case 382: return "Holy";
        case 383: return "Protection";
        case 381: return "Retribution";
        case 361: return "Beast Mastery";
        case 363: return "Marksmanship";
        case 362: return "Survival";
        case 182: return "Assassination";
        case 181: return "Combat";
        case 183: return "Subtlety";
        case 201: return "Discipline";
        case 202: return "Holy";
        case 203: return "Shadow";
        case 261: return "Elemental";
        case 263: return "Enhancement";
        case 262: return "Restoration";
        case 81:  return "Arcane";
        case 41:  return "Fire";
        case 61:  return "Frost";
        case 302: return "Affliction";
        case 303: return "Demonology";
        case 301: return "Destruction";
        case 283: return "Balance";
        case 281: return "Feral Combat";
        case 282: return "Restoration";
    }

    return "";
}

bool ChatHandler::HandleHarnessTalentsCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    // Premade specs are applied with LearnSpell rather than LearnTalent, which grants the spell
    // without checking that the tree beneath it was paid for. So an authored build can be
    // illegal in ways the game would never allow, and nothing says so at apply time. Recompute
    // the rules here instead of trusting that the build got in legitimately.
    struct TabState
    {
        uint32 tabId = 0;
        uint32 tabPage = 0;
        uint32 spent = 0;
        std::map<uint32 /*row*/, uint32 /*points*/> spentByRow;
    };

    std::map<uint32 /*tab id*/, TabState> tabs;
    for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
    {
        TalentTabEntry const* pTabInfo = sTalentTabStore.LookupEntry(i);
        if (!pTabInfo || (pTarget->GetClassMask() & pTabInfo->ClassMask) == 0)
            continue;

        TabState& tab = tabs[pTabInfo->TalentTabID];
        tab.tabId = pTabInfo->TalentTabID;
        tab.tabPage = pTabInfo->tabpage;
    }

    struct LearnedTalent
    {
        uint32 talentId = 0;
        uint32 tabId = 0;
        uint32 row = 0;
        uint32 rank = 0;
        uint32 maxRank = 0;
        uint32 spellId = 0;
        uint32 dependsOn = 0;
        uint32 dependsOnRank = 0;
    };

    std::vector<LearnedTalent> learned;
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* pTalentInfo = sTalentStore.LookupEntry(talentId);
        if (!pTalentInfo || !tabs.count(pTalentInfo->TalentTab))
            continue;

        uint32 maxRank = 0;
        while (maxRank < MAX_TALENT_RANK && pTalentInfo->RankID[maxRank])
            ++maxRank;

        // Rank is one-based and counted from the highest known, because the apply path learns
        // intermediate ranks too and the top one is what the player actually has.
        uint32 rank = 0;
        uint32 spellId = 0;
        for (uint32 i = 0; i < maxRank; ++i)
        {
            if (pTarget->HasSpell(pTalentInfo->RankID[i]))
            {
                rank = i + 1;
                spellId = pTalentInfo->RankID[i];
            }
        }

        if (!rank)
            continue;

        LearnedTalent entry;
        entry.talentId = talentId;
        entry.tabId = pTalentInfo->TalentTab;
        entry.row = pTalentInfo->Row;
        entry.rank = rank;
        entry.maxRank = maxRank;
        entry.spellId = spellId;
        entry.dependsOn = pTalentInfo->DependsOn;
        entry.dependsOnRank = pTalentInfo->DependsOnRank;
        learned.push_back(entry);

        TabState& tab = tabs[pTalentInfo->TalentTab];
        tab.spent += rank;
        tab.spentByRow[pTalentInfo->Row] += rank;
    }

    uint32 totalSpent = 0;
    for (auto const& itr : tabs)
        totalSpent += itr.second.spent;

    // A talent on row r needs five points per row above it in the same tree.
    uint32 illegal = 0;
    for (auto const& entry : learned)
    {
        TabState const& tab = tabs[entry.tabId];

        uint32 spentAbove = 0;
        for (auto const& itrRow : tab.spentByRow)
            if (itrRow.first < entry.row)
                spentAbove += itrRow.second;

        char const* problem = nullptr;
        if (spentAbove < entry.row * 5)
            problem = "tier";
        else if (entry.dependsOn)
        {
            TalentEntry const* pPrereq = sTalentStore.LookupEntry(entry.dependsOn);
            uint32 prereqRank = 0;
            if (pPrereq)
                for (uint32 i = 0; i < MAX_TALENT_RANK && pPrereq->RankID[i]; ++i)
                    if (pTarget->HasSpell(pPrereq->RankID[i]))
                        prereqRank = i + 1;

            if (prereqRank <= entry.dependsOnRank)
                problem = "prereq";
        }

        if (problem)
        {
            ++illegal;
            PSendSysMessage("illegal talent=%u tab=%u row=%u rank=%u reason=%s spell=%u",
                entry.talentId, entry.tabId, entry.row, entry.rank, problem, entry.spellId);
        }
    }

    // available is what the level entitles the character to, so spent below it means an authored
    // build is leaving points on the table -- the quiet failure when a level 39 template lands
    // on a level 45 bot. free is what the server itself thinks is unspent.
    uint32 const available = pTarget->GetLevel() < 10 ? 0 : pTarget->GetLevel() - 9;
    PSendSysMessage("talents character=%s class=%u level=%u spent=%u available=%u free=%u illegal=%u",
        pTarget->GetName(), pTarget->GetClass(), pTarget->GetLevel(),
        totalSpent, available, pTarget->GetFreeTalentPoints(), illegal);

    for (auto const& itr : tabs)
    {
        PSendSysMessage("tab id=%u page=%u spent=%u name=%s", itr.second.tabId,
            itr.second.tabPage, itr.second.spent, GetTalentTabName(itr.second.tabId));
    }

    for (auto const& entry : learned)
    {
        SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(entry.spellId);
        PSendSysMessage("talent id=%u tab=%u row=%u rank=%u max=%u spell=%u name=%s",
            entry.talentId, entry.tabId, entry.row, entry.rank, entry.maxRank, entry.spellId,
            pSpell ? pSpell->SpellName[0].c_str() : "");
    }

    return true;
}

bool ChatHandler::HandleHarnessInfoCommand(char* args)
{
    Player* pTarget = GetHarnessTarget(&args);
    if (!pTarget)
    {
        SetSentErrorMessage(true);
        return false;
    }

    // deathstate distinguishes a corpse still waiting for a resurrection from a released
    // ghost, which alive= alone cannot express and wipe recovery turns on. teleporting and
    // motion say whether the AI is running at all: a bot stuck mid teleport is not ticked,
    // which looks identical from the outside to one that has decided to stand still.
    PSendSysMessage("name=%s guid=%u level=%u map=%u instance=%u zone=%u alive=%u deathstate=%u corpse=%u teleporting=%u motion=%u x=%.2f y=%.2f z=%.2f",
        pTarget->GetName(), pTarget->GetGUIDLow(), pTarget->GetLevel(),
        pTarget->GetMapId(), pTarget->GetInstanceId(), pTarget->GetZoneId(),
        pTarget->IsAlive() ? 1 : 0, uint32(pTarget->GetDeathState()),
        pTarget->GetCorpse() ? 1 : 0,
        pTarget->IsBeingTeleported() ? 1 : 0,
        uint32(pTarget->GetMotionMaster()->GetCurrentMovementGeneratorType()),
        pTarget->GetPositionX(), pTarget->GetPositionY(), pTarget->GetPositionZ());

    // The resources a bot is not supposed to be given for free, none of which could be seen
    // from outside. Without these, a bot handed a full bar and a bot that sat and drank for
    // thirty seconds are the same observation, and so are a hunter with a quiver and one that
    // silently stopped shooting an hour ago. mhenchant covers the weapon imbues and poisons,
    // which are applied as a temporary enchant and otherwise leave no trace at all.
    Powers const powerType = pTarget->GetPowerType();
    uint32 const ammoId = pTarget->GetUInt32Value(PLAYER_AMMO_ID);
    Item const* pMainHand = pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item const* pOffHand = pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    // incombat and selfres are what tell a resurrection during a fight apart from one after it.
    // The two are worth a great deal more together than separately: a bot that stood back up is
    // only interesting if something was still attacking at the time, and selfres names the
    // charge the engine is holding for this death, so a soulstone that was never applied and an
    // Ankh that was never carried stop looking like an ability that failed to fire.
    // form gates more of a druid's spell list than any other single piece of state, and it is
    // invisible from outside: a druid in bear form declining to cast and a druid missing the
    // spell entirely produce the same silence.
    PSendSysMessage("health=%u maxhealth=%u power=%u maxpower=%u powertype=%u ammo=%u ammocount=%u mhenchant=%u ohenchant=%u incombat=%u selfres=%u form=%u",
        pTarget->GetHealth(), pTarget->GetMaxHealth(),
        pTarget->GetPower(powerType), pTarget->GetMaxPower(powerType), uint32(powerType),
        ammoId, ammoId ? pTarget->GetItemCount(ammoId) : 0,
        pMainHand ? pMainHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) : 0,
        pOffHand ? pOffHand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) : 0,
        pTarget->IsInCombat() ? 1 : 0,
        pTarget->GetUInt32Value(PLAYER_SELF_RES_SPELL),
        uint32(pTarget->GetShapeshiftForm()));

    // What the bot is carrying, which nothing outside the server could see at all. A consumable
    // that is supposed to be spent looks exactly like one that is not until the stack is counted,
    // and a rogue's poison vials are the case in hand: the enchant on the blade above says a
    // poison was applied and says nothing about whether applying it cost anything.
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item const* pItem = pTarget->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            PSendSysMessage("item entry=%u count=%u slot=%d name=%s", pItem->GetEntry(),
                pItem->GetCount(), i, pItem->GetProto()->Name1);
    }

    Group* pGroup = pTarget->GetGroup();
    if (!pGroup)
    {
        SendSysMessage("group=0 members=0 raid=0");
        return true;
    }

    PSendSysMessage("group=1 members=%u raid=%u leader=%s",
        pGroup->GetMembersCount(), pGroup->isRaidGroup() ? 1 : 0, pGroup->GetLeaderName());

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
            PSendSysMessage("member name=%s class=%u level=%u subgroup=%u alive=%u deathstate=%u map=%u zone=%u incombat=%u",
                pMember->GetName(), pMember->GetClass(), pMember->GetLevel(),
                pGroup->GetMemberGroup(pMember->GetObjectGuid()),
                pMember->IsAlive() ? 1 : 0, uint32(pMember->GetDeathState()),
                pMember->GetMapId(), pMember->GetZoneId(),
                pMember->IsInCombat() ? 1 : 0);
    }

    return true;
}
