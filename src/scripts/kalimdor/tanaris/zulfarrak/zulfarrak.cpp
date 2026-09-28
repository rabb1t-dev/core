/*
 * Copyright (C) 2008-2010 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
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

/*######
## npc_sergeant_bly
######*/

enum blyAndCrewFactions
{
    FACTION_HOSTILE           = 14,
    FACTION_FRIENDLY          = 35,  //while in cages (so the trolls won't attack them while they're caged)
    FACTION_FREED             = 250  //after release (so they'll be hostile towards trolls)
};

enum blySays
{
    SAY_1       = 3882,
    SAY_2       = 3884,
    SAY_WEEGLI  = 3811
};

enum blySpells
{
    SPELL_SHIELD_BASH          = 11972,
    SPELL_REVENGE              = 12170
};

#define GOSSIP_BLY                  "That's it! I'm tired of helping you out.  It's time we settled things on the battlefield!"


struct npc_sergeant_blyAI : public ScriptedAI
{
    npc_sergeant_blyAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        pInstance = pCreature->GetInstanceData();
        postGossipStep = 0;
        Text_Timer = 0;
        PlayerGUID = 0;
        Reset();
    }

    InstanceData* pInstance;

    uint32 postGossipStep;
    uint32 Text_Timer;
    uint32 ShieldBash_Timer;
    uint32 Revenge_Timer;                                   //this is wrong, spell should never be used unless m_creature->GetVictim() dodge, parry or block attack. Trinity support required.
    uint64 PlayerGUID;

    void Reset() override
    {
        ShieldBash_Timer = 5000;
        Revenge_Timer = 8000;

//        m_creature->SetFactionTemplateId(FACTION_FRIENDLY);
    }

    void UpdateAI(uint32 const diff) override
    {
        if (!pInstance)
            return;

        if (postGossipStep > 0 && postGossipStep < 4)
        {
            if (Text_Timer < diff)
            {
                switch (postGossipStep)
                {
                    case 1:
                        DoScriptText(SAY_1, m_creature);
                        Text_Timer = 5000;
                        break;
                    case 2:
                        DoScriptText(SAY_2, m_creature);
                        Text_Timer = 5000;
                        break;
                    case 3:
                        m_creature->SetFactionTemplateId(FACTION_HOSTILE);
                        if (Player* pTarget = ((Player*)Unit::GetUnit(*m_creature, PlayerGUID)))
                            AttackStart(pTarget);
                        //weegli doesn't fight - he goes & blows up the door
                        if (Creature* weegli = pInstance->instance->GetCreature(pInstance->GetData64(ENTRY_WEEGLI)))
                        {
                            weegli->AI()->OnScriptEventHappened();
                            DoScriptText(SAY_WEEGLI, weegli);
                        }

                        switchFactionIfAlive(pInstance, ENTRY_RAVEN);
                        switchFactionIfAlive(pInstance, ENTRY_ORO);
                        switchFactionIfAlive(pInstance, ENTRY_MURTA);
                }
                postGossipStep++;
            }
            else
                Text_Timer -= diff;
        }

        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;

        if (ShieldBash_Timer <= diff)
        {
            DoCastSpellIfCan(m_creature->GetVictim(), SPELL_SHIELD_BASH);
            ShieldBash_Timer = 15000;
        }
        else
            ShieldBash_Timer -= diff;

        if (Revenge_Timer <= diff)
        {
            DoCastSpellIfCan(m_creature->GetVictim(), SPELL_REVENGE);
            Revenge_Timer = 10000;
        }
        else
            Revenge_Timer -= diff;

        DoMeleeAttackIfReady();
    }

    void OnScriptEventHappened(uint32 /*uiEvent*/ = 0, uint32 /*uiData*/ = 0, WorldObject* /*pInvoker*/ = 0) override
    {
        postGossipStep = 1;
        Text_Timer = 0;
    }

    void switchFactionIfAlive(InstanceData* pInstance, uint32 entry)
    {
        if (Creature* crew = pInstance->instance->GetCreature(pInstance->GetData64(entry)))
            if (crew->IsAlive())
                crew->SetFactionTemplateId(FACTION_HOSTILE);
    }
};

bool OnGossipSelect_npc_sergeant_bly(Player* pPlayer, Creature* pCreature, uint32 uiSender, uint32 uiAction)
{
    if (uiAction == GOSSIP_ACTION_INFO_DEF + 1)
    {
        pPlayer->CLOSE_GOSSIP_MENU();
        if (npc_sergeant_blyAI* ai = dynamic_cast<npc_sergeant_blyAI*>(pCreature->AI()))
        {
            ai->PlayerGUID = pPlayer->GetGUID();
            ai->OnScriptEventHappened();
        }
    }
    return true;
}

bool OnGossipHello_npc_sergeant_bly(Player* pPlayer, Creature* pCreature)
{
    if (InstanceData* pInstance = pCreature->GetInstanceData())
    {
        if (pInstance->GetData(EVENT_PYRAMID) == PYRAMID_KILLED_ALL_TROLLS)
        {
            pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, GOSSIP_BLY, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
            pPlayer->SEND_GOSSIP_MENU(1517, pCreature->GetGUID());
        }
        else if (pInstance->GetData(EVENT_PYRAMID) == PYRAMID_NOT_STARTED)
            pPlayer->SEND_GOSSIP_MENU(1515, pCreature->GetGUID());
        else
            pPlayer->SEND_GOSSIP_MENU(1516, pCreature->GetGUID());
        return true;
    }
    return false;
}

CreatureAI* GetAI_npc_sergeant_bly(Creature* pCreature)
{
    return new npc_sergeant_blyAI(pCreature);
}

/*######
+## go_troll_cage
+######*/

void initBlyCrewMember(InstanceData* pInstance, uint32 entry, float x, float y, float z)
{
    uint64 creaGUID = pInstance->GetData64(entry);

    if (Creature* crew = pInstance->instance->GetCreature(creaGUID))
    {
        //crew->GetCharmInfo()->SetReactState(REACT_AGGRESSIVE);
        crew->SetCombatStartPosition(x, y, z);
        crew->SetHomePosition(x, y, z, 4.7f);
        crew->GetMotionMaster()->MovePoint(1, x, y, z, MOVE_PATHFINDING | MOVE_WALK_MODE);
        crew->SetFactionTemplateId(FACTION_FREED);
    }
}


bool OnGossipHello_go_troll_cage(Player* pPlayer, GameObject* pGo)
{
    if (InstanceData* pInstance = pGo->GetInstanceData())
    {
        // Once, at the start, and never again. This guard is the whole reason the event is
        // survivable.
        //
        // There are five cages and every one of them runs this handler, which used to drive the
        // phase back to PYRAMID_CAGES_OPEN unconditionally and re-issue MovePoint(1) to all five
        // freed NPCs -- Weegli included. A MovePoint to a spot he is already standing on still
        // finalises, and Finalize calls MovementInform, so his handler saw CAGES_OPEN with id 1
        // again and set PYRAMID_ARRIVED_AT_STAIR again. The instance answers that by summoning
        // wave one. Five cages therefore queued up to a hundred and fifteen trolls at the foot of
        // the stairs instead of twenty three, in a pool IsWaveAllDead can never empty, so the
        // release never stopped and the wave never ended. Clicking a cage again mid-fight did it
        // once more.
        //
        // Nothing is lost by refusing the later cages: the first one frees all five of the crew
        // below, because this handler was always written to init the whole group rather than
        // whichever NPC was in the cage that was opened.
        if (pInstance->GetData(EVENT_PYRAMID) != PYRAMID_NOT_STARTED)
            return false;

        pInstance->SetData(EVENT_PYRAMID, PYRAMID_CAGES_OPEN);
        //set bly & co to aggressive & start moving to top of stairs
        initBlyCrewMember(pInstance, ENTRY_BLY, 1887.17f, 1263.72f, 41.484f);
        initBlyCrewMember(pInstance, ENTRY_RAVEN, 1890.76f, 1265.82f, 41.43f);
        initBlyCrewMember(pInstance, ENTRY_ORO, 1883.3f, 1272.53f, 41.87f);
        initBlyCrewMember(pInstance, ENTRY_WEEGLI, 1883.87f, 1263.49f, 41.55f);
        initBlyCrewMember(pInstance, ENTRY_MURTA, 1886.48f, 1272.76f, 41.76f);
    }
    return false;
}

/*######
## npc_weegli_blastfuse
######*/

enum weegliSpells
{
    SPELL_BOMB                 = 8858,
    SPELL_GOBLIN_LAND_MINE     = 21688,
    SPELL_SHOOT                = 6660,
    SPELL_WEEGLIS_BARREL       = 10772
};

enum weegliSays
{
    SAY_WEEGLI_OHNO      = 3744,
    SAY_WEEGLI_OK_I_GO   = 3785,
    SAY_CHIEF_UKORZ_DOOR = 6067
};

#define GOSSIP_WEEGLI               "Will you blow up that door now?"


struct npc_weegli_blastfuseAI : public ScriptedAI
{
    npc_weegli_blastfuseAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        pInstance = (ScriptedInstance*)pCreature->GetInstanceData();
        destroyingDoor = false;
        runAway = false;
        Bomb_Timer = 10000;
        LandMine_Timer = 30000;
        explosiveGUID = 0;
        disappear = false;
        regen = false;
    }

    uint64 explosiveGUID;
    uint32 Bomb_Timer;
    uint32 LandMine_Timer;
    bool destroyingDoor;
    bool runAway;
    bool disappear;
    bool regen;
    ScriptedInstance* pInstance;

    void Reset() override
    {
        /*if (pInstance)
            pInstance->SetData(0, NOT_STARTED);*/
    }

    void AttackStart(Unit *victim) override
    {
        ScriptedAI::AttackStart(victim);
        //AttackStartCaster(victim,10);//keep back & toss bombs/shoot
    }

    void JustDied(Unit * /*victim*/) override
    {
        /*if (pInstance)
            pInstance->SetData(0, DONE);*/
    }

    void UpdateAI(uint32 const diff) override
    {
        if (!regen && pInstance->GetData(EVENT_PYRAMID) == PYRAMID_KILLED_ALL_TROLLS)
        {
            regen = true;

            Creature* pOro = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_ORO));
            if (pOro->IsAlive())
                pOro->SetHealthPercent(100.0f);

            Creature* pMurta = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_MURTA));
            if (pMurta->IsAlive())
                pMurta->SetHealthPercent(100.0f);

            Creature* pBly = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_BLY));
            if (pBly->IsAlive())
                pBly->SetHealthPercent(100.0f);

            Creature* pRaven = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_RAVEN));
            if (pRaven->IsAlive())
                pRaven->SetHealthPercent(100.0f);

            if (m_creature->IsAlive())
                m_creature->SetHealthPercent(100.0f);
        }


        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;


        if (pInstance->GetData(EVENT_PYRAMID) != PYRAMID_KILLED_ALL_TROLLS)
        {
            Creature* pOro = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_ORO));
            if (!pOro->SelectHostileTarget() || !pOro->GetVictim())
                ((CreatureAI*)pOro->AI())->AttackStart(m_creature->GetVictim());

            Creature* pMurta = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_MURTA));
            if (!pMurta->SelectHostileTarget() || !pMurta->GetVictim())
                ((CreatureAI*)pMurta->AI())->AttackStart(m_creature->GetVictim());

            Creature* pBly = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_BLY));
            if (!pBly->SelectHostileTarget() || !pBly->GetVictim())
                ((CreatureAI*)pBly->AI())->AttackStart(m_creature->GetVictim());
        }

        if (Bomb_Timer < diff)
        {
            DoCastSpellIfCan(m_creature->GetVictim(), SPELL_BOMB);
            Bomb_Timer = 10000;
        }
        else
            Bomb_Timer -= diff;

        if (m_creature->IsAttackReady() && !m_creature->CanReachWithMeleeAutoAttack(m_creature->GetVictim()))
        {
            DoCastSpellIfCan(m_creature->GetVictim(), SPELL_SHOOT);
            m_creature->SetSheath(SHEATH_STATE_RANGED);
        }
        else
        {
            m_creature->SetSheath(SHEATH_STATE_MELEE);
            DoMeleeAttackIfReady();
        }
    }

    void MovementInform(uint32 type, uint32 id) override
    {
        if (pInstance)
        {
            if (pInstance->GetData(EVENT_PYRAMID) == PYRAMID_CAGES_OPEN)
            {
                if (id == 1)
                {
                    pInstance->SetData(EVENT_PYRAMID, PYRAMID_ARRIVED_AT_STAIR);
                    DoScriptText(SAY_WEEGLI_OHNO, m_creature);
                    m_creature->SetCombatStartPosition(1882.69f, 1272.28f, 41.87f);
                    m_creature->SetWalk(false);
                    m_creature->GetMotionMaster()->MovePoint(2, 1883.27f, 1268.72f, 41.73f);
                }
            }
            else if (pInstance->GetData(EVENT_PYRAMID) == PYRAMID_WAVE_1 && id == 2)
            {
                m_creature->GetMotionMaster()->MovePoint(3, 1888.55f, 1272.19f, 41.67f);
                m_creature->SetCombatStartPosition(1888.55f, 1272.19f, 41.67f);
                m_creature->SetHomePosition(1888.55f, 1272.19f, 41.67f, 4.7f);
                m_creature->SetWalk(true);
            }
            else
            {
                if (destroyingDoor)
                {
                    GameObject* go = m_creature->SummonGameObject(144065, 1856.314209f, 1144.990479f, 15.486275f, 5.6635f, 0, 0, 0, 0, -1, false);
                    explosiveGUID = go->GetGUID();
                    destroyingDoor = false;
                    RunAfterExplosion1();
                }
                if (runAway && id == 1)
                {
                    if (GameObject* pBoom = m_creature->GetMap()->GetGameObject(explosiveGUID))
                    {
                        pBoom->SetSpellId(13259);
                        pBoom->UseDoorOrButton(explosiveGUID);
                    }
                    uint64 EndDoorGUID = pInstance->GetData64(GO_END_DOOR);
                    pInstance->DoUseDoorOrButton(EndDoorGUID, 0, true);
                    pInstance->SetData(EVENT_END_DOOR, DONE);
                    if (Creature* pChief = m_creature->GetMap()->GetCreature(pInstance->GetData64(ENTRY_UKORZ)))
                        DoScriptText(SAY_CHIEF_UKORZ_DOOR, pChief);
                    RunAfterExplosion2();
                    runAway = false;
                }
                if (disappear && id == 2)
                    m_creature->ForcedDespawn();
            }
        }
    }

    void OnScriptEventHappened(uint32 /*uiEvent*/ = 0, uint32 /*uiData*/ = 0, WorldObject* /*pInvoker*/ = 0) override
    {
        DestroyDoor();
    }

    void DestroyDoor()
    {
        if (m_creature->IsAlive())
        {
            m_creature->SetFactionTemplateId(FACTION_FRIENDLY);
            m_creature->SetWalk(false);
            m_creature->GetMotionMaster()->MovePoint(0, 1858.57f, 1146.35f, 14.745f);
            m_creature->SetCombatStartPosition(1858.57f, 1146.35f, 14.745f); // in case he gets interrupted
            DoScriptText(SAY_WEEGLI_OK_I_GO, m_creature);
            destroyingDoor = true;
        }
    }

    void RunAfterExplosion1()
    {
        if (m_creature->IsAlive())
        {
            m_creature->GetMotionMaster()->MovePoint(1, 1863.77f, 1176.99f, 9.993f);
            m_creature->SetCombatStartPosition(1863.77f, 1176.99f, 9.993f); // in case he gets interrupted

            runAway = true;
        }
    }

    void RunAfterExplosion2()
    {
        if (m_creature->IsAlive())
        {
            m_creature->GetMotionMaster()->MovePoint(2, 1827.1f, 1184.0f, 8.993f);
            m_creature->SetCombatStartPosition(1827.1f, 1184.0f, 8.993f); // in case he gets interrupted
            disappear = true;
        }
    }

};


bool OnGossipSelect_npc_weegli_blastfuse(Player* pPlayer, Creature* pCreature, uint32 uiSender, uint32 uiAction)
{
    if (uiAction == GOSSIP_ACTION_INFO_DEF + 1)
    {
        pPlayer->CLOSE_GOSSIP_MENU();
        //here we make him run to door, set the charge and run away off to nowhere
        pCreature->AI()->OnScriptEventHappened();
    }
    return true;
}

bool OnGossipHello_npc_weegli_blastfuse(Player* pPlayer, Creature* pCreature)
{
    if (InstanceData* pInstance = pCreature->GetInstanceData())
    {
        switch (pInstance->GetData(EVENT_PYRAMID))
        {
            case PYRAMID_KILLED_ALL_TROLLS:
                pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, GOSSIP_WEEGLI, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
                pPlayer->SEND_GOSSIP_MENU(1514, pCreature->GetGUID());  //if event can proceed to end
                break;
            case PYRAMID_NOT_STARTED:
                pPlayer->SEND_GOSSIP_MENU(1511, pCreature->GetGUID());  //if event not started
                break;
            default:
                pPlayer->SEND_GOSSIP_MENU(1513, pCreature->GetGUID());  //if event are in progress
        }
        return true;
    }
    return false;
}

CreatureAI* GetAI_npc_weegli_blastfuse(Creature* pCreature)
{
    return new npc_weegli_blastfuseAI(pCreature);
}

/*######
## go_shallow_grave
######*/

enum
{
    ZOMBIE = 7286,
    DEAD_HERO = 7276,
    ZOMBIE_CHANCE = 65,
    DEAD_HERO_CHANCE = 10,

    SPELL_AWAKEN_ZULFARRAK_ZOMBIE = 10731
};

// Zum'rah's grave raising, which without this does not happen at all.
//
// EventAI 727103 runs on a timer in combat -- one to five seconds in, then every eighteen --
// and casts 10731, "Awaken Zul'Farrak Zombie", at grave entry 128403. The spell is built the way
// it should be: two ACTIVATE_OBJECT effects carrying GameObjectActions::Disturb and ::Despawn, so
// he disturbs a grave to raise what is in it and then despawns that grave so it cannot be raised
// a second time.
//
// It just never does anything. Disturb routes to GameObject::Use, the graves are chests, and the
// chest branch of Use opens with
//
//     if (user->GetTypeId() != TYPEID_PLAYER)
//         return;
//
// Zum'rah is a creature, so the call returns on that line every eighteen seconds for the whole
// fight. The signature mechanic of the boss the graves exist for is dead, and has been.
//
// EffectActivateObject offers the object's AI the activation before it falls through to Use, so
// that is where this goes. One roll per grave, same sixty five and ten as a player looting one,
// which also keeps the level forty five to forty six Dead Hero at the one-in-ten it was written
// to be rather than something guaranteed.
//
// The add is deliberately given no despawn timer. An add that removes itself is one the group can
// back away from and wait out instead of killing, which is not a fight; clearing them when the
// encounter resets is the instance's job. See instance_zulfarrak::Update.
struct shallow_grave_zumrahAI : public GameObjectAI
{
    shallow_grave_zumrahAI(GameObject* pGo) : GameObjectAI(pGo) {}

    bool OnActivateBySpell(SpellCaster* /*pCaster*/, uint32 uiSpellId, uint32 uiAction) override
    {
        if (uiSpellId != SPELL_AWAKEN_ZULFARRAK_ZOMBIE)
            return false;

        // Only the disturb is ours. The second effect despawns the grave, and that is correct
        // behaviour worth keeping -- it is what stops one grave being raised over and over.
        if (uiAction != uint32(GameObjectActions::Disturb))
            return false;

        uint32 const roll = urand(0, 100);
        uint32 uiEntry = 0;

        if (roll < ZOMBIE_CHANCE)
            uiEntry = ZOMBIE;
        else if ((roll - ZOMBIE_CHANCE) < DEAD_HERO_CHANCE)
            uiEntry = DEAD_HERO;

        // A quarter of the time the grave is empty, exactly as it is for a player who loots one.
        if (uiEntry)
            me->SummonCreature(uiEntry, me->GetPositionX(), me->GetPositionY(),
                               me->GetPositionZ(), 0, TEMPSUMMON_DEAD_DESPAWN, 0);

        // Handled, so EffectActivateObject does not go on to fire the linked trap and add the
        // guaranteed pair on top of the one rolled above.
        return true;
    }
};

GameObjectAI* GetAI_go_shallow_grave(GameObject* pGo)
{
    return new shallow_grave_zumrahAI(pGo);
}

bool OnGossipHello_go_shallow_grave(Player* pPlayer, GameObject* pGo)
{
    // randomly summon a zombie or dead hero the first time a grave is used
    if (pGo->GetUseCount() == 0)
    {
        uint32 randomchance = urand(0, 100);
        if (randomchance < ZOMBIE_CHANCE)
            pGo->SummonCreature(ZOMBIE, pGo->GetPositionX(), pGo->GetPositionY(), pGo->GetPositionZ(), 0, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 30000);
        else if ((randomchance - ZOMBIE_CHANCE) < DEAD_HERO_CHANCE)
            pGo->SummonCreature(DEAD_HERO, pGo->GetPositionX(), pGo->GetPositionY(), pGo->GetPositionZ(), 0, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 30000);
    }
    pGo->AddUse();
    return true;
}

/*######
## go_table_theka
######*/

bool OnGossipHello_go_table_theka(Player* pPlayer, GameObject* pGo)
{
    if (pPlayer->GetQuestStatus(2936) == QUEST_STATUS_INCOMPLETE)
        pPlayer->AreaExploredOrEventHappens(2936);

    pPlayer->SEND_GOSSIP_MENU(1653, pGo->GetGUID());

    return true;
}

/*######
## ward_zumrah
######*/

enum
{
    NPC_ZUMRAH_OWNER        = 7271,
    NPC_WARD_OF_ZUMRAH      = 7785,
    NPC_SKELETON_OF_ZUMRAH  = 7786,

    SPELL_SUMMON_SKELETON   = 11088,

    // Unchanged. Five seconds is a ScriptDev2-era number rather than anything read out of game
    // data, so there is nothing to restore it to; it is left alone because the measured problem
    // was the number of spawners, not the rate of any one of them.
    //
    // Zum'rah raises the Ward with spell 11086, SPELL_EFFECT_SUMMON_TOTEM, which resolves to
    // TOTEM_SLOT_NONE. Spell::EffectSummonTotem only unsummons the standing totem when the slot
    // is below MAX_TOTEM_SLOT, and 255 is not, so nothing ever replaced the previous Ward.
    //
    // Bounded, though, and worth being exact about rather than calling it runaway growth: both
    // summons carry durationIndex 18, which is the twenty second row that Magma Totem, Scorpid
    // Sting and Sweeping Strikes share. A Ward therefore expires on its own after twenty
    // seconds while the spell list re-casts it every fifteen to thirty two, so the overlap is
    // one extra spawner for a few seconds at a time rather than one more per minute. That still
    // doubles the spawn rate whenever it happens, and removing it costs nothing, which is the
    // whole case for the rule below.
    WARD_SKELETON_INTERVAL  = 5000,

    // A failed cast used to leave the timer below the tick length, so it retried on every
    // single update until it succeeded. Backing off by a second instead keeps one blocked cast
    // from becoming a burst the moment whatever blocked it clears.
    WARD_SKELETON_RETRY     = 1000,
};

// Far enough to cover the whole pit, since each Ward is summoned wherever Zum'rah happens to be
// standing rather than at a fixed point.
static float const WARD_CLEANUP_RADIUS = 100.0f;

struct ward_zumrahAI : public ScriptedAI
{
    ward_zumrahAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        Reset();
    }

    uint32 m_uiSkeletonTimer;
    bool m_bDisplacedOlderWards;

    void Reset() override
    {
        m_uiSkeletonTimer = WARD_SKELETON_INTERVAL;
        m_bDisplacedOlderWards = false;
        m_creature->SetDefaultMovementType(IDLE_MOTION_TYPE);
    }

    void DespawnSkeletons() const
    {
        std::list<Creature*> skeletons;
        m_creature->GetCreatureListWithEntryInGrid(skeletons, NPC_SKELETON_OF_ZUMRAH,
                                                  WARD_CLEANUP_RADIUS);
        for (Creature* pSkeleton : skeletons)
            pSkeleton->DisappearAndDie();
    }

    void UpdateAI(uint32 const uiDiff) override
    {
        // One spawner at a time. Done here rather than in Reset because Reset runs from the
        // constructor, before this Ward is in the world and able to see the others.
        if (!m_bDisplacedOlderWards)
        {
            m_bDisplacedOlderWards = true;

            std::list<Creature*> wards;
            m_creature->GetCreatureListWithEntryInGrid(wards, NPC_WARD_OF_ZUMRAH,
                                                       WARD_CLEANUP_RADIUS);
            for (Creature* pWard : wards)
                if (pWard != m_creature)
                    pWard->DisappearAndDie();
        }

        // The fight has ended, by kill or by wipe. Nothing in this encounter is tied to Zum'rah's
        // own reset: the Ward is a totem in no slot and the skeletons are guardians of the Ward
        // rather than of him, so his evade does not touch either.
        //
        // They do clear themselves eventually, on the twenty second duration, so this is about
        // how long "eventually" is. A Ward that was raised a moment before the wipe keeps
        // spawning for its remaining life, and the last skeleton it raises then lives twenty
        // seconds beyond that -- so the pit can still be occupied for the better part of a
        // minute, which is exactly the window a group spends running back from the graveyard.
        // Ending it on the reset is what makes the second attempt the same fight as the first.
        Creature* pZumrah = m_creature->FindNearestCreature(NPC_ZUMRAH_OWNER, 200.0f, false);
        if (!pZumrah || !pZumrah->IsAlive() || !pZumrah->IsInCombat())
        {
            DespawnSkeletons();
            m_creature->DisappearAndDie();
            return;
        }

        if (m_uiSkeletonTimer <= uiDiff)
        {
            if (DoCastSpellIfCan(m_creature, SPELL_SUMMON_SKELETON, true) == CAST_OK)
                m_uiSkeletonTimer = WARD_SKELETON_INTERVAL;
            else
                m_uiSkeletonTimer = WARD_SKELETON_RETRY;
        }
        else
            m_uiSkeletonTimer -= uiDiff;
    }
};

CreatureAI* GetAI_ward_zumrah(Creature* pCreature)
{
    return new ward_zumrahAI(pCreature);
}

/*######
## at_zumrah
######*/

enum
{
    NPC_WITCH_DOCTOR_ZUMRAH = 7271,
    ZUMRAH_HOSTILE_FACTION  = 37,

    SAY_ZUMRAH_TRIGGER      = 3622,
    SAY_ZUMRAH_YELL         = 6221,
    SAY_ZUMRAH_KILLED       = 6222
};

bool OnTrigger_at_zumrah(Player* pPlayer, AreaTriggerEntry const *at)
{
    Creature* pZumrah = pPlayer->FindNearestCreature(NPC_WITCH_DOCTOR_ZUMRAH, 30.0f);

    if (!pZumrah || !pZumrah->IsAlive())
        return false;

    if (pZumrah->GetFactionTemplateId() != ZUMRAH_HOSTILE_FACTION)
    {
        if (InstanceData* pInstance = pZumrah->GetInstanceData())
            pInstance->SetData(EVENT_ZUMRAH, IN_PROGRESS);

        pZumrah->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER);
        pZumrah->SetFactionTemplateId(ZUMRAH_HOSTILE_FACTION);
        DoScriptText(SAY_ZUMRAH_TRIGGER, pZumrah);
    }

    return true;
}

/*######
## at_antusul
######*/

enum
{
    NPC_ANTUSUL            = 8127,
    NPS_SULITHUZ_BROODLING = 8138,
    SAY_ANTUSUL_TRIGGER    = 4166,
};

bool OnTrigger_at_antusul(Player* pPlayer, AreaTriggerEntry const *at)
{
    // A corpse does not pull a boss. Running back through this room as a ghost re-armed the whole
    // event -- four broodlings, all of them SetInCombatWithZone -- waiting for the player to
    // resurrect into it. The same goes for anyone the fight has already killed while it runs.
    if (!pPlayer->IsAlive())
        return false;

    Creature* pAntusul = pPlayer->FindNearestCreature(NPC_ANTUSUL, 100.0f);

    if (!pAntusul || !pAntusul->IsAlive() || pAntusul->IsInCombat())
        return false;

    InstanceData* pInstance = pAntusul->GetInstanceData();
    if (!pInstance || pInstance->GetData(EVENT_ANTUSUL) != NOT_STARTED)
        return false;
    
    pInstance->SetData(EVENT_ANTUSUL, IN_PROGRESS);
    DoScriptText(SAY_ANTUSUL_TRIGGER, pAntusul);
    pAntusul->m_Events.AddLambdaEventAtOffset([pAntusul]()
    {
        // World of Warcraft Client Patch 1.12.0 (2006-08-22)
        // - Antu'sul's Sul'lithuz Broodlings now only hatch 4 at a time and are
        //   significantly weaker.
        uint32 count = sWorld.GetWowPatch() < WOW_PATCH_112 ? 2 : 1;
        for (uint32 i = 0; i < count; ++i)
        {
            if (Creature* pBroodling = pAntusul->SummonCreature(NPS_SULITHUZ_BROODLING, 1823.415161f, 748.297485f, 20.794931f, 3.944444f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN))
                pBroodling->SetInCombatWithZone(true);
            if (Creature* pBroodling = pAntusul->SummonCreature(NPS_SULITHUZ_BROODLING, 1786.019165f, 743.399048f, 15.481779f, 6.108652f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN))
                pBroodling->SetInCombatWithZone(true);
            if (Creature* pBroodling = pAntusul->SummonCreature(NPS_SULITHUZ_BROODLING, 1827.460571f, 738.032410f, 19.131363f, 3.385939f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN))
                pBroodling->SetInCombatWithZone(true);
            if (Creature* pBroodling = pAntusul->SummonCreature(NPS_SULITHUZ_BROODLING, 1810.196533f, 749.873230f, 17.597878f, 4.555309f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN))
                pBroodling->SetInCombatWithZone(true);
        }
        
        if (pAntusul->IsAlive() && !pAntusul->IsInCombat())
            pAntusul->GetMotionMaster()->MovePoint(0, 1805.133667f, 740.349304f, 14.763382f, MOVE_PATHFINDING | MOVE_RUN_MODE);

    }, BATCHING_INTERVAL * 3);

    return true;
}

void AddSC_zulfarrak()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "npc_sergeant_bly";
    newscript->GetAI = &GetAI_npc_sergeant_bly;
    newscript->pGossipHello = &OnGossipHello_npc_sergeant_bly;
    newscript->pGossipSelect = &OnGossipSelect_npc_sergeant_bly;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_weegli_blastfuse";
    newscript->GetAI = &GetAI_npc_weegli_blastfuse;
    newscript->pGossipHello = &OnGossipHello_npc_weegli_blastfuse;
    newscript->pGossipSelect = &OnGossipSelect_npc_weegli_blastfuse;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "go_shallow_grave";
    newscript->pGOOpen = &OnGossipHello_go_shallow_grave;
    newscript->GOGetAI = &GetAI_go_shallow_grave;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "go_troll_cage";
    newscript->pGOHello = &OnGossipHello_go_troll_cage;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "go_table_theka";
    newscript->pGOHello = &OnGossipHello_go_table_theka;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "ward_zumrah";
    newscript->GetAI = &GetAI_ward_zumrah;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "at_zumrah";
    newscript->pAreaTrigger = &OnTrigger_at_zumrah;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "at_antusul";
    newscript->pAreaTrigger = &OnTrigger_at_antusul;
    newscript->RegisterSelf();
}
