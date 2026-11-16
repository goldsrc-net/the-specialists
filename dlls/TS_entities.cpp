/***
*  TS_entities.cpp — RECONSTRUCTED original TU (funcfile.tsv): the small map-entity classes,
*  in the binary's address order: CTSTrigger (lands here in C13), CTSSlowMotion,
*  CTSSlowMotionPoint, CTSDMHill, CTSModel, CTSMapGlobals, CTSSellZone, CTSPlayerAmount,
*  CTSTimer, CTSCustomTarget. Merged from the former per-class files (see git history).
***/
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "gamerules.h"
#include "ts_gamerules.h"
#include "ts_utils.h"        // TSHudMessageAll
#include "UserMessages.h"    // gmsgScoreInfo (CTSDMHill)
#include "ts_objectives.h"   // CTSTrigger cross-deps (C13)
#include "ts_slowmotion.h"
#include "ts_slowmotionpoint.h"
#include "ts_dmhill.h"
#include "ts_mapglobals.h"
#include "ts_playeramount.h"
#include "ts_flag.h"          // HaveFlag (ts_trigger spawnflag 8)
#include "ts_weapons.h"       // CashBased, get_weapon_info (ts_sellzone)
#include "ts_worldgun.h"      // CTSWorldGun (ts_sellzone)

// CTSTrigger — first in the original address order (extracted from ts_objectives.cpp).
//============================================================================
// CTSTrigger — a TS brush trigger: a live player touching it fires its target when he is on the
// allowed team, and its message is shown whoever touched it, then it waits `nextcheck` seconds.
// @0x609b8 Spawn / @0x60ad8 WaitTouch / @0x60c68 WaitThink. The original's base is CBaseTrigger;
// reTS keeps CTSObjective for the team member (attacker_team, the original's +0x108).
//============================================================================
#define SF_TS_TRIGGER_FLAGCARRIER 8

class CTSTrigger : public CTSObjective
{
public:
	void Spawn() override;
	bool KeyValue(KeyValueData *pkvd) override;
	void EXPORT WaitTouch(CBaseEntity *pOther);
	void EXPORT WaitThink();                  // @0x60c68 — reschedules; nothing sets it as the think
	int ObjectCaps() override { return 0; }   // @0x63ad0

	float m_flNextCheck = 0;   // +0x10c nextcheck (the wait, at least 0.5 s)
	float m_flNextAllowed = 0; // +0x104 earliest time it can be touched again
};

LINK_ENTITY_TO_CLASS(ts_trigger, CTSTrigger);

// @0x609b8: a hidden brush trigger (movetype 5) with no think, touchable after the wait.
void CTSTrigger::Spawn()
{
	pev->classname = MAKE_STRING("ts_trigger");
	SET_MODEL(ENT(pev), STRING(pev->model));
	UTIL_SetSize(pev, pev->mins, pev->maxs);
	UTIL_SetOrigin(pev, pev->origin);
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_TRIGGER;
	pev->movetype = MOVETYPE_FLY;
	SetTouch(&CTSTrigger::WaitTouch);
	SetThink(NULL);
	const float wait = (m_flNextCheck < 0.5f) ? 0.5f : m_flNextCheck;
	pev->nextthink = gpGlobals->time + wait;
	m_flNextAllowed = gpGlobals->time + wait;
}

void CTSTrigger::WaitThink()
{
	float wait = (m_flNextCheck < 0.5f) ? 0.5f : m_flNextCheck;
	pev->nextthink = gpGlobals->time + wait;
}

// @0x60890: nextcheck, attackerteam and allowedteam (the same member; allowedteam 2 means any team,
// -1). No other key, and no base KeyValue.
bool CTSTrigger::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "nextcheck"))
	{
		m_flNextCheck = atof(pkvd->szValue);
		return true;
	}
	if (FStrEq(pkvd->szKeyName, "attackerteam"))
	{
		attacker_team = (int)lrint(atof(pkvd->szValue));
		return true;
	}
	if (FStrEq(pkvd->szKeyName, "allowedteam"))
	{
		attacker_team = (int)lrint(atof(pkvd->szValue));
		if (attacker_team == 2)
			attacker_team = -1;
		return true;
	}
	return false;
}

// @0x60ad8: a live player past the wait. With spawnflag 8 only the flag carrier counts. The team
// decides only whether the target fires; the message (a format string, %s the toucher's name) shows
// and the wait starts again either way.
void CTSTrigger::WaitTouch(CBaseEntity *pOther)
{
	if (gpGlobals->time < m_flNextAllowed)
		return;
	if (!pOther->IsPlayer() || !pOther->IsAlive())
		return;
	bool allowed = true;
	if (attacker_team != -1 && attacker_team != TSGameRules->GetTeamIndex(pOther->TeamID()))
		allowed = false;
	if ((pev->spawnflags & SF_TS_TRIGGER_FLAGCARRIER) != 0 && !HaveFlag((CBasePlayer *)pOther))
		return;

	if (allowed && !FStringNull(pev->target))
		FireTargets((char *)STRING(pev->target), this, this, USE_TOGGLE, 0);
	if (!FStringNull(pev->message))
	{
		// The original sprintf's into 124 bytes unchecked; here the text stops at the buffer's end.
		char text[124];
		snprintf(text, sizeof(text), STRING(pev->message), STRING(pOther->pev->netname));
		TSHudMessageAll(0, 0xff, 0, 8, 30, text);
	}

	float delay = m_flNextCheck < 0.5f ? 0.5f : m_flNextCheck;
	m_flNextAllowed = gpGlobals->time + delay;
}


// ===================== ts_slowmotion.cpp (merged) =====================
#define SF_SLOMO_START_ON 1
#define SF_SLOMO_ALL_MAP 2
#define SF_SLOMO_SLOW_BULLETS 4

LINK_ENTITY_TO_CLASS(ts_slowmotion, CTSSlowMotion);

TYPEDESCRIPTION CTSSlowMotion::m_SaveData[] = {
	DEFINE_FIELD(CTSSlowMotion, duration, FIELD_FLOAT),
	DEFINE_FIELD(CTSSlowMotion, slow_ammount, FIELD_FLOAT),
};
IMPLEMENT_SAVERESTORE(CTSSlowMotion, CBaseEntity);

bool CTSSlowMotion::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "duration"))
	{
		duration = atof(pkvd->szValue);
		return true;
	}
	if (FStrEq(pkvd->szKeyName, "slowamt"))
	{
		slow_ammount = atof(pkvd->szValue);
		return true;
	}
	return CBaseEntity::KeyValue(pkvd);
}

// @0x60e14: a hidden brush trigger (movetype 5) polling every 0.5 s, nobody slowed yet. The drop range
// is half the diagonal of its absolute box (the size plus the engine's 1 unit each way), truncated.
void CTSSlowMotion::Spawn()
{
	pev->classname = MAKE_STRING("ts_slowmotion");
	SET_MODEL(ENT(pev), STRING(pev->model)); // brush model
	UTIL_SetSize(pev, pev->mins, pev->maxs);
	UTIL_SetOrigin(pev, pev->origin);
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_TRIGGER;
	pev->movetype = MOVETYPE_FLY;

	SetTouch(NULL);
	SetThink(&CTSSlowMotion::WaitThink);
	m_flStopTime = 0;
	m_iToggledOn = 0;
	pev->nextthink = gpGlobals->time + 0.5f;
	for (int i = 0; i < TS_SLOMO_MAX_PLAYERS; i++)
		m_hPlayers[i] = NULL;

	m_iRadius = (int)((pev->absmax - pev->absmin).Length() * 0.5f);
	m_iAllMap = (pev->spawnflags & SF_SLOMO_ALL_MAP) ? 1 : 0;
	m_bSlowBullets = (pev->spawnflags & SF_SLOMO_SLOW_BULLETS) ? 1 : 0;

	if (pev->spawnflags & SF_SLOMO_START_ON)
		ActivateSlow();
}

// @ ActivateSlow__13CTSSlowMotion — arm the zone (timed or toggle), set the touch.
void CTSSlowMotion::ActivateSlow()
{
	if (duration > 0.0f)
	{
		m_flStopTime = gpGlobals->time + duration;
	}
	else
	{
		m_flStopTime = 0;
		m_iToggledOn ^= 1;
		if (m_iToggledOn == 0)
			DeactivateSlow();
	}

	if (!m_iAllMap)
		SetTouch(&CTSSlowMotion::WaitTouch);
}

// @ DeactivateSlow__13CTSSlowMotion — restore everyone the zone slowed, clear it.
void CTSSlowMotion::DeactivateSlow()
{
	m_flStopTime = 0;

	if (!m_iAllMap)
	{
		for (int i = 0; i < TS_SLOMO_MAX_PLAYERS; i++)
		{
			CBaseEntity *pEnt = m_hPlayers[i];
			if (pEnt && pEnt->IsPlayer())
				((CBasePlayer *)pEnt)->GoSlow(1.0f, 0);
			m_hPlayers[i] = NULL;
		}
	}
	else
	{
		TSGameRules->GlobalSlowMotion2(0, 0);
		for (int i = 0; i < TS_SLOMO_MAX_PLAYERS; i++)
			m_hPlayers[i] = NULL;
	}
}

// @ WaitTouch__13CTSSlowMotion — slow the touching player (if not already slowed).
void CTSSlowMotion::WaitTouch(CBaseEntity *pOther)
{
	if (m_flStopTime == 0.0f && m_iToggledOn == 0)
		return; // zone not active

	if (!pOther->IsPlayer())
		return;

	CBasePlayer *pPlayer = (CBasePlayer *)pOther;
	if (pPlayer->InSlow())
		return; // already slowed (by this or another source)

	pPlayer->GoSlow(slow_ammount, 0);
	pPlayer->m_bSlowBullets = m_bSlowBullets != 0;

	int idx = ENTINDEX(pOther->edict());
	if (idx >= 0 && idx < TS_SLOMO_MAX_PLAYERS)
		m_hPlayers[idx] = pOther;
}

// @ WaitThink__13CTSSlowMotion — poll: keep the slow applied / time it out.
void CTSSlowMotion::WaitThink()
{
	pev->nextthink = gpGlobals->time + 0.5f;

	if (m_iToggledOn)
	{
		if (!m_iAllMap)
			CheckPlayersDistance();
		else
			TSGameRules->GlobalSlowMotion2(slow_ammount, m_bSlowBullets);
	}

	if (m_flStopTime != 0.0f)
	{
		if (m_flStopTime < gpGlobals->time)
		{
			SetTouch(NULL);
			DeactivateSlow();
		}
		else if (!m_iAllMap)
		{
			CheckPlayersDistance();
		}
		else
		{
			TSGameRules->GlobalSlowMotion2(slow_ammount, m_bSlowBullets);
		}
	}
}

// @0x6118c CheckPlayersDistance — every tracked player still slowed who is farther than the drop
// range from the zone's centre goes back to normal speed and leaves the list.
void CTSSlowMotion::CheckPlayersDistance()
{
	Vector center = Center();
	for (int i = 0; i < TS_SLOMO_MAX_PLAYERS; i++)
	{
		CBaseEntity *pEnt = m_hPlayers[i];
		if (!pEnt || !pEnt->IsPlayer())
			continue;

		CBasePlayer *pPlayer = (CBasePlayer *)pEnt;
		if (!pPlayer->InSlow())
			continue;

		float dist = (pPlayer->pev->origin - center).Length();
		if (dist > m_iRadius)
		{
			pPlayer->GoSlow(1.0f, 0);
			m_hPlayers[i] = NULL;
		}
	}
}

// @ Use__13CTSSlowMotion — trigger the zone (button/relay), announce, fire targets.
void CTSSlowMotion::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	ActivateSlow();

	if (!m_iAllMap)
		SetTouch(&CTSSlowMotion::WaitTouch);

	if (!FStringNull(pev->message))
		TSHudMessageAll(0, 255, 0, 8, 30, STRING(pev->message));

	if (!FStringNull(pev->target))
		FireTargets(STRING(pev->target), this, this, USE_TOGGLE, 0);
}

// ===================== ts_slowmotionpoint.cpp (merged) =====================

LINK_ENTITY_TO_CLASS(ts_slowmotionpoint, CTSSlowMotionPoint);

// @0x61564: a hidden point, thinking every 0.5 s from 0.1 s on, nobody slowed yet
void CTSSlowMotionPoint::Spawn()
{
	pev->classname = MAKE_STRING("ts_slowmotionpoint");
	UTIL_SetSize(pev, g_vecZero, g_vecZero);
	UTIL_SetOrigin(pev, pev->origin);
	pev->effects = EF_NODRAW;
	pev->movetype = MOVETYPE_NONE;
	pev->solid = SOLID_NOT;
	SetTouch(NULL);
	SetThink(&CTSSlowMotionPoint::WaitThink);
	pev->nextthink = gpGlobals->time + 0.1f;
	m_flEndTime = 0;
	m_flScale = 0;
	m_flValue = 0;
	m_bSlowBullets = false;
	for (int i = 0; i < 33; i++)
		m_hPlayers[i] = NULL;
}

// @0x618ac ActivateSlow — the slow factor and the lifetime
void CTSSlowMotionPoint::ActivateSlow(float scale, float duration, float value, bool slowBullets)
{
	m_flEndTime = gpGlobals->time + duration;
	m_flScale = scale;
	m_flValue = value;
	m_bSlowBullets = slowBullets;
}

// @0x61728 WaitThink — until the lifetime ends, every player not yet slowed whom the point sees (a
// trace to his eyes, ignoring monsters and glass, that ends clear) is slowed to the bubble's factor,
// dead or alive; then everyone it slowed goes back to normal and the point is removed.
void CTSSlowMotionPoint::WaitThink()
{
	pev->nextthink = gpGlobals->time + 0.5f;

	if (gpGlobals->time > m_flEndTime)
	{
		DeactivateSlow();
		UTIL_Remove(this);
		return;
	}

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = (CBasePlayer *)UTIL_PlayerByIndex(i);
		if (!p || !p->IsPlayer() || p->InSlow())
			continue;

		TraceResult tr;
		UTIL_TraceLine(pev->origin, p->pev->origin + p->pev->view_ofs, ignore_monsters, ignore_glass, NULL, &tr);
		if (tr.flFraction < 1.0f)
			continue;

		p->GoSlow(m_flScale, 0);
		p->m_bSlowBullets = m_bSlowBullets;
		const int idx = ENTINDEX(p->edict());
		if (idx < 33)
			m_hPlayers[idx] = p;
	}
}

// @0x6166c DeactivateSlow — everyone this bubble slowed goes back to normal speed.
void CTSSlowMotionPoint::DeactivateSlow()
{
	for (int i = 0; i < 33; i++)
	{
		CBaseEntity *p = m_hPlayers[i];
		if (p)
			p->GoSlow(1.0f, 0);
		m_hPlayers[i] = NULL;
	}
}

CTSSlowMotionPoint *CTSSlowMotionPoint::CreateSlowMotionPoint(const Vector &origin, float scale, float duration,
	float value, bool slowBullets)
{
	CTSSlowMotionPoint *pPoint = GetClassPtr((CTSSlowMotionPoint *)NULL);
	if (!pPoint)
	{
		UTIL_ClientPrintAll(HUD_PRINTTALK, "WARNING:cannot create slowmotion point!\n");
		return NULL;
	}
	pPoint->pev->origin = origin;
	pPoint->Spawn();
	pPoint->ActivateSlow(scale, duration, value, slowBullets);
	return pPoint;
}

// ===================== ts_dmhill.cpp (merged) =====================

LINK_ENTITY_TO_CLASS(ts_dmhill, CTSDMHill);

TYPEDESCRIPTION CTSDMHill::m_SaveData[] = {
	DEFINE_FIELD(CTSDMHill, nextcheck, FIELD_FLOAT),
	DEFINE_FIELD(CTSDMHill, fragseverycheck, FIELD_INTEGER),
	DEFINE_FIELD(CTSDMHill, fragseverycheckteam, FIELD_INTEGER),
};
IMPLEMENT_SAVERESTORE(CTSDMHill, CBaseEntity);

bool CTSDMHill::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "nextcheck"))
	{
		nextcheck = atof(pkvd->szValue);
		return true;
	}
	if (FStrEq(pkvd->szKeyName, "fragamt"))     { fragseverycheck = atoi(pkvd->szValue); return true; }
	if (FStrEq(pkvd->szKeyName, "fragamtteam")) { fragseverycheckteam = atoi(pkvd->szValue); return true; }
	return CBaseEntity::KeyValue(pkvd);
}

// @Spawn__9CTSDMHill — a (usually brush) trigger volume. Only arms the touch handler if the relevant
// frag amount is configured for the active mode (DM: fragamt>0; team: fragamtteam>0).
void CTSDMHill::Spawn()
{
	if (!FStringNull(pev->model))
		SET_MODEL(ENT(pev), STRING(pev->model)); // the brush, from the map
	else
		UTIL_SetSize(pev, Vector(-48, -48, -36), Vector(48, 48, 36)); // synthetic/point fallback
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_TRIGGER; // non-blocking trigger volume
	pev->movetype = MOVETYPE_NONE;
	UTIL_SetOrigin(pev, pev->origin);

	const bool team = g_teamplay;
	if ((!team || fragseverycheckteam > 0) && (!team || fragseverycheck > 0))
		SetTouch(&CTSDMHill::WaitTouch);
	else
		SetTouch(NULL);

	SetThink(&CTSDMHill::WaitThink);
	float check = (nextcheck < 0.5f) ? 0.5f : nextcheck;
	pev->nextthink = gpGlobals->time + check;
	m_flStartGate = gpGlobals->time + 1.0f; // 1s startup grace
	m_hKing = NULL;
}

// @WaitThink — keep the entity thinking (the holder logic is all in WaitTouch).
void CTSDMHill::WaitThink()
{
	float check = (nextcheck < 0.5f) ? 0.5f : nextcheck;
	pev->nextthink = gpGlobals->time + check;
}

// @WaitTouch — a living player overlapping the volume. Track the holder; pay out once they've held
// past the award time. A same-team toucher keeps the hold; an enemy toucher drops it.
void CTSDMHill::WaitTouch(CBaseEntity *pOther)
{
	if (gpGlobals->time < m_flStartGate)
		return;
	if (!pOther || !pOther->IsPlayer() || !pOther->IsAlive())
		return;
	CBasePlayer *pPlayer = (CBasePlayer *)pOther;
	const float check = (nextcheck < 0.5f) ? 0.5f : nextcheck;

	// No holder yet — this toucher claims it and starts the hold timer.
	if (!((CBaseEntity *)m_hKing))
	{
		m_hKing = pPlayer;
		m_flStartGate = 0;
		m_flAwardTime = gpGlobals->time + check;
		m_flLastTouch = gpGlobals->time + check;
		return;
	}

	if ((CBaseEntity *)m_hKing == pPlayer)
	{
		// the holder is still here — drop them only if their touches lapsed past a full check window
		if (check < gpGlobals->time - m_flLastTouch)
		{
			m_hKing = NULL;
			m_flLastTouch = gpGlobals->time + check;
			return;
		}
	}
	else
	{
		// a DIFFERENT player touched — a teammate keeps the hold, an enemy steals/resets it
		CBaseEntity *pKing = m_hKing;
		bool sameTeam = false;
		if (pKing && g_teamplay)
			sameTeam = TSGameRules->GetTeamIndex(((CBasePlayer *)pKing)->TeamID()) ==
				TSGameRules->GetTeamIndex(pPlayer->TeamID());
		if (!sameTeam)
		{
			m_hKing = NULL;
			m_flLastTouch = gpGlobals->time + check;
			return;
		}
	}

	if (gpGlobals->time <= m_flAwardTime)
		return; // held, but not long enough yet

	// --- PAYOUT ---
	const bool team = g_teamplay;
	char kingMsg[128] = {0};
	if (!FStringNull(pev->message))
	{
		if (team)
		{
			strcpy(kingMsg, "Team ");
			strcat(kingMsg, pPlayer->TeamID());
		}
		else
		{
			strncpy(kingMsg, STRING(pPlayer->pev->netname), sizeof(kingMsg) - 1);
		}
		strcat(kingMsg, " ");
		strncat(kingMsg, STRING(pev->message), sizeof(kingMsg) - strlen(kingMsg) - 1);
	}

	if (!team)
	{
		pPlayer->pev->frags += fragseverycheck;
		MESSAGE_BEGIN(MSG_ALL, gmsgScoreInfo);
		WRITE_BYTE(pPlayer->entindex());
		WRITE_SHORT((int)pPlayer->pev->frags);
		WRITE_SHORT(pPlayer->m_iDeaths);
		WRITE_SHORT(0);
		WRITE_SHORT(g_pGameRules->GetTeamIndex(pPlayer->m_szTeamName) + 1);
		MESSAGE_END();
		if (!FStringNull(pev->message))
			strncat(kingMsg, UTIL_VarArgs(" ,%i frags for him", fragseverycheck),
				sizeof(kingMsg) - strlen(kingMsg) - 1);
	}
	else
	{
		// award every player on the holder's team
		CBaseEntity *pEnt = NULL;
		while ((pEnt = UTIL_FindEntityByClassname(pEnt, "player")) != NULL)
		{
			if (!pEnt->IsPlayer())
				continue;
			CBasePlayer *p = (CBasePlayer *)pEnt;
			if (strcmp(p->TeamID(), pPlayer->TeamID()) != 0)
				continue;
			p->pev->frags += fragseverycheckteam;
			MESSAGE_BEGIN(MSG_ALL, gmsgScoreInfo);
			WRITE_BYTE(p->entindex());
			WRITE_SHORT((int)p->pev->frags);
			WRITE_SHORT(p->m_iDeaths);
			WRITE_SHORT(0);
			WRITE_SHORT(g_pGameRules->GetTeamIndex(p->m_szTeamName) + 1);
			MESSAGE_END();
		}
		if (!FStringNull(pev->message))
			strncat(kingMsg, UTIL_VarArgs(" ,%i frags for they", fragseverycheckteam),
				sizeof(kingMsg) - strlen(kingMsg) - 1);
	}

	if (!FStringNull(pev->message))
	{
		float dur = check * 10.0f;
		if (dur > 30.0f) dur = 30.0f;
		TSHudMessageAll(0, 0xff, 0, 8, (int)(dur + 0.5f), kingMsg);
	}

	m_flAwardTime = gpGlobals->time + check;
	if (!FStringNull(pev->target))
		FireTargets(STRING(pev->target), this, this, USE_TOGGLE, 0);
	m_flLastTouch = gpGlobals->time;
}

// ===================== ts_model.cpp (merged) =====================

class CTSModel : public CBaseToggle // the original's base (its typeinfo): wait, master, lip, distance,
                                  // delay and killtarget reach CBaseToggle::KeyValue
{
public:
	void Spawn(void) override;
	void Precache(void) override;
};

LINK_ENTITY_TO_CLASS(ts_model, CTSModel);

// Original Precache is empty (the model is precached in Spawn); kept because
// Spawn calls it through the vtable.
void CTSModel::Precache(void)
{
}

void CTSModel::Spawn(void)
{
	Precache();
	pev->classname = MAKE_STRING("ts_model");

	if (FStringNull(pev->model))
	{
		// No model assigned — nothing to display, so remove ourselves.
		REMOVE_ENTITY(ENT(pev));
		return;
	}

	PRECACHE_MODEL((char *)STRING(pev->model));
	SET_MODEL(ENT(pev), STRING(pev->model));
	UTIL_SetSize(pev, Vector(0, 0, 0), Vector(0, 0, 0));
	UTIL_SetOrigin(pev, pev->origin);

	InitBoneControllers();

	// Cycler-style decorative prop: non-moving, animates client-side from its
	// sequence. (Unlike CCycler it is not a damageable target and is point-sized.)
	pev->movetype = MOVETYPE_NONE;
	pev->solid = SOLID_SLIDEBOX;
	pev->effects = 0;
	pev->framerate = 1.0f;
	pev->yaw_speed = 5;
	pev->ideal_yaw = pev->angles.y;

	m_flFrameRate = 75;
	m_flGroundSpeed = 0;

	pev->nextthink += 1.0;

	ResetSequenceInfo();
}

// ===================== ts_mapglobals.cpp (merged) =====================

LINK_ENTITY_TO_CLASS(ts_mapglobals, CTSMapGlobals);

// @0x62488/0x624d8 — standard save/restore of the two vestigial fields (WriteFields count 2), in
// the original's order (save table @0x1011a0: player_respawn_num +0x64, then player_at_a_time_num +0x68).
TYPEDESCRIPTION CTSMapGlobals::m_SaveData[] = {
	DEFINE_FIELD(CTSMapGlobals, player_respawn_num, FIELD_INTEGER),
	DEFINE_FIELD(CTSMapGlobals, player_at_a_time_num, FIELD_INTEGER),
};
IMPLEMENT_SAVERESTORE(CTSMapGlobals, CBaseEntity);

bool CTSMapGlobals::KeyValue(KeyValueData *pkvd)
{
	// Original marks every key unhandled — the entity has no keyvalues beyond the
	// engine-applied spawnflags/origin/angles.
	return false;
}

void CTSMapGlobals::Precache(void)
{
}

void CTSMapGlobals::Spawn(void)
{
	Precache();
	pev->movetype = MOVETYPE_NONE; // 0x108 = 0
	pev->solid = SOLID_NOT;        // 0x10c = 0
	UTIL_SetOrigin(pev, pev->origin);
	UTIL_SetSize(pev, g_vecZero, g_vecZero);

	// @0x625b1..0x625df: no think or touch function (+0xc and +0x14, the think and touch members,
	// are zeroed), yet a think 10 s out, which then does nothing. The two saved fields (+0x64,
	// +0x68) are left as they are.
	SetThink(nullptr);
	SetTouch(nullptr);
	pev->nextthink = gpGlobals->time + 10.0f;

	// @0x625d8..0x6262e: the spawnflags go into CTSGameRules. SF 0x20 turns the round timer off
	// (+0x10), SUPERJUMP (SF 0x40) -> +0x5885 (TSInit gives every spawn low-gravity + the "mj"
	// physinfo), and get_weapon_restriction() (InitWeaponRestriction() XOR the spawnflags, the
	// "No <weapon>" bits) becomes both the restriction and the allowed categories the
	// weaponrestriction command masks with (+0x5870/+0x586c). CTSGameRules::Init puts all three
	// back for the next map. The original also zeroes +0x5875 and +0x5874 here; nothing reads the
	// first, and the second (read in RestartRound) is never set, so reTS keeps neither.
	CTSGameRules *pRules = TSGameRules; // @0x625e5: TSGameRules
	pRules->m_bRoundTimerEnabled = (pev->spawnflags & 0x20) == 0;
	pRules->m_bSrvSuperJump = (pev->spawnflags & 0x40) ? 1 : 0;
	pRules->m_iAllowedCategories = pRules->m_iWeaponRestriction = get_weapon_restriction();
}


// Bitmask of permitted weapon types: InitWeaponRestriction() (all five) XOR the "No <weapon>"
// spawnflags. Consumed by the game rules — kept here for faithfulness.
int InitWeaponRestriction(void); // defined in TS_gamerules.cpp (its original home)

int CTSMapGlobals::get_weapon_restriction(void)
{
	return InitWeaponRestriction() ^ pev->spawnflags;
}

// Present in the original but never scheduled by Spawn (an inert 10s
// self-reschedule); kept for completeness.
void CTSMapGlobals::WaitThink(void)
{
	pev->nextthink = gpGlobals->time + 10.0f;
}

// ===================== ts_sellzone.cpp (merged) =====================

class CTSSellZone : public CBaseEntity
{
public:
	bool KeyValue(KeyValueData *pkvd) override;
	void Spawn() override;
	void EXPORT SellTouch(CBaseEntity *pOther);

	// +0x64 — tsobjective attackerteam (-1 = any). Nothing initialises it (sizeof 0x68; GetClassPtr
	// @0x63754 sets only the vptr), so a zone without the key has 0 from the engine's zeroed memory.
	int m_attackerTeam = 0;
};

LINK_ENTITY_TO_CLASS(ts_sellzone, CTSSellZone)

// @0x626c0 KeyValue — attackerteam only; any other key is not handled, with no base call.
bool CTSSellZone::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "attackerteam"))
	{
		m_attackerTeam = atoi(pkvd->szValue);
		return true;
	}
	return false;
}

// @0x62720 Spawn — an invisible brush trigger. The model is set before solid, so the first links are
// SOLID_NOT; MOVETYPE_FLY's physics relinks it as a trigger on its next frame.
void CTSSellZone::Spawn()
{
	pev->classname = MAKE_STRING("ts_sellzone");
	SET_MODEL(ENT(pev), STRING(pev->model));
	UTIL_SetSize(pev, pev->mins, pev->maxs);
	UTIL_SetOrigin(pev, pev->origin);
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_TRIGGER;
	pev->movetype = MOVETYPE_FLY;
	SetTouch(&CTSSellZone::SellTouch);
	SetThink(NULL);
}

// @0x627e8 SellTouch — a dropped gun sells only while CashBased() (@0x62830), which is hard off in the
// shipped game: a sell zone does nothing and the gun stays. When it sells, the gun needs its recorded
// owner (m_status.hOwner, gun+0xbc, written on the buy path), and its dropper (m_pDropper, gun+0xd0) is
// the seller if a player; the briefcase is not sold (@0x628d2); attackerteam is matched against the
// dropper's team. The dropper's cash (+0x540) grows by half the weapon's price and half each attachment's
// buy price (AddItem @0x8440c: silencer 900, laser 800, flashlight 500, scope 1800), written directly:
// no clamp, no TSCash, no print. Then the gun is removed.
void CTSSellZone::SellTouch(CBaseEntity *pOther)
{
	if (!FClassnameIs(pOther->pev, "WorldGun"))
		return;
	if (!CashBased())
		return;

	CTSWorldGun *pGun = (CTSWorldGun *)CBaseEntity::Instance(pOther->pev);
	if (!pGun->m_status.hOwner)
		return;
	if (!pGun->m_pDropper)
		return;
	CBaseEntity *pSeller = CBaseEntity::Instance(pGun->m_pDropper);
	if (!pSeller->IsPlayer())
		return;
	if (pGun->m_weaponType == TSW_FLAG)
		return;
	if (m_attackerTeam != -1 && TSGameRules->GetTeamIndex(pSeller->TeamID()) != m_attackerTeam)
		return;

	CBasePlayer *pPlayer = (CBasePlayer *)pSeller;
	pPlayer->m_iCash += get_weapon_info((tsweapon_t)pGun->m_weaponType)->iPrice / 2;
	if (pGun->m_status.attach & 1)
		pPlayer->m_iCash += 450;
	if (pGun->m_status.attach & 2)
		pPlayer->m_iCash += 400;
	if (pGun->m_status.attach & 4)
		pPlayer->m_iCash += 250;
	if (pGun->m_status.attach & 8)
		pPlayer->m_iCash += 900;

	UTIL_Remove(pGun);
}


// ===================== ts_playeramount.cpp (merged) =====================

LINK_ENTITY_TO_CLASS(ts_playeramount, CTSPlayerAmount)

bool CTSPlayerAmount::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "clients"))          { m_iClients = atoi(pkvd->szValue); return true; }
	if (FStrEq(pkvd->szKeyName, "connecttarget"))    { m_iszConnectTarget = ALLOC_STRING(pkvd->szValue); return true; }
	if (FStrEq(pkvd->szKeyName, "disconnecttarget")) { m_iszDisconnectTarget = ALLOC_STRING(pkvd->szValue); return true; }
	// @0x629c0: these three only, no CBaseDelay chain, so a map's delay or killtarget is ignored
	// (m_flDelay stays 0 and the original's DelayedUse branch never runs).
	return false;
}

void CTSPlayerAmount::Spawn()
{
	pev->classname = MAKE_STRING("ts_playeramount");
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_NOT;
	pev->movetype = MOVETYPE_FLY; // @0x62a78: movetype 5, no touch, no think
	SetTouch(NULL);
	SetThink(NULL);
	m_iCount = 0;
}

// @0x72aec ClientConnect — a client connected: count up; on reaching `clients`, fire connecttarget.
void CTSPlayerAmount::ClientConnect()
{
	if (++m_iCount != m_iClients)
		return;
	if (!FStringNull(m_iszConnectTarget))
	{
		// No activator (@0x62bd8); spawnflag 1 removes the relay once it has fired.
		FireTargets(STRING(m_iszConnectTarget), NULL, this, USE_TOGGLE, 0);
		if (pev->spawnflags & 1) // bit 1 = remove the relay after it fires
			UTIL_Remove(this);
	}
}

// @0x72c08 ClientDisconnect — a client left: count down; on reaching `clients`, fire disconnecttarget.
void CTSPlayerAmount::ClientDisconnect()
{
	if (--m_iCount != m_iClients)
		return;
	if (!FStringNull(m_iszDisconnectTarget))
	{
		FireTargets(STRING(m_iszDisconnectTarget), NULL, this, USE_TOGGLE, 0);
		if (pev->spawnflags & 1)
			UTIL_Remove(this);
	}
}

// ===================== ts_timer.cpp (merged) =====================

#define SF_TIMER_START_OFF 1   // FGD bit 1
#define SF_TIMER_LOOP      2   // FGD bit 2

class CTSTimer : public CBaseEntity
{
public:
	void Spawn(void);
	bool KeyValue(KeyValueData *pkvd);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);
	void EXPORT WaitThink(void);
	void ToggleTimer(void);

	int   m_iDuration;   // this+0x64 — "duration" keyvalue, whole seconds (atoi, @0x62d50)
	int   m_iActive;      // this+0x68 — running toggle state
};

LINK_ENTITY_TO_CLASS(ts_timer, CTSTimer);

bool CTSTimer::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "duration"))
	{
		m_iDuration = atoi(pkvd->szValue);
		return true;
	}
	return CBaseEntity::KeyValue(pkvd);
}

void CTSTimer::Spawn(void)
{
	pev->classname = MAKE_STRING("ts_timer");
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_NOT;     // @0x62deb
	pev->movetype = MOVETYPE_FLY;

	m_iActive = 0;
	SetThink(NULL);

	if (!(pev->spawnflags & SF_TIMER_START_OFF))
		ToggleTimer();
	else
		SetThink(NULL);
}

// @ ToggleTimer__8CTSTimer
void CTSTimer::ToggleTimer(void)
{
	if (m_iDuration < 1)
	{
		SetThink(NULL);
		m_iActive = 0;
		return;
	}

	m_iActive ^= 1;
	if (!m_iActive)
	{
		SetThink(NULL);
	}
	else
	{
		SetThink(&CTSTimer::WaitThink);
		pev->nextthink = gpGlobals->time + m_iDuration;
	}
}

// @ WaitThink__8CTSTimer
void CTSTimer::WaitThink(void)
{
	if (pev->target)
		FireTargets(STRING(pev->target), this, this, USE_TOGGLE, 0);

	if (pev->message)
		TSHudMessageAll(0, 255, 0, 1, 30, STRING(pev->message));

	// @0x62f3a: spawnflag 1 (the FGD's "Start OFF") also makes it fire once; the FGD's "Loop" (2) is
	// not read.
	if (pev->spawnflags & SF_TIMER_START_OFF)
		SetThink(NULL);

	pev->nextthink = gpGlobals->time + m_iDuration;
}

void CTSTimer::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	ToggleTimer();
}

// ===================== ts_customtarget.cpp (merged) =====================

// Original storage: a 0x800-byte block at this+0x64, 0x20 bytes/slot -> 64 slots
// of 32 chars (recovered offsets are provenance, not targets).
#define TS_MAX_TARGETS 64
#define TS_TARGET_LEN  32

// triggermethod values (FGD "fire method" choices)
#define TS_FIRE_ALL    0
#define TS_FIRE_RANDOM 1
#define TS_FIRE_SEQ    2

class CTSCustomTarget : public CBaseEntity
{
public:
	void Spawn(void);
	bool KeyValue(KeyValueData *pkvd);
	void Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value);

	void GetTargets(const char *pTargets);
	void FireTarget(int index);

	char m_szTargets[TS_MAX_TARGETS][TS_TARGET_LEN]; // this+0x64 — parsed target names
	int  m_iNumTargets;                              // this+0x864 — parsed count
	int  m_iTriggerMethod;                           // this+0x868 — 0/1/2
	int  m_iAccumulate;                              // this+0x86c — calls before fire
	int  m_iCurCount;                               // this+0x870 — Use counter
	int  m_iSeqIndex;                               // this+0x874 — sequential cursor
};

LINK_ENTITY_TO_CLASS(ts_customtarget, CTSCustomTarget);

// @ GetTargets__15CTSCustomTargetPc — split a ';'-separated list into m_szTargets.
void CTSCustomTarget::GetTargets(const char *pTargets)
{
	memset(m_szTargets, 0, sizeof(m_szTargets));
	m_iNumTargets = 0;

	if (!pTargets)
		return;

	const char *p = pTargets;
	while (*p && m_iNumTargets < TS_MAX_TARGETS)
	{
		int n = 0;
		while (*p && *p != ';' && n < TS_TARGET_LEN - 1)
			m_szTargets[m_iNumTargets][n++] = *p++;
		m_szTargets[m_iNumTargets][n] = '\0';
		m_iNumTargets++;

		// discard any overflow of an over-long token, then the separator
		while (*p && *p != ';')
			p++;
		if (*p == ';')
			p++;
	}
}

bool CTSCustomTarget::KeyValue(KeyValueData *pkvd)
{
	if (FStrEq(pkvd->szKeyName, "targets"))
	{
		GetTargets(pkvd->szValue);
		return true;
	}
	if (FStrEq(pkvd->szKeyName, "accumulate"))
	{
		m_iAccumulate = atoi(pkvd->szValue);
		return true;
	}
	if (FStrEq(pkvd->szKeyName, "triggermethod"))
	{
		m_iTriggerMethod = atoi(pkvd->szValue);
		return true;
	}
	// "message" -> pev->message, plus targetname/etc.
	return CBaseEntity::KeyValue(pkvd);
}

void CTSCustomTarget::Spawn(void)
{
	pev->classname = MAKE_STRING("ts_customtarget");
	pev->effects |= EF_NODRAW;
	pev->solid = SOLID_NOT;     // @0x630b8
	pev->movetype = MOVETYPE_FLY;

	m_iCurCount = 0;
	m_iSeqIndex = 0;
	// m_iAccumulate is zero-initialised by the engine; the FGD always emits
	// "accumulate" (default 1). With 0, Use still fires every call (== 1).
}

// @ FireTarget__15CTSCustomTargeti — index<0 or out of range fires ALL targets.
void CTSCustomTarget::FireTarget(int index)
{
	if (index < 0 || index >= m_iNumTargets)
	{
		for (int i = 0; i < m_iNumTargets; i++)
			FireTargets(m_szTargets[i], this, this, USE_TOGGLE, 0);
	}
	else
	{
		FireTargets(m_szTargets[index], this, this, USE_TOGGLE, 0);
	}
}

void CTSCustomTarget::Use(CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value)
{
	if (++m_iCurCount < m_iAccumulate)
		return;

	m_iCurCount = 0;

	if (pev->message)
		TSHudMessageAll(0, 255, 0, 1, 30, STRING(pev->message));

	switch (m_iTriggerMethod)
	{
	case TS_FIRE_RANDOM:
		FireTarget(RANDOM_LONG(0, m_iNumTargets - 1));
		break;

	case TS_FIRE_SEQ:
	{
		int idx = m_iSeqIndex++;
		FireTarget(idx);
		if (m_iSeqIndex >= m_iNumTargets)
			m_iSeqIndex = 0;
		break;
	}

	case TS_FIRE_ALL:
	default:
		FireTarget(-1);
		break;
	}
}
