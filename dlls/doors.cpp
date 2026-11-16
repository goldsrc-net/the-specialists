/***
*
*	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
/*

===== doors.cpp ========================================================

*/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "doors.h"
#include "func_break.h" // TS breakable-door reuse: Materials + MaterialSound{Precache,Random}
#include "ts_func_break.h" // CInfoBreakable — the shared respawn retry-helper (doors ride it too)

extern int gmsgTSCreateDebris; // func_break.cpp — the TS glass-shatter sprite shower


extern void SetMovedir(entvars_t* ev);

#define noiseMoving noise1
#define noiseArrived noise2

class CBaseDoor : public CBaseToggle
{
public:
	void Spawn() override;
	void Precache() override;
	bool KeyValue(KeyValueData* pkvd) override;
	void Use(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value) override;
	void Blocked(CBaseEntity* pOther) override;


	int ObjectCaps() override
	{
		if ((pev->spawnflags & SF_ITEM_USE_ONLY) != 0)
			return (CBaseToggle::ObjectCaps() & ~FCAP_ACROSS_TRANSITION) | FCAP_IMPULSE_USE;
		else
			return (CBaseToggle::ObjectCaps() & ~FCAP_ACROSS_TRANSITION);
	};
	bool Save(CSave& save) override;
	bool Restore(CRestore& restore) override;

	static TYPEDESCRIPTION m_SaveData[];

	void SetToggleState(int state) override;

	// used to selectivly override defaults
	void EXPORT DoorTouch(CBaseEntity* pOther);

	// local functions
	bool DoorActivate();
	void EXPORT DoorGoUp();
	void EXPORT DoorGoDown();
	void EXPORT DoorHitTop();
	void EXPORT DoorHitBottom();

	byte m_bHealthValue; // some doors are medi-kit doors, they give players health

	byte m_bMoveSnd; // sound a door makes while moving
	byte m_bStopSnd; // sound a door makes when it stops

	locksound_t m_ls; // door lock sounds

	byte m_bLockedSound; // ordinals from entity selection
	byte m_bLockedSentence;
	byte m_bUnlockedSound;
	byte m_bUnlockedSentence;

	// TS breakable-door system (@0xb57ac TraceAttack / @0xb5604 BreakableRespawn). A door with a
	// "maxdamage" keyvalue takes damage and shatters like a func_breakable, then respawns.
	void TraceAttack(entvars_t* pevAttacker, float flDamage, Vector vecDir, TraceResult* ptr, int bitsDamageType) override;
	void BreakableRespawn(bool forceRespawn) override; // @0xb5604 — box-clear retry + restore

	EHANDLE m_hRespawner; // +0x104 — the info_breakable retry-helper (created in Spawn @0xb60cc
	                      // tail; its RespawnThink drives the respawn — the door has NO think of
	                      // its own for this, exactly like CBreakable)

	float m_flBreakHealth = 0;   // +0x118 — "maxdamage": damage to shatter (<=0 = unbreakable)
	int   m_iAccumDamage = 0;    // +0x110 — accumulated damage toward the threshold
	int   m_DoorMaterial = 1;    // +0x11c — "doormaterial" (Materials; default 1 = matWood-ish)
	float m_flRespawnTime = 0;   // +0x10c — "respawntime". DEFAULT 0 (gcc2 entity data is calloc'd:
	                             // an absent keyvalue = 0 = the 30s default — breakable doors
	                             // respawn unless the map sets it NEGATIVE)
	Vector m_vecSpawnOrigin;     // +0x124 — saved origin (restored on respawn)
	Vector m_vecSpawnAngles;     // +0x130 — saved angles
	float m_flDoorNextAttack = 0; // +0x114 — per-door damage cooldown
};


TYPEDESCRIPTION CBaseDoor::m_SaveData[] =
	{
		DEFINE_FIELD(CBaseDoor, m_bHealthValue, FIELD_CHARACTER),
		DEFINE_FIELD(CBaseDoor, m_bMoveSnd, FIELD_CHARACTER),
		DEFINE_FIELD(CBaseDoor, m_bStopSnd, FIELD_CHARACTER),

		DEFINE_FIELD(CBaseDoor, m_bLockedSound, FIELD_CHARACTER),
		DEFINE_FIELD(CBaseDoor, m_bLockedSentence, FIELD_CHARACTER),
		DEFINE_FIELD(CBaseDoor, m_bUnlockedSound, FIELD_CHARACTER),
		DEFINE_FIELD(CBaseDoor, m_bUnlockedSentence, FIELD_CHARACTER),

};

IMPLEMENT_SAVERESTORE(CBaseDoor, CBaseToggle);


#define DOOR_SENTENCEWAIT 6
#define DOOR_SOUNDWAIT 3
#define BUTTON_SOUNDWAIT 0.5

// play door or button locked or unlocked sounds.
// pass in pointer to valid locksound struct.
// if flocked is true, play 'door is locked' sound,
// otherwise play 'door is unlocked' sound
// NOTE: this routine is shared by doors and buttons

void PlayLockSounds(entvars_t* pev, locksound_t* pls, bool flocked, bool fbutton)
{
	// LOCKED SOUND

	// CONSIDER: consolidate the locksound_t struct (all entries are duplicates for lock/unlock)
	// CONSIDER: and condense this code.
	float flsoundwait;

	if (fbutton)
		flsoundwait = BUTTON_SOUNDWAIT;
	else
		flsoundwait = DOOR_SOUNDWAIT;

	if (flocked)
	{
		bool fplaysound = (!FStringNull(pls->sLockedSound) && gpGlobals->time > pls->flwaitSound);
		bool fplaysentence = (!FStringNull(pls->sLockedSentence) && 0 == pls->bEOFLocked && gpGlobals->time > pls->flwaitSentence);
		float fvol;

		if (fplaysound && fplaysentence)
			fvol = 0.25;
		else
			fvol = 1.0;

		// if there is a locked sound, and we've debounced, play sound
		if (fplaysound)
		{
			// play 'door locked' sound
			EMIT_SOUND(ENT(pev), CHAN_ITEM, (char*)STRING(pls->sLockedSound), fvol, ATTN_NORM);
			pls->flwaitSound = gpGlobals->time + flsoundwait;
		}

		// if there is a sentence, we've not played all in list, and we've debounced, play sound
		if (fplaysentence)
		{
			// play next 'door locked' sentence in group
			int iprev = pls->iLockedSentence;

			pls->iLockedSentence = SENTENCEG_PlaySequentialSz(ENT(pev), STRING(pls->sLockedSentence),
				0.85, ATTN_NORM, 0, 100, pls->iLockedSentence, false);
			pls->iUnlockedSentence = 0;

			// make sure we don't keep calling last sentence in list
			pls->bEOFLocked = static_cast<int>(iprev == pls->iLockedSentence);

			pls->flwaitSentence = gpGlobals->time + DOOR_SENTENCEWAIT;
		}
	}
	else
	{
		// UNLOCKED SOUND

		bool fplaysound = (!FStringNull(pls->sUnlockedSound) && gpGlobals->time > pls->flwaitSound);
		bool fplaysentence = (!FStringNull(pls->sUnlockedSentence) && 0 == pls->bEOFUnlocked && gpGlobals->time > pls->flwaitSentence);
		float fvol;

		// if playing both sentence and sound, lower sound volume so we hear sentence
		if (fplaysound && fplaysentence)
			fvol = 0.25;
		else
			fvol = 1.0;

		// play 'door unlocked' sound if set
		if (fplaysound)
		{
			EMIT_SOUND(ENT(pev), CHAN_ITEM, (char*)STRING(pls->sUnlockedSound), fvol, ATTN_NORM);
			pls->flwaitSound = gpGlobals->time + flsoundwait;
		}

		// play next 'door unlocked' sentence in group
		if (fplaysentence)
		{
			int iprev = pls->iUnlockedSentence;

			pls->iUnlockedSentence = SENTENCEG_PlaySequentialSz(ENT(pev), STRING(pls->sUnlockedSentence),
				0.85, ATTN_NORM, 0, 100, pls->iUnlockedSentence, false);
			pls->iLockedSentence = 0;

			// make sure we don't keep calling last sentence in list
			pls->bEOFUnlocked = static_cast<int>(iprev == pls->iUnlockedSentence);
			pls->flwaitSentence = gpGlobals->time + DOOR_SENTENCEWAIT;
		}
	}
}

//
// Cache user-entity-field values until spawn is called.
//

bool CBaseDoor::KeyValue(KeyValueData* pkvd)
{

	if (FStrEq(pkvd->szKeyName, "skin")) //skin is used for content type
	{
		pev->skin = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "movesnd"))
	{
		m_bMoveSnd = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "stopsnd"))
	{
		m_bStopSnd = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "healthvalue"))
	{
		m_bHealthValue = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "locked_sound"))
	{
		m_bLockedSound = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "locked_sentence"))
	{
		m_bLockedSentence = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "unlocked_sound"))
	{
		m_bUnlockedSound = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "unlocked_sentence"))
	{
		m_bUnlockedSentence = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "WaveHeight"))
	{
		pev->scale = atof(pkvd->szValue) * (1.0 / 8.0);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "maxdamage")) // TS breakable door: damage to shatter
	{
		m_flBreakHealth = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "doormaterial")) // TS breakable door: material (debris/sound)
	{
		m_DoorMaterial = atoi(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "respawntime")) // TS breakable door: respawn delay (<0 = never)
	{
		m_flRespawnTime = atof(pkvd->szValue);
		return true;
	}

	return CBaseToggle::KeyValue(pkvd);
}

/*QUAKED func_door (0 .5 .8) ? START_OPEN x DOOR_DONT_LINK TOGGLE
if two doors touch, they are assumed to be connected and operate as a unit.

TOGGLE causes the door to wait in both the start and end states for a trigger event.

START_OPEN causes the door to move to its destination when spawned, and operate in reverse.
It is used to temporarily or permanently close off an area when triggered (not usefull for
touch or takedamage doors).

"angle"         determines the opening direction
"targetname"	if set, no touch field will be spawned and a remote button or trigger
				field activates the door.
"health"        if set, door must be shot open
"speed"         movement speed (100 default)
"wait"          wait before returning (3 default, -1 = never return)
"lip"           lip remaining at end of move (8 default)
"dmg"           damage to inflict when blocked (2 default)
"sounds"
0)      no sound
1)      stone
2)      base
3)      stone chain
4)      screechy metal
*/

LINK_ENTITY_TO_CLASS(func_door, CBaseDoor);
//
// func_water - same as a door.
//
LINK_ENTITY_TO_CLASS(func_water, CBaseDoor);


void CBaseDoor::Spawn()
{
	Precache();
	SetMovedir(pev);

	if (pev->skin == 0)
	{ //normal door
		if (FBitSet(pev->spawnflags, SF_DOOR_PASSABLE))
			pev->solid = SOLID_NOT;
		else
			pev->solid = SOLID_BSP;
	}
	else
	{ // special contents
		pev->solid = SOLID_NOT;
		SetBits(pev->spawnflags, SF_DOOR_SILENT); // water is silent for now
	}

	pev->movetype = MOVETYPE_PUSH;
	UTIL_SetOrigin(pev, pev->origin);
	SET_MODEL(ENT(pev), STRING(pev->model));

	if (pev->speed == 0)
		pev->speed = 100;

	m_vecPosition1 = pev->origin;
	// Subtract 2 from size because the engine expands bboxes by 1 in all directions making the size too big
	m_vecPosition2 = m_vecPosition1 + (pev->movedir * (fabs(pev->movedir.x * (pev->size.x - 2)) + fabs(pev->movedir.y * (pev->size.y - 2)) + fabs(pev->movedir.z * (pev->size.z - 2)) - m_flLip));
	ASSERTSZ(m_vecPosition1 != m_vecPosition2, "door start/end positions are equal");
	if (FBitSet(pev->spawnflags, SF_DOOR_START_OPEN))
	{ // swap pos1 and pos2, put door at pos2
		UTIL_SetOrigin(pev, m_vecPosition2);
		m_vecPosition2 = m_vecPosition1;
		m_vecPosition1 = pev->origin;
	}

	m_toggle_state = TS_AT_BOTTOM;

	// if the door is flagged for USE button activation only, use NULL touch function
	if (FBitSet(pev->spawnflags, SF_DOOR_USE_ONLY))
	{
		SetTouch(NULL);
	}
	else // touchable button
		SetTouch(&CBaseDoor::DoorTouch);

	// TS breakable-door init (@0xa60cc): save the spawn pose for respawn, reset the damage tally,
	// and make the door damageable only if it carries a "maxdamage" threshold.
	m_vecSpawnOrigin = pev->origin;
	m_vecSpawnAngles = pev->angles;
	m_iAccumDamage = 0;
	m_flDoorNextAttack = 0;
	pev->takedamage = (m_flBreakHealth > 0) ? DAMAGE_YES : DAMAGE_NO;

	// @0xb60cc tail: a respawn-capable door creates its info_breakable retry-helper once
	// (cross-linked by EHANDLE both ways), exactly like CBreakable::Spawn.
	if (m_flRespawnTime >= 0)
	{
		if (!(CBaseEntity*)m_hRespawner)
			m_hRespawner = CBaseEntity::Create("info_breakable", VecBModelOrigin(pev), pev->angles, edict());
		if ((CBaseEntity*)m_hRespawner)
			((CInfoBreakable*)(CBaseEntity*)m_hRespawner)->m_hBreakable = this;
	}
}

// @0xb57ac TraceAttack — a breakable door accumulates damage and shatters (debris + material sound)
// when it passes its maxdamage threshold, then schedules a respawn. // UNPLAYTESTED.
void CBaseDoor::TraceAttack(entvars_t* pevAttacker, float flDamage, Vector vecDir, TraceResult* ptr, int bitsDamageType)
{
	if (gpGlobals->time < m_flDoorNextAttack)
		return;
	if (pev->takedamage == DAMAGE_NO)
		return;
	// only blast (0x40), bit 25 (0x2000000) and TS close combat (0x4000000) hurt a door; a TS
	// bullet is DMG_BULLET | DMG_NEVERGIB (0x1002) and never does
	if ((bitsDamageType & 0x6000040) == 0)
		return;
	if (FClassnameIs(pev, "func_water"))
		return;

	float dmg = flDamage;
	if (bitsDamageType & DMG_BLAST) // @0xb5844 testl $0x40: blast does double
		dmg *= 2;
	m_iAccumDamage += (int)dmg;
	if ((float)m_iAccumDamage < m_flBreakHealth)
		return; // not enough yet

	// shatter — emit debris flying in the shot (bullet-travel) direction.
	Vector vecShatterDir = vecDir.Normalize();
	Vector vecSpot = pev->origin + (pev->mins + pev->maxs) * 0.5f;
	float vol = RANDOM_FLOAT(0.85f, 1.0f);
	CBreakable::MaterialSoundRandom(ENT(pev), (Materials)m_DoorMaterial, vol);

	if (m_DoorMaterial == matGlass && gmsgTSCreateDebris != 0)
	{
		Vector vecShatter = vecShatterDir; // bullet travel dir (away from the shooter)
		MESSAGE_BEGIN(MSG_PVS, gmsgTSCreateDebris, vecSpot);
		WRITE_COORD(vecSpot.x); WRITE_COORD(vecSpot.y); WRITE_COORD(vecSpot.z);
		WRITE_COORD(pev->size.x); WRITE_COORD(pev->size.y); WRITE_COORD(pev->size.z);
		WRITE_BYTE(0); // material 0 = glass
		// @0x752e0 wire order (see func_break Die): triple 1 = impact POINT, triple 2 = DIRECTION; the LONG
		// is the killing hit's bitsDamageType (selects the client velocity branch); velMag = damage (×8 for
		// the shotgun bit 0x2000000). The client resolves the glassfrag sprite by name.
		WRITE_COORD(ptr->vecEndPos.x); WRITE_COORD(ptr->vecEndPos.y); WRITE_COORD(ptr->vecEndPos.z); // triple 1 = impact point
		WRITE_COORD(vecShatter.x); WRITE_COORD(vecShatter.y); WRITE_COORD(vecShatter.z);             // triple 2 = direction
		WRITE_LONG(bitsDamageType);
		WRITE_COORD((bitsDamageType & 0x2000000) ? flDamage * 8.0f : flDamage);
		MESSAGE_END();
	}
	else
	{
		MESSAGE_BEGIN(MSG_PVS, SVC_TEMPENTITY, vecSpot);
		WRITE_BYTE(TE_BREAKMODEL);
		WRITE_COORD(vecSpot.x); WRITE_COORD(vecSpot.y); WRITE_COORD(vecSpot.z);
		WRITE_COORD(pev->size.x); WRITE_COORD(pev->size.y); WRITE_COORD(pev->size.z);
		WRITE_COORD(0); WRITE_COORD(0); WRITE_COORD(25); // velocity
		WRITE_BYTE(10);                                  // randomization
		WRITE_SHORT(MODEL_INDEX((char*)STRING(pev->model))); // gib the door's own model
		WRITE_BYTE(0);  // shard count (client decides)
		WRITE_BYTE(25); // duration (2.5s)
		WRITE_BYTE(0x19); // flags
		MESSAGE_END();
	}

	// kill the door (@0xb57ac tail: exact stores — effects = EF_NODRAW, no SUB_UseTargets;
	// the original fires NO targets on shatter) and arm the retry-HELPER's think (the door
	// itself never thinks for respawn).
	pev->solid = SOLID_NOT;
	pev->effects = EF_NODRAW;
	pev->takedamage = DAMAGE_NO;
	pev->deadflag = DEAD_DEAD;
	SetTouch(NULL);
	SetThink(NULL);

	if (m_flRespawnTime >= 0 && (CBaseEntity*)m_hRespawner)
	{
		CInfoBreakable* pHelper = (CInfoBreakable*)(CBaseEntity*)m_hRespawner;
		pHelper->SetThink(&CInfoBreakable::RespawnThink);
		pHelper->pev->nextthink = gpGlobals->time + (m_flRespawnTime != 0 ? m_flRespawnTime : 30.0f);
	}
}

// @0xb5604 BreakableRespawn — if the door's box is clear, restore it to the spawn pose and re-Spawn;
// else retry in 5s. // UNPLAYTESTED.
void CBaseDoor::BreakableRespawn(bool forceRespawn)
{
	// @0xb5604 gate: proceed if dead OR exactly hidden (effects == EF_NODRAW) OR forced.
	if (!(pev->deadflag == DEAD_DEAD || pev->effects == EF_NODRAW || forceRespawn))
		return;

	CBaseEntity* pList[1];
	int count = UTIL_EntitiesInBox(pList, 1, pev->absmin, pev->absmax, FL_CLIENT | FL_MONSTER);
	if (count == 0)
	{
		pev->deadflag = DEAD_NO;
		pev->effects = 0; // exact store (the original zeroes ALL effects)
		pev->origin = m_vecSpawnOrigin;
		UTIL_SetOrigin(pev, pev->origin);
		pev->angles = m_vecSpawnAngles;
		pev->avelocity = g_vecZero;
		pev->velocity = g_vecZero;
		SetTouch(NULL);
		SetThink(NULL);
		Spawn();
	}
	else if ((CBaseEntity*)m_hRespawner)
	{
		// blocked — re-arm the HELPER to retry in 5s (the original never thinks on the door)
		CInfoBreakable* pHelper = (CInfoBreakable*)(CBaseEntity*)m_hRespawner;
		pHelper->SetThink(&CInfoBreakable::RespawnThink);
		pHelper->pev->nextthink = gpGlobals->time + 5.0f;
	}
}


void CBaseDoor::SetToggleState(int state)
{
	if (state == TS_AT_TOP)
		UTIL_SetOrigin(pev, m_vecPosition2);
	else
		UTIL_SetOrigin(pev, m_vecPosition1);
}


void CBaseDoor::Precache()
{
	const char* pszSound;

	// set the door's "in-motion" sound
	switch (m_bMoveSnd)
	{
	case 0:
		pev->noiseMoving = ALLOC_STRING("common/null.wav");
		break;
	case 1:
		PRECACHE_SOUND("doors/doormove1.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove1.wav");
		break;
	case 2:
		PRECACHE_SOUND("doors/doormove2.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove2.wav");
		break;
	case 3:
		PRECACHE_SOUND("doors/doormove3.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove3.wav");
		break;
	case 4:
		PRECACHE_SOUND("doors/doormove4.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove4.wav");
		break;
	case 5:
		PRECACHE_SOUND("doors/doormove5.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove5.wav");
		break;
	case 6:
		PRECACHE_SOUND("doors/doormove6.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove6.wav");
		break;
	case 7:
		PRECACHE_SOUND("doors/doormove7.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove7.wav");
		break;
	case 8:
		PRECACHE_SOUND("doors/doormove8.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove8.wav");
		break;
	case 9:
		PRECACHE_SOUND("doors/doormove9.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove9.wav");
		break;
	case 10:
		PRECACHE_SOUND("doors/doormove10.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove10.wav");
		break;
	default:
		pev->noiseMoving = ALLOC_STRING("common/null.wav");
		break;
	}

	// set the door's 'reached destination' stop sound
	switch (m_bStopSnd)
	{
	case 0:
		pev->noiseArrived = ALLOC_STRING("common/null.wav");
		break;
	case 1:
		PRECACHE_SOUND("doors/doorstop1.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop1.wav");
		break;
	case 2:
		PRECACHE_SOUND("doors/doorstop2.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop2.wav");
		break;
	case 3:
		PRECACHE_SOUND("doors/doorstop3.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop3.wav");
		break;
	case 4:
		PRECACHE_SOUND("doors/doorstop4.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop4.wav");
		break;
	case 5:
		PRECACHE_SOUND("doors/doorstop5.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop5.wav");
		break;
	case 6:
		PRECACHE_SOUND("doors/doorstop6.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop6.wav");
		break;
	case 7:
		PRECACHE_SOUND("doors/doorstop7.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop7.wav");
		break;
	case 8:
		PRECACHE_SOUND("doors/doorstop8.wav");
		pev->noiseArrived = ALLOC_STRING("doors/doorstop8.wav");
		break;
	default:
		pev->noiseArrived = ALLOC_STRING("common/null.wav");
		break;
	}

	// get door button sounds, for doors which are directly 'touched' to open

	if (0 != m_bLockedSound)
	{
		pszSound = ButtonSound((int)m_bLockedSound);
		PRECACHE_SOUND(pszSound);
		m_ls.sLockedSound = ALLOC_STRING(pszSound);
	}

	if (0 != m_bUnlockedSound)
	{
		pszSound = ButtonSound((int)m_bUnlockedSound);
		PRECACHE_SOUND(pszSound);
		m_ls.sUnlockedSound = ALLOC_STRING(pszSound);
	}

	// get sentence group names, for doors which are directly 'touched' to open

	switch (m_bLockedSentence)
	{
	case 1:
		m_ls.sLockedSentence = ALLOC_STRING("NA");
		break; // access denied
	case 2:
		m_ls.sLockedSentence = ALLOC_STRING("ND");
		break; // security lockout
	case 3:
		m_ls.sLockedSentence = ALLOC_STRING("NF");
		break; // blast door
	case 4:
		m_ls.sLockedSentence = ALLOC_STRING("NFIRE");
		break; // fire door
	case 5:
		m_ls.sLockedSentence = ALLOC_STRING("NCHEM");
		break; // chemical door
	case 6:
		m_ls.sLockedSentence = ALLOC_STRING("NRAD");
		break; // radiation door
	case 7:
		m_ls.sLockedSentence = ALLOC_STRING("NCON");
		break; // gen containment
	case 8:
		m_ls.sLockedSentence = ALLOC_STRING("NH");
		break; // maintenance door
	case 9:
		m_ls.sLockedSentence = ALLOC_STRING("NG");
		break; // broken door

	default:
		m_ls.sLockedSentence = 0;
		break;
	}

	switch (m_bUnlockedSentence)
	{
	case 1:
		m_ls.sUnlockedSentence = ALLOC_STRING("EA");
		break; // access granted
	case 2:
		m_ls.sUnlockedSentence = ALLOC_STRING("ED");
		break; // security door
	case 3:
		m_ls.sUnlockedSentence = ALLOC_STRING("EF");
		break; // blast door
	case 4:
		m_ls.sUnlockedSentence = ALLOC_STRING("EFIRE");
		break; // fire door
	case 5:
		m_ls.sUnlockedSentence = ALLOC_STRING("ECHEM");
		break; // chemical door
	case 6:
		m_ls.sUnlockedSentence = ALLOC_STRING("ERAD");
		break; // radiation door
	case 7:
		m_ls.sUnlockedSentence = ALLOC_STRING("ECON");
		break; // gen containment
	case 8:
		m_ls.sUnlockedSentence = ALLOC_STRING("EH");
		break; // maintenance door

	default:
		m_ls.sUnlockedSentence = 0;
		break;
	}

	// TS breakable-door debris assets (only when the door carries a maxdamage threshold).
	if (m_flBreakHealth > 0)
	{
		CBreakable::MaterialSoundPrecache((Materials)m_DoorMaterial);
		if (m_DoorMaterial == matGlass)
			PRECACHE_MODEL("sprites/glassfrag.spr");
		else
			PRECACHE_MODEL((char*)STRING(pev->model)); // gib the door's own model
	}
}

//
// Doors not tied to anything (e.g. button, another door) can be touched, to make them activate.
//
void CBaseDoor::DoorTouch(CBaseEntity* pOther)
{
	entvars_t* pevToucher = pOther->pev;

	// Ignore touches by anything but players
	if (!FClassnameIs(pevToucher, "player"))
		return;

	// If door has master, and it's not ready to trigger,
	// play 'locked' sound

	if (!FStringNull(m_sMaster) && !UTIL_IsMasterTriggered(m_sMaster, pOther))
		PlayLockSounds(pev, &m_ls, true, false);

	// If door is somebody's target, then touching does nothing.
	// You have to activate the owner (e.g. button).

	if (!FStringNull(pev->targetname))
	{
		// play locked sound
		PlayLockSounds(pev, &m_ls, true, false);
		return;
	}

	m_hActivator = pOther; // remember who activated the door

	if (DoorActivate())
		SetTouch(NULL); // Temporarily disable the touch function, until movement is finished.
}


//
// Used by SUB_UseTargets, when a door is the target of a button.
//
void CBaseDoor::Use(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value)
{
	m_hActivator = pActivator;
	// if not ready to be used, ignore "use" command.
	if (m_toggle_state == TS_AT_BOTTOM || FBitSet(pev->spawnflags, SF_DOOR_NO_AUTO_RETURN) && m_toggle_state == TS_AT_TOP)
		DoorActivate();
}

//
// Causes the door to "do its thing", i.e. start moving, and cascade activation.
//
bool CBaseDoor::DoorActivate()
{
	if (!UTIL_IsMasterTriggered(m_sMaster, m_hActivator))
		return false;

	if (FBitSet(pev->spawnflags, SF_DOOR_NO_AUTO_RETURN) && m_toggle_state == TS_AT_TOP)
	{ // door should close
		DoorGoDown();
	}
	else
	{ // door should open

		if (m_hActivator != NULL && m_hActivator->IsPlayer())
		{ // give health if player opened the door (medikit)
			// VARS( m_eoActivator )->health += m_bHealthValue;

			m_hActivator->TakeHealth(m_bHealthValue, DMG_GENERIC);
		}

		// play door unlock sounds
		PlayLockSounds(pev, &m_ls, false, false);

		DoorGoUp();
	}

	return true;
}

extern Vector VecBModelOrigin(entvars_t* pevBModel);

//
// Starts the door going to its "up" position (simply ToggleData->vecPosition2).
//
void CBaseDoor::DoorGoUp()
{
	entvars_t* pevActivator;

	// It could be going-down, if blocked.
	ASSERT(m_toggle_state == TS_AT_BOTTOM || m_toggle_state == TS_GOING_DOWN);

	// emit door moving and stop sounds on CHAN_STATIC so that the multicast doesn't
	// filter them out and leave a client stuck with looping door sounds!
	if (!FBitSet(pev->spawnflags, SF_DOOR_SILENT))
	{
		if (m_toggle_state != TS_GOING_UP && m_toggle_state != TS_GOING_DOWN)
			EMIT_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving), 1, ATTN_NORM);
	}

	m_toggle_state = TS_GOING_UP;

	SetMoveDone(&CBaseDoor::DoorHitTop);
	if (FClassnameIs(pev, "func_door_rotating")) // !!! BUGBUG Triggered doors don't work with this yet
	{
		float sign = 1.0;

		if (m_hActivator != NULL)
		{
			pevActivator = m_hActivator->pev;

			if (!FBitSet(pev->spawnflags, SF_DOOR_ONEWAY) && 0 != pev->movedir.y) // Y axis rotation, move away from the player
			{
				Vector vec = pevActivator->origin - pev->origin;
				Vector angles = pevActivator->angles;
				angles.x = 0;
				angles.z = 0;
				UTIL_MakeVectors(angles);
				//			Vector vnext = (pevToucher->origin + (pevToucher->velocity * 10)) - pev->origin;
				UTIL_MakeVectors(pevActivator->angles);
				Vector vnext = (pevActivator->origin + (gpGlobals->v_forward * 10)) - pev->origin;
				if ((vec.x * vnext.y - vec.y * vnext.x) < 0)
					sign = -1.0;
			}
		}
		AngularMove(m_vecAngle2 * sign, pev->speed);
	}
	else
		LinearMove(m_vecPosition2, pev->speed);
}


//
// The door has reached the "up" position.  Either go back down, or wait for another activation.
//
void CBaseDoor::DoorHitTop()
{
	if (!FBitSet(pev->spawnflags, SF_DOOR_SILENT))
	{
		STOP_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving));
		EMIT_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseArrived), 1, ATTN_NORM);
	}

	ASSERT(m_toggle_state == TS_GOING_UP);
	m_toggle_state = TS_AT_TOP;

	// toggle-doors don't come down automatically, they wait for refire.
	if (FBitSet(pev->spawnflags, SF_DOOR_NO_AUTO_RETURN))
	{
		// Re-instate touch method, movement is complete
		if (!FBitSet(pev->spawnflags, SF_DOOR_USE_ONLY))
			SetTouch(&CBaseDoor::DoorTouch);
	}
	else
	{
		// In flWait seconds, DoorGoDown will fire, unless wait is -1, then door stays open
		pev->nextthink = pev->ltime + m_flWait;
		SetThink(&CBaseDoor::DoorGoDown);

		if (m_flWait == -1)
		{
			pev->nextthink = -1;
		}
	}

	// Fire the close target (if startopen is set, then "top" is closed) - netname is the close target
	if (!FStringNull(pev->netname) && (pev->spawnflags & SF_DOOR_START_OPEN) != 0)
		FireTargets(STRING(pev->netname), m_hActivator, this, USE_TOGGLE, 0);

	SUB_UseTargets(m_hActivator, USE_TOGGLE, 0); // this isn't finished
}


//
// Starts the door going to its "down" position (simply ToggleData->vecPosition1).
//
void CBaseDoor::DoorGoDown()
{
	if (!FBitSet(pev->spawnflags, SF_DOOR_SILENT))
	{
		if (m_toggle_state != TS_GOING_UP && m_toggle_state != TS_GOING_DOWN)
			EMIT_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving), 1, ATTN_NORM);
	}

#ifdef DOOR_ASSERT
	ASSERT(m_toggle_state == TS_AT_TOP);
#endif // DOOR_ASSERT
	m_toggle_state = TS_GOING_DOWN;

	SetMoveDone(&CBaseDoor::DoorHitBottom);
	if (FClassnameIs(pev, "func_door_rotating")) //rotating door
		AngularMove(m_vecAngle1, pev->speed);
	else
		LinearMove(m_vecPosition1, pev->speed);
}

//
// The door has reached the "down" position.  Back to quiescence.
//
void CBaseDoor::DoorHitBottom()
{
	if (!FBitSet(pev->spawnflags, SF_DOOR_SILENT))
	{
		STOP_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving));
		EMIT_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseArrived), 1, ATTN_NORM);
	}

	ASSERT(m_toggle_state == TS_GOING_DOWN);
	m_toggle_state = TS_AT_BOTTOM;

	// Re-instate touch method, cycle is complete
	if (FBitSet(pev->spawnflags, SF_DOOR_USE_ONLY))
	{ // use only door
		SetTouch(NULL);
	}
	else // touchable door
		SetTouch(&CBaseDoor::DoorTouch);

	SUB_UseTargets(m_hActivator, USE_TOGGLE, 0); // this isn't finished

	// Fire the close target (if startopen is set, then "top" is closed) - netname is the close target
	if (!FStringNull(pev->netname) && (pev->spawnflags & SF_DOOR_START_OPEN) == 0)
		FireTargets(STRING(pev->netname), m_hActivator, this, USE_TOGGLE, 0);
}

void CBaseDoor::Blocked(CBaseEntity* pOther)
{
	edict_t* pentTarget = NULL;
	CBaseDoor* pDoor = NULL;


	// Hurt the blocker a little.
	if (0 != pev->dmg)
		pOther->TakeDamage(pev, pev, pev->dmg, DMG_CRUSH);

	// if a door has a negative wait, it would never come back if blocked,
	// so let it just squash the object to death real fast

	if (m_flWait >= 0)
	{
		if (m_toggle_state == TS_GOING_DOWN)
		{
			DoorGoUp();
		}
		else
		{
			DoorGoDown();
		}
	}

	// Block all door pieces with the same targetname here.
	if (!FStringNull(pev->targetname))
	{
		for (;;)
		{
			pentTarget = FIND_ENTITY_BY_TARGETNAME(pentTarget, STRING(pev->targetname));

			if (VARS(pentTarget) != pev)
			{
				if (FNullEnt(pentTarget))
					break;

				if (FClassnameIs(pentTarget, "func_door") || FClassnameIs(pentTarget, "func_door_rotating"))
				{

					pDoor = GetClassPtr((CBaseDoor*)VARS(pentTarget));

					if (pDoor->m_flWait >= 0)
					{
						if (pDoor->pev->velocity == pev->velocity && pDoor->pev->avelocity == pev->velocity)
						{
							// this is the most hacked, evil, bastardized thing I've ever seen. kjb
							if (FClassnameIs(pentTarget, "func_door"))
							{ // set origin to realign normal doors
								pDoor->pev->origin = pev->origin;
								pDoor->pev->velocity = g_vecZero; // stop!
							}
							else
							{ // set angles to realign rotating doors
								pDoor->pev->angles = pev->angles;
								pDoor->pev->avelocity = g_vecZero;
							}
						}

						if (!FBitSet(pev->spawnflags, SF_DOOR_SILENT))
							STOP_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving));

						if (pDoor->m_toggle_state == TS_GOING_DOWN)
							pDoor->DoorGoUp();
						else
							pDoor->DoorGoDown();
					}
				}
			}
		}
	}
}


/*QUAKED FuncRotDoorSpawn (0 .5 .8) ? START_OPEN REVERSE  
DOOR_DONT_LINK TOGGLE X_AXIS Y_AXIS
if two doors touch, they are assumed to be connected and operate as  
a unit.

TOGGLE causes the door to wait in both the start and end states for  
a trigger event.

START_OPEN causes the door to move to its destination when spawned,  
and operate in reverse.  It is used to temporarily or permanently  
close off an area when triggered (not usefull for touch or  
takedamage doors).

You need to have an origin brush as part of this entity.  The  
center of that brush will be
the point around which it is rotated. It will rotate around the Z  
axis by default.  You can
check either the X_AXIS or Y_AXIS box to change that.

"distance" is how many degrees the door will be rotated.
"speed" determines how fast the door moves; default value is 100.

REVERSE will cause the door to rotate in the opposite direction.

"angle"		determines the opening direction
"targetname" if set, no touch field will be spawned and a remote  
button or trigger field activates the door.
"health"	if set, door must be shot open
"speed"		movement speed (100 default)
"wait"		wait before returning (3 default, -1 = never return)
"dmg"		damage to inflict when blocked (2 default)
"sounds"
0)	no sound
1)	stone
2)	base
3)	stone chain
4)	screechy metal
*/
class CRotDoor : public CBaseDoor
{
public:
	void Spawn() override;
	void SetToggleState(int state) override;
	void BreakableRespawn(bool forceRespawn) override; // @0xb7838 — restore angles only (keeps position)
};

// @0xb7838 — like CBaseDoor's, but a rotating door keeps its position (restore angles, not origin).
void CRotDoor::BreakableRespawn(bool forceRespawn)
{
	if (!(pev->deadflag == DEAD_DEAD || pev->effects == EF_NODRAW || forceRespawn))
		return;

	CBaseEntity* pList[1];
	int count = UTIL_EntitiesInBox(pList, 1, pev->absmin, pev->absmax, FL_CLIENT | FL_MONSTER);
	if (count == 0)
	{
		pev->deadflag = DEAD_NO;
		pev->effects = 0;
		pev->angles = m_vecSpawnAngles;
		pev->avelocity = g_vecZero;
		pev->velocity = g_vecZero;
		SetTouch(NULL);
		SetThink(NULL);
		Spawn();
	}
	else if ((CBaseEntity*)m_hRespawner)
	{
		CInfoBreakable* pHelper = (CInfoBreakable*)(CBaseEntity*)m_hRespawner;
		pHelper->SetThink(&CInfoBreakable::RespawnThink);
		pHelper->pev->nextthink = gpGlobals->time + 5.0f;
	}
}

LINK_ENTITY_TO_CLASS(func_door_rotating, CRotDoor);


// @0xb79b8: TS's own CRotDoor::Spawn opens with the same breakable-door init as CBaseDoor::Spawn
// (@0xb60cc) and ends with the same info_breakable helper, so a rotating door with "maxdamage"
// breaks and respawns like a func_door. The pose is saved before START_OPEN turns the door.
void CRotDoor::Spawn()
{
	m_vecSpawnOrigin = pev->origin;
	m_vecSpawnAngles = pev->angles;
	m_iAccumDamage = 0;
	m_flDoorNextAttack = 0;
	pev->takedamage = (m_flBreakHealth > 0) ? DAMAGE_YES : DAMAGE_NO;

	Precache();
	// set the axis of rotation
	CBaseToggle::AxisDir(pev);

	// check for clockwise rotation
	if (FBitSet(pev->spawnflags, SF_DOOR_ROTATE_BACKWARDS))
		pev->movedir = pev->movedir * -1;

	//m_flWait			= 2; who the hell did this? (sjb)
	m_vecAngle1 = pev->angles;
	m_vecAngle2 = pev->angles + pev->movedir * m_flMoveDistance;

	ASSERTSZ(m_vecAngle1 != m_vecAngle2, "rotating door start/end positions are equal");

	if (FBitSet(pev->spawnflags, SF_DOOR_PASSABLE))
		pev->solid = SOLID_NOT;
	else
		pev->solid = SOLID_BSP;

	pev->movetype = MOVETYPE_PUSH;
	UTIL_SetOrigin(pev, pev->origin);
	SET_MODEL(ENT(pev), STRING(pev->model));

	if (pev->speed == 0)
		pev->speed = 100;

	// DOOR_START_OPEN is to allow an entity to be lighted in the closed position
	// but spawn in the open position
	if (FBitSet(pev->spawnflags, SF_DOOR_START_OPEN))
	{ // swap pos1 and pos2, put door at pos2, invert movement direction
		pev->angles = m_vecAngle2;
		Vector vecSav = m_vecAngle1;
		m_vecAngle2 = m_vecAngle1;
		m_vecAngle1 = vecSav;
		pev->movedir = pev->movedir * -1;
	}

	m_toggle_state = TS_AT_BOTTOM;

	if (FBitSet(pev->spawnflags, SF_DOOR_USE_ONLY))
	{
		SetTouch(NULL);
	}
	else // touchable button
		SetTouch(&CRotDoor::DoorTouch);

	// @0xb7c75: the respawn helper, created once and linked both ways, as CBaseDoor::Spawn does
	if (m_flRespawnTime >= 0)
	{
		if (!(CBaseEntity*)m_hRespawner)
			m_hRespawner = CBaseEntity::Create("info_breakable", VecBModelOrigin(pev), pev->angles, edict());
		if ((CBaseEntity*)m_hRespawner)
			((CInfoBreakable*)(CBaseEntity*)m_hRespawner)->m_hBreakable = this;
	}
}


void CRotDoor::SetToggleState(int state)
{
	if (state == TS_AT_TOP)
		pev->angles = m_vecAngle2;
	else
		pev->angles = m_vecAngle1;

	UTIL_SetOrigin(pev, pev->origin);
}


class CMomentaryDoor : public CBaseToggle
{
public:
	void Spawn() override;
	void Precache() override;

	bool KeyValue(KeyValueData* pkvd) override;
	void Use(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value) override;
	int ObjectCaps() override { return CBaseToggle::ObjectCaps() & ~FCAP_ACROSS_TRANSITION; }

	bool Save(CSave& save) override;
	bool Restore(CRestore& restore) override;
	static TYPEDESCRIPTION m_SaveData[];

	void EXPORT DoorMoveDone();
	void EXPORT StopMoveSound();

	byte m_bMoveSnd; // sound a door makes while moving
};

LINK_ENTITY_TO_CLASS(momentary_door, CMomentaryDoor);

TYPEDESCRIPTION CMomentaryDoor::m_SaveData[] =
	{
		DEFINE_FIELD(CMomentaryDoor, m_bMoveSnd, FIELD_CHARACTER),
};

IMPLEMENT_SAVERESTORE(CMomentaryDoor, CBaseToggle);

void CMomentaryDoor::Spawn()
{
	SetMovedir(pev);

	pev->solid = SOLID_BSP;
	pev->movetype = MOVETYPE_PUSH;

	UTIL_SetOrigin(pev, pev->origin);
	SET_MODEL(ENT(pev), STRING(pev->model));

	if (pev->speed == 0)
		pev->speed = 100;
	if (pev->dmg == 0)
		pev->dmg = 2;

	m_vecPosition1 = pev->origin;
	// Subtract 2 from size because the engine expands bboxes by 1 in all directions making the size too big
	m_vecPosition2 = m_vecPosition1 + (pev->movedir * (fabs(pev->movedir.x * (pev->size.x - 2)) + fabs(pev->movedir.y * (pev->size.y - 2)) + fabs(pev->movedir.z * (pev->size.z - 2)) - m_flLip));
	ASSERTSZ(m_vecPosition1 != m_vecPosition2, "door start/end positions are equal");

	if (FBitSet(pev->spawnflags, SF_DOOR_START_OPEN))
	{ // swap pos1 and pos2, put door at pos2
		UTIL_SetOrigin(pev, m_vecPosition2);
		m_vecPosition2 = m_vecPosition1;
		m_vecPosition1 = pev->origin;
	}
	SetTouch(NULL);

	Precache();
}

void CMomentaryDoor::Precache()
{

	// set the door's "in-motion" sound
	switch (m_bMoveSnd)
	{
	case 0:
		pev->noiseMoving = ALLOC_STRING("common/null.wav");
		break;
	case 1:
		PRECACHE_SOUND("doors/doormove1.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove1.wav");
		break;
	case 2:
		PRECACHE_SOUND("doors/doormove2.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove2.wav");
		break;
	case 3:
		PRECACHE_SOUND("doors/doormove3.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove3.wav");
		break;
	case 4:
		PRECACHE_SOUND("doors/doormove4.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove4.wav");
		break;
	case 5:
		PRECACHE_SOUND("doors/doormove5.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove5.wav");
		break;
	case 6:
		PRECACHE_SOUND("doors/doormove6.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove6.wav");
		break;
	case 7:
		PRECACHE_SOUND("doors/doormove7.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove7.wav");
		break;
	case 8:
		PRECACHE_SOUND("doors/doormove8.wav");
		pev->noiseMoving = ALLOC_STRING("doors/doormove8.wav");
		break;
	default:
		pev->noiseMoving = ALLOC_STRING("common/null.wav");
		break;
	}
}

bool CMomentaryDoor::KeyValue(KeyValueData* pkvd)
{

	if (FStrEq(pkvd->szKeyName, "movesnd"))
	{
		m_bMoveSnd = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "stopsnd"))
	{
		//		m_bStopSnd = atof(pkvd->szValue);
		return true;
	}
	else if (FStrEq(pkvd->szKeyName, "healthvalue"))
	{
		//		m_bHealthValue = atof(pkvd->szValue);
		return true;
	}

	return CBaseToggle::KeyValue(pkvd);
}

void CMomentaryDoor::Use(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value)
{
	if (useType != USE_SET) // Momentary buttons will pass down a float in here
		return;

	if (value > 1.0)
		value = 1.0;
	if (value < 0.0)
		value = 0.0;

	Vector move = m_vecPosition1 + (value * (m_vecPosition2 - m_vecPosition1));

	Vector delta = move - pev->origin;
	//float speed = delta.Length() * 10;
	float speed = delta.Length() / 0.1; // move there in 0.1 sec
	if (speed == 0)
		return;

	// This entity only thinks when it moves, so if it's thinking, it's in the process of moving
	// play the sound when it starts moving (not yet thinking)
	if (pev->nextthink < pev->ltime || pev->nextthink == 0)
		EMIT_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving), 1, ATTN_NORM);
	// If we already moving to designated point, return
	else if (move == m_vecFinalDest)
		return;

	SetMoveDone(&CMomentaryDoor::DoorMoveDone);
	LinearMove(move, speed);
}

//
// The door has reached needed position.
//
void CMomentaryDoor::DoorMoveDone()
{
	// Stop sounds at the next think, rather than here as another
	// Use call might immediately follow the end of this move
	//This think function will be replaced by LinearMove if that happens.
	SetThink(&CMomentaryDoor::StopMoveSound);
	pev->nextthink = pev->ltime + 0.1f;
}

void CMomentaryDoor::StopMoveSound()
{
	STOP_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseMoving));
	EMIT_SOUND(ENT(pev), CHAN_STATIC, (char*)STRING(pev->noiseArrived), 1, ATTN_NORM);
	SetThink(nullptr);
}
