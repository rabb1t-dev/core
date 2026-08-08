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
            PSendSysMessage("member name=%s class=%u level=%u subgroup=%u alive=%u deathstate=%u map=%u zone=%u",
                pMember->GetName(), pMember->GetClass(), pMember->GetLevel(),
                pGroup->GetMemberGroup(pMember->GetObjectGuid()),
                pMember->IsAlive() ? 1 : 0, uint32(pMember->GetDeathState()),
                pMember->GetMapId(), pMember->GetZoneId());
    }

    return true;
}
