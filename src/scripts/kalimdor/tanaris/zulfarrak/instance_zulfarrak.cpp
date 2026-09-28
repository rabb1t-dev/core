/*
* Copyright (C) 2008-2010 TrinityCore <http://www.trinitycore.org/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "scriptPCH.h"
#include "zulfarrak.h"

#define NPC_GAHZRILLA 7273
#define PATH_ADDS 81553

int const pyramidSpawnTotal = 54;
/* list of wave spawns: 0 = wave ID, 1 = creature id, 2 = x, 3 = y
no z coordinat b/c they're all the same */
float pyramidSpawns [pyramidSpawnTotal][4] =
{
    {1, 7789, 1894.64f, 1206.29f},
    {1, 7787, 1890.08f, 1218.68f},
    {1, 8876, 1883.76f, 1222.3f},
    {1, 7789, 1874.18f, 1221.24f},
    {1, 7787, 1892.28f, 1225.49f},
    {1, 7788, 1889.94f, 1212.21f},
    {1, 7787, 1879.02f, 1223.06f},
    {1, 7789, 1874.45f, 1204.44f},
    {1, 8876, 1898.23f, 1217.97f},
    {1, 7787, 1882.07f, 1225.7f},
    {1, 8877, 1896.46f, 1205.62f},
    {1, 7787, 1886.97f, 1225.86f},
    {1, 7787, 1894.72f, 1221.91f},
    {1, 7787, 1883.5f, 1218.25f},
    {1, 7787, 1886.93f, 1221.4f},
    {1, 8876, 1889.82f, 1222.51f},
    {1, 7788, 1893.07f, 1215.26f},
    {1, 7788, 1878.57f, 1214.16f},
    {1, 7788, 1883.74f, 1212.35f},
    {1, 8877, 1877, 1207.27f},
    {1, 8877, 1873.63f, 1204.65f},
    {1, 8876, 1877.4f, 1216.41f},
    {1, 8877, 1899.63f, 1202.52f},
    {2, 7789, 1902.83f, 1223.41f},
    {2, 8876, 1889.82f, 1222.51f},
    {2, 7787, 1883.5f, 1218.25f},
    {2, 7788, 1883.74f, 1212.35f},
    {2, 8877, 1877, 1207.27f},
    {2, 7787, 1890.08f, 1218.68f},
    {2, 7789, 1894.64f, 1206.29f},
    {2, 8876, 1877.4f, 1216.41f},
    {2, 7787, 1892.28f, 1225.49f},
    {2, 7788, 1893.07f, 1215.26f},
    {2, 8877, 1896.46f, 1205.62f},
    {2, 7789, 1874.45f, 1204.44f},
    {2, 7789, 1874.18f, 1221.24f},
    {2, 7787, 1879.02f, 1223.06f},
    {2, 8876, 1898.23f, 1217.97f},
    {2, 7787, 1882.07f, 1225.7f},
    {2, 8877, 1873.63f, 1204.65f},
    {2, 7787, 1886.97f, 1225.86f},
    {2, 7788, 1878.57f, 1214.16f},
    {2, 7787, 1894.72f, 1221.91f},
    {2, 7787, 1886.93f, 1221.4f},
    {2, 8876, 1883.76f, 1222.3f},
    {2, 7788, 1889.94f, 1212.21f},
    {2, 8877, 1899.63f, 1202.52f},
    {3, 7788, 1878.57f, 1214.16f},
    {3, 7787, 1894.72f, 1221.91f},
    {3, 7787, 1886.93f, 1221.4f},
    {3, 8876, 1883.76f, 1222.3f},
    {3, 7788, 1889.94f, 1212.21f},
    {3, 7275, 1889.23f, 1207.72f},
    {3, 7796, 1879.77f, 1207.96f}
};

float Spawnsway[2][3] =
{
    {1884.86f, 1228.62f, 9.0f},
    {1887.53f, 1263.0f, 41.0f}
};

// How the trolls are let up the stairs.
//
// The event only reads as a gauntlet if the group holding the landing is fighting a handful at a
// time. Three numbers decide that and all three were wrong.
//
// The group size was a ramp with no ceiling: two, then three, then four, climbing by one every ten
// seconds for as long as the wave lasted. A wave of twenty three was therefore fully dispatched
// inside fifty seconds and the last group of it was seven abreast, which is not "a few at a time"
// by the time it reaches the top.
//
// The ceiling is the one that actually decides the fight, and there was none: nothing counted what
// was already up there. A group that is falling behind got sent more, and more again, which is the
// definition of unwinnable. Holding the released-and-still-alive count at six means falling behind
// slows the event down rather than speeding it up. The wave still ends only when all of it is
// dead, so this changes the pacing and not the amount of work.
uint32 const PYRAMID_RELEASE_INTERVAL = 10000;
uint32 const PYRAMID_RELEASE_MIN = 2;
uint32 const PYRAMID_RELEASE_MAX = 4;
uint32 const PYRAMID_MAX_RELEASED_ALIVE = 6;

struct instance_zulfarrak : public ScriptedInstance
{
public:
    instance_zulfarrak(Map* pMap) : ScriptedInstance(pMap)
    {
        Initialize();
    };

    uint32 GahzRillaEncounter;
    uint32 EndDoorEncounter;
    uint32 ZumrahEncounter;
    uint32 AntusulEncounter;
    std::string strInstData;

    uint64 UkorzGUID;
    uint64 ZumrahGUID;
    uint64 BlyGUID;
    uint64 WeegliGUID;
    uint64 OroGUID;
    uint64 RavenGUID;
    uint64 MurtaGUID;
    uint64 EndDoorGUID;
    uint32 PyramidPhase;
    uint32 major_wave_Timer;
    uint32 minor_wave_Timer;
    uint32 addGroupSize;
    uint32 waypoint;
    uint32 zumrahCleanupTimer;
    bool zumrahAddsPending;

    void Initialize() override
    {
        EndDoorEncounter = NOT_STARTED;
        GahzRillaEncounter = NOT_STARTED;
        ZumrahEncounter = NOT_STARTED;
        AntusulEncounter = NOT_STARTED;

        PyramidPhase = 0;
        major_wave_Timer = 0;
        minor_wave_Timer = 0;
        addGroupSize = 0;
        waypoint = 0;
        zumrahCleanupTimer = 0;
        zumrahAddsPending = false;

        UkorzGUID = 0;
        ZumrahGUID = 0;
        BlyGUID = 0;
        WeegliGUID = 0;
        OroGUID = 0;
        RavenGUID = 0;
        MurtaGUID = 0;
        EndDoorGUID = 0;
    }

    void OnCreatureCreate(Creature* pCreature) override
    {
        switch (pCreature->GetEntry())
        {
            case ENTRY_ZUMRAH:
                ZumrahGUID = pCreature->GetGUID();
                break;
            case ENTRY_BLY:
                BlyGUID = pCreature->GetGUID();
                //pCreature->GetCharmInfo()->SetReactState(REACT_PASSIVE); // starts out passive (in a cage)
                break;
            case ENTRY_RAVEN:
                RavenGUID = pCreature->GetGUID();
                //pCreature->GetCharmInfo()->SetReactState(REACT_PASSIVE);// starts out passive (in a cage)
                break;
            case ENTRY_ORO:
                OroGUID = pCreature->GetGUID();
                //pCreature->GetCharmInfo()->SetReactState(REACT_PASSIVE);// starts out passive (in a cage)
                break;
            case ENTRY_WEEGLI:
                WeegliGUID = pCreature->GetGUID();
                //pCreature->GetCharmInfo()->SetReactState(REACT_PASSIVE);// starts out passive (in a cage)
                break;
            case ENTRY_MURTA:
                MurtaGUID = pCreature->GetGUID();
                //pCreature->GetCharmInfo()->SetReactState(REACT_PASSIVE);// starts out passive (in a cage)
                break;
            case ENTRY_UKORZ:
                UkorzGUID = pCreature->GetGUID();
                break;
            case NPC_GAHZRILLA:
                if (GahzRillaEncounter >= IN_PROGRESS)
                    pCreature->DisappearAndDie();
                else
                    GahzRillaEncounter = IN_PROGRESS;
                break;
        }
    }

    void OnObjectCreate(GameObject* pGo) override
    {
        switch (pGo->GetEntry())
        {
            case GO_END_DOOR:
                EndDoorGUID = pGo->GetGUID();
                if (EndDoorEncounter == DONE)
                    pGo->UseDoorOrButton(0, true);
                break;
        }
    }

    uint32 GetData(uint32 type) override
    {
        switch (type)
        {
            case EVENT_PYRAMID:
                return PyramidPhase;
            case EVENT_ZUMRAH:
                return ZumrahEncounter;
            case EVENT_ANTUSUL:
                return AntusulEncounter;
        }
        return 0;
    }

    uint64 GetData64(uint32 data) override
    {
        switch (data)
        {
            case ENTRY_ZUMRAH:
                return ZumrahGUID;
            case ENTRY_BLY:
                return BlyGUID;
            case ENTRY_RAVEN:
                return RavenGUID;
            case ENTRY_ORO:
                return OroGUID;
            case ENTRY_WEEGLI:
                return WeegliGUID;
            case ENTRY_MURTA:
                return MurtaGUID;
            case ENTRY_UKORZ:
                return UkorzGUID;
            case GO_END_DOOR:
                return EndDoorGUID;
        }
        return 0;
    }

    void SetData(uint32 type, uint32 data) override
    {
        switch (type)
        {
            case EVENT_PYRAMID:
                PyramidPhase = data;
                break;
            case EVENT_END_DOOR:
                EndDoorEncounter = data;
                break;
            case EVENT_ZUMRAH:
                ZumrahEncounter = data;
                // Arm the add sweep from the pull, so it only ever runs on a fight that started.
                if (data == IN_PROGRESS)
                    zumrahAddsPending = true;
                break;
            case EVENT_ANTUSUL:
                AntusulEncounter = data;
                break;
        };

        // Every encounter that can be finished, not just the door.
        //
        // Only the door was ever written down, so Antu'sul and Zum'rah lived in memory alone and
        // came back NOT_STARTED after any restart. For Antu'sul that re-arms his area trigger,
        // and his trigger is the pull: it summons four broodlings and puts them in combat with
        // the whole zone. A group that had already killed him could walk back through his room
        // after a restart and be attacked by an encounter that no longer exists.
        if ((type == EVENT_END_DOOR || type == EVENT_ANTUSUL || type == EVENT_ZUMRAH) &&
            data == DONE)
        {
            OUT_SAVE_INST_DATA;
            std::ostringstream saveStream;
            saveStream << EndDoorEncounter << " " << AntusulEncounter << " " << ZumrahEncounter;
            strInstData = saveStream.str();
            SaveToDB();
            OUT_SAVE_INST_DATA_COMPLETE;
        }
    }

    void Update(uint32 diff) override
    {
        // The backstop for Zum'rah's leftovers.
        //
        // The Ward already clears itself and its skeletons once he is dead or out of combat, and
        // that covers the ordinary wipe. It cannot cover the case where the group does the right
        // thing: kill every Ward, then wipe to Zum'rah anyway. There is then no Ward left to run
        // that cleanup, and the skeletons -- guardians of a spawner that no longer exists -- are
        // left standing with nothing in the encounter referring to them.
        //
        // Anchored on Zum'rah rather than searched for by area, because both entries only ever
        // exist near him, and asked on a timer rather than every tick since it is a sweep.
        // Gated on the encounter actually having been pulled, because the graves can be looted
        // before anyone engages him and those adds belong to whoever opened the grave, not to the
        // fight. Without the gate this sweep would delete them the moment they appeared.
        if (zumrahCleanupTimer <= diff)
        {
            zumrahCleanupTimer = 2000;

            if (zumrahAddsPending)
            {
                if (Creature* pZumrah = instance->GetCreature(ZumrahGUID))
                {
                    // Dead means the fight was won, alive and out of combat means it was wiped or
                    // run from. Neither should leave anything he raised standing for the next try.
                    if (!pZumrah->IsAlive() || !pZumrah->IsInCombat())
                    {
                        pZumrah->DespawnNearCreaturesByEntry(ENTRY_WARD_OF_ZUMRAH, 200.0f);
                        pZumrah->DespawnNearCreaturesByEntry(ENTRY_SKELETON_OF_ZUMRAH, 200.0f);
                        pZumrah->DespawnNearCreaturesByEntry(ENTRY_ZULFARRAK_ZOMBIE, 200.0f);
                        pZumrah->DespawnNearCreaturesByEntry(ENTRY_ZULFARRAK_DEAD_HERO, 200.0f);
                        zumrahAddsPending = false;
                    }
                }
            }
        }
        else
            zumrahCleanupTimer -= diff;

        switch (PyramidPhase)
        {
            case PYRAMID_NOT_STARTED:
            case PYRAMID_KILLED_ALL_TROLLS:
                break;
            case PYRAMID_ARRIVED_AT_STAIR:
                SpawnPyramidWave(1);
                SetData(EVENT_PYRAMID, PYRAMID_WAVE_1);
                major_wave_Timer = 120000;
                minor_wave_Timer = 0;
                addGroupSize = PYRAMID_RELEASE_MIN;
                break;
            case PYRAMID_WAVE_1:
                if (IsWaveAllDead())
                {
                    SetData(EVENT_PYRAMID, PYRAMID_PRE_WAVE_2);
                    major_wave_Timer = 10000; //give players a few seconds before wave 2 starts to rebuff
                }
                else if (minor_wave_Timer < diff)
                {
                    ReleaseNextGroup();
                    minor_wave_Timer = PYRAMID_RELEASE_INTERVAL;
                }
                else
                    minor_wave_Timer -= diff;
                break;
            case PYRAMID_PRE_WAVE_2:
                if (major_wave_Timer < diff)
                {
                    // beginning 2nd wave!
                    SpawnPyramidWave(2);
                    SetData(EVENT_PYRAMID, PYRAMID_WAVE_2);
                    minor_wave_Timer = 0;
                    addGroupSize = PYRAMID_RELEASE_MIN;
                }
                else
                    major_wave_Timer -= diff;
                break;
            case PYRAMID_WAVE_2:
                if (IsWaveAllDead())
                {
                    SpawnPyramidWave(3);
                    SetData(EVENT_PYRAMID, PYRAMID_PRE_WAVE_3);
                    major_wave_Timer = 5000; //give NPCs time to return to their home spots
                }
                else if (minor_wave_Timer < diff)
                {
                    ReleaseNextGroup();
                    minor_wave_Timer = PYRAMID_RELEASE_INTERVAL;
                }
                else
                    minor_wave_Timer -= diff;
                break;
            case PYRAMID_PRE_WAVE_3:
                if (major_wave_Timer < diff)
                {
                    // move NPCs to bottom of stair
                    MoveNPCIfAlive(ENTRY_BLY, 1887.92f, 1228.179f, 9.98f, 4.78f);
                    MoveNPCIfAlive(ENTRY_MURTA, 1891.57f, 1228.68f, 9.69f, 4.78f);
                    MoveNPCIfAlive(ENTRY_ORO, 1897.23f, 1228.34f, 9.43f, 4.78f);
                    MoveNPCIfAlive(ENTRY_RAVEN, 1883.68f, 1227.95f, 9.543f, 4.78f);
                    MoveNPCIfAlive(ENTRY_WEEGLI, 1878.02f, 1227.65f, 9.485f, 4.78f);
                    SetData(EVENT_PYRAMID, PYRAMID_WAVE_3);
                }
                else
                    major_wave_Timer -= diff;
                break;
            case PYRAMID_WAVE_3:
                if (IsWaveAllDead()) // move NPCS to their final positions
                {
                    SetData(EVENT_PYRAMID, PYRAMID_KILLED_ALL_TROLLS);
                    MoveNPCIfAlive(ENTRY_BLY, 1883.82f, 1200.83f, 8.87f, 1.32f);
                    MoveNPCIfAlive(ENTRY_MURTA, 1891.83f, 1201.45f, 8.87f, 1.32f);
                    MoveNPCIfAlive(ENTRY_ORO, 1894.50f, 1204.40f, 8.87f, 1.32f);
                    MoveNPCIfAlive(ENTRY_RAVEN, 1874.11f, 1206.17f, 8.87f, 1.32f);
                    MoveNPCIfAlive(ENTRY_WEEGLI, 1877.52f, 1199.63f, 8.87f, 1.32f);
                }
                break;
        };
    }

    std::list<uint64> addsAtBase, movedadds;

    void MoveNPCIfAlive(uint32 entry, float x, float y, float z, float o)
    {
        if (Creature* npc = instance->GetCreature(GetData64(entry)))
        {
            if (npc->IsAlive())
            {
                npc->GetMotionMaster()->MovePoint(1, x, y, z, MOVE_PATHFINDING | MOVE_WALK_MODE);
                npc->SetCombatStartPosition(x, y, z);
                npc->SetHomePosition(x, y, z, npc->GetOrientation());
            }
        }
    }

    void SpawnPyramidWave(uint32 wave)
    {
        // A wave starts from an empty pool.
        //
        // Both lists were appended to and never cleared, which was merely untidy while every
        // earlier entry was reliably dead by the time the next wave spawned -- and not untidy at
        // all once anything could put a live troll from an earlier spawn in them. IsWaveAllDead
        // walks both, so one stale live guid holds the wave open for ever and the release carries
        // on running underneath it. Clearing here is safe by construction: wave two is spawned
        // from PYRAMID_PRE_WAVE_2 and wave three from the all-dead branch of PYRAMID_WAVE_2, so
        // in both cases everything in the lists has already been established as dead.
        addsAtBase.clear();
        movedadds.clear();

        for (const auto& pyramidSpawn : pyramidSpawns)
        {
            if (pyramidSpawn[0] != (float)wave)
                continue;

            // Null checked. SummonCreature answers an unknown template or a placement it cannot
            // make with null, and this dereferenced it.
            if (Creature* ts = instance->SummonCreature(pyramidSpawn[1], pyramidSpawn[2],
                                                        pyramidSpawn[3], 8.87f, 0.0f))
                addsAtBase.push_back(ts->GetGUID());
        }
    }

    // How many of the trolls already sent up the stairs are still alive to be fought.
    //
    // Counted rather than tracked, because the alternative is a counter that has to be decremented
    // from a death hook the summons do not have. An entry the map no longer knows about is not
    // alive, which is the same reading IsWaveAllDead takes of it.
    uint32 CountReleasedAlive()
    {
        uint32 alive = 0;
        for (const auto& guid : movedadds)
            if (Creature* add = instance->GetCreature(guid))
                if (add->IsAlive())
                    ++alive;

        return alive;
    }

    // One tick of the release: send the next group up, then widen the group for the tick after.
    void ReleaseNextGroup()
    {
        SendAddsUpStairs(addGroupSize);

        if (addGroupSize < PYRAMID_RELEASE_MAX)
            ++addGroupSize;
    }

    bool IsWaveAllDead()
    {
        for (const auto& guid : addsAtBase)
        {
            if (Creature* add = instance->GetCreature(guid))
            {
                if (add->IsAlive())
                    return false;
            }
        }
        for (const auto& guid : movedadds)
        {
            if (Creature* add = instance->GetCreature((guid)))
            {
                if (add->IsAlive())
                    return false;
            }
        }
        return true;
    }

    void SendAddsUpStairs(uint32 count)
    {
        // Never more than a handful loose on the landing at once. See PYRAMID_MAX_RELEASED_ALIVE:
        // this is what stops a group that is losing from being handed the rest of the pyramid,
        // and it is why the event now waits for the group rather than the other way round.
        uint32 const alreadyUp = CountReleasedAlive();
        if (alreadyUp >= PYRAMID_MAX_RELEASED_ALIVE)
            return;

        uint32 const room = PYRAMID_MAX_RELEASED_ALIVE - alreadyUp;
        if (count > room)
            count = room;

        //pop a add from list, send him up the stairs...
        for (uint32 addCount = 0; addCount < count && !addsAtBase.empty(); addCount++)
        {
            if (Creature* add = instance->GetCreature(*addsAtBase.begin()))
            {
                if (add->IsAlive())
                {
                    add->GetMotionMaster()->MovePoint(0, 1880 + urand(0, 10), 1274, 42, MOVE_PATHFINDING | MOVE_RUN_MODE);
                    add->SetWalk(false);
                }
                movedadds.push_back(add->GetGUID());
            }
            addsAtBase.erase(addsAtBase.begin());
        }
    }

    char const* Save() override
    {
        return strInstData.c_str();
    }

    void Load(char const* chrIn) override
    {
        if (!chrIn)
        {
            OUT_LOAD_INST_DATA_FAIL;
            return;
        }

        OUT_LOAD_INST_DATA(chrIn);
        std::istringstream loadStream(chrIn);

        // Reads short on saves written before the other two encounters were recorded, which
        // leaves them at the NOT_STARTED the constructor set. That is the right answer for old
        // data: it says nothing about them either way.
        loadStream >> EndDoorEncounter >> AntusulEncounter >> ZumrahEncounter;

        // Anything mid-fight when the server went down is not progress. Only a kill survives a
        // restart, so a wipe still leaves the encounter there to be retried.
        if (EndDoorEncounter != DONE)
            EndDoorEncounter = NOT_STARTED;
        if (AntusulEncounter != DONE)
            AntusulEncounter = NOT_STARTED;
        if (ZumrahEncounter != DONE)
            ZumrahEncounter = NOT_STARTED;

        OUT_LOAD_INST_DATA_COMPLETE;
    }
};

InstanceData* GetInstanceData_instance_zulfarak(Map* pMap)
{
    return new instance_zulfarrak(pMap);
}


void AddSC_instance_zulfarrak()
{
    Script* newscript;
    newscript = new Script;
    newscript->Name = "instance_zulfarrak";
    newscript->GetInstanceData = &GetInstanceData_instance_zulfarak;
    newscript->RegisterSelf();
}
