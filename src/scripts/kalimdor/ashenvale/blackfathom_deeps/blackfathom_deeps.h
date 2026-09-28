/* Copyright (C) 2006 - 2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 * This program is free software licensed under GPL version 2
 * Please see the included DOCS/LICENSE.TXT for more information */

#ifndef DEF_BFD_H
#define DEF_BFD_H

enum
{
    DATA_SHRINE1                = 1,
    DATA_SHRINE2                = 2,
    DATA_SHRINE3                = 3,
    DATA_SHRINE4                = 4,
    DATA_TWILIGHT_LORD_KELRIS   = 5,
    DATA_SHRINE_OF_GELIHAST     = 6,
    DATA_ALTAR_OF_THE_DEEPS     = 7,
    DATA_MAINDOOR               = 8,

    TYPE_KELRIS                 = 10,
    TYPE_SHRINE                 = 11,
    TYPE_AQUANIS                = 12,
    TYPE_AKUMAI                 = 13,

    NPC_BARON_AQUANIS           = 12876,
    NPC_AKUMAI                  = 4829,
    GO_FATHOM_STONE             = 177964,

    // Shrine event
    NPC_AKUMAI_SERVANT          = 4978,
    NPC_AKUMAI_SNAPJAW          = 4825,
    NPC_MURKSHALLOW_SNAPCLAW    = 4815,
    NPC_MURKSHALLOW_SOFTSHELL   = 4977,

    GO_PORTAL_DOOR              = 21117,
    GO_SHRINE_1                 = 21118,
    GO_SHRINE_2                 = 21119,
    GO_SHRINE_3                 = 21120,
    GO_SHRINE_4                 = 21121,

    // The two clickable shrine/altar buttons. Each drives a linked trap that carries the
    // actual spell: Gelihast's grants Blessing of Blackfathom, Aku'mai's teleports the
    // group back to the instance entrance.
    GO_SHRINE_OF_GELIHAST       = 103015,
    GO_ALTAR_OF_THE_DEEPS       = 103016,

    BFD_ENCOUNTER_KELRIS        = 0,
    BFD_ENCOUNTER_SHRINE        = 1,
    BFD_ENCOUNTER_AQUANIS       = 2,
    BFD_ENCOUNTER_AKUMAI        = 3,
    INSTANCE_BFD_MAX_ENCOUNTER  = 4,
};
#endif
