/***
*  ts_player.cpp — RECONSTRUCTED TS player extensions (operate on CBasePlayer).
*  Functional/source-faithful (see CLAUDE.md).
***/
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "ts_gamerules.h" // TS_PWUP_* bit constants
#include "ts_player.h"
#include "ts_utils.h"     // TSHudMessageOne/All
#include "UserMessages.h" // gmsgSpectator (the (idx,flag) spectator-state broadcast)
#include "ts_flag.h"      // CTSFlag::CreateFlag (CTF drop)
#include "ts_powerup.h"   // CTSPowerUp::CreatePowerUp (UpdatePowerUpState's death re-drop)
#include "ts_weapons.h"   // TSPlayerFreeSlots (slot-weight gate)
#include "ts_gun.h"       // GetPlayerTSGun (block-bar for GetMeleeMultiplier)
#include "ts_worldgun.h"  // CTSAmmoPack::CreateAmmoPack (DropArmor)
#include "ts_bomb.h"      // CTSBomb::CutWire (the cutwire_<n> command helper)
#include "game.h"         // friendlyfire cvar (the melee twins' teammate gate)
#include "gamerules.h"    // g_pGameRules->PlayerRelationship
#include "weapons.h"      // ClearMultiDamage/ApplyMultiDamage

extern int gmsgTSHaveFlag; // wire name "TSFlag" (4) — defined in ts_flag.cpp

// The TS rules object (the original's `TSGameRules` @0x1681a0, a bss object constructed at
// static init by _GLOBAL_.I.SlowMotion @0x671b4, the unit that holds SlowMotion). It exists in
// every game mode: InstallGameRules never clears it, so single-player rules read it too.
static CTSGameRules s_TSGameRules;
CTSGameRules *TSGameRules = &s_TSGameRules;

// Kill-flavor globals (@0x104480/81/82 in the original .data) — set around TraceAttack/
// ApplyMultiDamage on a melee/coldcock/headshot kill blow; read by TSGetPointsForFrag +
// PlayerKilled (death-notice weapon string "Kung Fu"/"<weapon> Stock" + #TS_Coldcock HUD)
// + Killed. Wire-visible via the kill feed. RE dossier-melee.md §1.4.
bool glast_closecombat = false;
bool glast_coldcock = false;
bool glast_headshot = false;

// TS powerup user messages — registered centrally in LinkUserMessages (UserMessages.cpp),
// in the original's slot order (see ts_player.h).
int gmsgTSPowerUpTime = 0;  // "PwUpTime" 1 — active countdown (UsePowerUp)
int gmsgTSMotionBlur = 0;   // "TSMBlur"  2 — entindex + 1/2 (UsePowerUp)
int gmsgTSPowerUp = 0;      // "PwUp"     3 — held type (short) + duration (byte) — the held-powerup HUD (UpdateClientData)
int gmsgTSSlowAmt = 0;      // "TSSlowMo" 2 — coord(100*applied slow) — drives the client green slow-mo overlay
int gmsgKFuPower = 0;       // "KFuPower" 1 — the melee stamina bar (CHAR, 0..100); sent ONLY by UpdateMeleeBar
int gmsgTSKickAlign = 0;    // "TSKickAlign" — DEAD in the original: the global exists but is never
                            // registered or sent (star addendum). Kept for symbol parity only.

// TS custom-HUD user messages — registered centrally in LinkUserMessages (UserMessages.cpp).
// Names + fixed sizes are byte-faithful to the original (star_LinkUserMessages.md): TSHealth/
// TSArmor are a single byte, TSCash a short. The original sends these (not the stock Health/
// Battery) from CBasePlayer::UpdateClientData (player.cpp — the send block lives inline there,
// fold-plan G2). GLOBALS like the original's (.data D symbols), defined here (their original
// TS_player.cpp home).
int gmsgTSHealth = 0;
int gmsgTSArmor = 0;
int gmsgTSCash = 0;
int gmsgTSState = 0;      // "TSState" 1 — posture byte 0x498 (star_TSState_TSSpace_TSBTime.md)
int gmsgTSSpace = 0;      // "TSSpace" 1 — free carry slots (int 0x548)
int gmsgTSBulletTime = 0; // "TSBTime" 1 — the bullet-time variant flag (byte 0x175 = m_bSlowBullets)
int gmsgTSPlayerAward = 0; // "TSPAward" 2 — MSG_ALL: entindex + award byte 0x74c (star_TSPAward.md)
int gmsgTSWires = 0;       // "Wires" 3 — the nearby-bomb wire triple, MSG_ONE on change (star_Wires.md)

// (TSUpdateClientData DELETED, fold-plan G2: the original has no such method — the TS
// per-frame HUD send block IS part of CBasePlayer::UpdateClientData @0x81d00 and now lives
// inline there, player.cpp.)

// @0x82f08 CheckForWires — clear the wire triple, then sphere-64 scan for an ACTIVE ts_bomb
// this player CanInteract with; the LAST match in the sphere wins (no break). The copied bytes
// are the bomb's wire ROLE codes (0 cut / 1 defuse / 2 boobytrap / 3 intact) — SetRandomWires
// deals {3,3,3} then one 1 and one 2. Caller: PreThink, mode-3 maps only, throttled 0.5 s.
void CBasePlayer::CheckForWires()
{
	m_iWires[0] = m_iWires[1] = m_iWires[2] = 0;
	CBaseEntity *e = nullptr;
	while ((e = UTIL_FindEntityInSphere(e, pev->origin, 64.0f)) != nullptr)
	{
		if (!FClassnameIs(e->pev, "ts_bomb"))
			continue;
		CTSBomb *pBomb = (CTSBomb *)e;
		if (!pBomb->m_bActive || !pBomb->CanInteract(this))
			continue;
		m_iWires[0] = (byte)pBomb->m_wires[0];
		m_iWires[1] = (byte)pBomb->m_wires[1];
		m_iWires[2] = (byte)pBomb->m_wires[2];
	}
}

// @ SlowMotion__FP11CBasePlayer
bool SlowMotion(CBasePlayer *pPlayer)
{
	return (pPlayer->m_iPowerups & TS_PWUP_SLOWMO) != 0;
}

// @ InfiniteClips__FP11CBasePlayer
bool InfiniteClips(CBasePlayer *pPlayer)
{
	return (pPlayer->m_iPowerups & TS_PWUP_INFCLIP) != 0;
}

// @ SlowPause__FP11CBasePlayer
bool SlowPause(CBasePlayer *pPlayer)
{
	return (pPlayer->m_iPowerups & TS_PWUP_SLOWPAUSE) != 0;
}

// @ DoubleFireRate__FP11CBasePlayer
bool DoubleFireRate(CBasePlayer *pPlayer)
{
	return (pPlayer->m_iPowerups & TS_PWUP_DBLFIRE) != 0;
}

// @ KungFu__FP11CBasePlayer
int KungFu(CBasePlayer *pPlayer)
{
	return pPlayer->m_iKungFu;
}

// ---------------------------------------------------------------------------
// Powerup activation state machine (the held timed powerups: slow-motion,
// slow-pause, double-firerate). Reconstructed from CBasePlayer::UsePowerUp
// (@0x85980), UpdatePowerUpState (@0x85748) and DeactivateSlowMotionFlow
// (@0x86a9c). The held powerup (set by PlayerAddPowerUp on pickup) becomes
// *active* on the `usepowerup` command, runs for its duration, then expires.
//
// The rates reach movement through pev->fuser1, which UpdatePowerUpState eases toward
// m_flSlowMotionGo and pm_shared scales the player's frametime by (PM_PlayerMove @0xf08d8); a slowed
// grenade, knife or dropped gun keeps its rate in its own fuser1 (CBaseEntity::GoSlow) and its
// physics reads it there (CTSPhysicObject).
// ---------------------------------------------------------------------------

// @ GoSlow__11CBasePlayerfc — apply a time-scale to this player (1 = normal speed).
void CBasePlayer::GoSlow(float scale, int slowBullets)
{
	// @0x829d0: an observer (iuser1 set) keeps his own rate at 1; the target is set either way
	m_flSlowMotionGo = (pev->iuser1 == 0) ? scale : 1.0f;
	m_flSlowMotionTarget = scale;
	m_bSlowBullets = slowBullets != 0;
}

// The slow-motion rate a player's dropped or thrown object starts with, which each factory hands to the
// object's GoSlow (CTSWorldGun::CreateWeapon @0x90896, CTSKnife::CreateKnife @0x6daa3,
// CM61Grenade::CreateGrenade @0x6c91b, CTSFlag::CreateFlag @0x8d902, the same inlined test in each):
// while his own slow powerup runs, the rate he slows others to (m_flSlowMotionRamp, player+0x180);
// otherwise his own rate (GiveSlowMul, fuser1 floored at 0.01).
float TSThrowSlow(CBasePlayer *pPlayer)
{
	return (pPlayer->m_flSlowMotionRamp != 0.0f) ? pPlayer->m_flSlowMotionRamp : pPlayer->GiveSlowMul();
}

// @ InSlow__11CBasePlayer — is this player currently slowed?
int CBasePlayer::InSlow()
{
	return (m_flSlowMotionGo != 1.0f || m_flSlowMotionTarget != 1.0f) ? 1 : 0;
}

// @ DeactivateSlowMotionFlow__11CBasePlayer (@0x86a9c) — slow-motion ended: restore THIS player to
// normal speed and restore every player it was slowing (unless they're now slowing themselves), then
// clear the cache.
void CBasePlayer::DeactivateSlowMotionFlow()
{
	GoSlow(1.0f, 0);
	m_flSlowMotionRamp = 0;
	m_flSlowMotionScale = 0;
	m_bSlowBullets = 0;
	for (int i = 0; i < 32; i++)
	{
		CBaseEntity *e = m_hSlowPlayers[i];
		if (!e)
			continue;
		CBasePlayer *p = (CBasePlayer *)e;
		if (p->m_flSlowMotionScale == 0.0f) // they're not self-slowing → restore them
		{
			p->GoSlow(1.0f, 0);
			p->m_bSlowBullets = 0;
		}
		m_hSlowPlayers[i] = NULL;
	}
	// @0x86b55: then every grenade, knife and dropped gun the bubble slowed goes back to 1 and the list
	// is emptied.
	for (int i = 0; i < m_iSlowedEntCount; i++)
	{
		if (m_pSlowedEnts[i])
		{
			m_pSlowedEnts[i]->GoSlow(1.0f, 0);
			m_pSlowedEnts[i] = NULL;
		}
	}
	m_iSlowedEntCount = 0;
}

// @0x86ea4: the projectile pass of SlowMotionFlow, for one classname. A grenade is slowed whether it
// moves or not, a knife or a dropped gun only while it moves. Each one the player's eye sees that is
// not on his list yet is slowed to the rate he slows others to and added, while the list has room.
// The search stops at an entity with no edict or the world's.
static void TSSlowProjectiles(CBasePlayer *pPlayer, const char *pszClassname, bool bMovingOnly)
{
	CBaseEntity *pEnt = NULL;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, pszClassname)) != NULL)
	{
		if (!pEnt->pev->pContainingEntity || ENTINDEX(pEnt->pev->pContainingEntity) == 0)
			break;
		if (bMovingOnly && pEnt->pev->velocity == g_vecZero)
			continue;
		if (UTIL_EntInArray(pPlayer->m_pSlowedEnts, 32, pEnt) != -1)
			continue;
		TraceResult tr;
		UTIL_TraceLine(pPlayer->pev->origin + pPlayer->pev->view_ofs, pEnt->pev->origin, ignore_monsters,
			ignore_glass, pPlayer->edict(), &tr);
		if (tr.flFraction < 1.0f)
			continue;
		pEnt->GoSlow(pPlayer->m_flSlowMotionRamp, 0);
		if (pPlayer->m_iSlowedEntCount < 32)
			pPlayer->m_pSlowedEnts[pPlayer->m_iSlowedEntCount++] = pEnt;
	}
}

// @ SlowMotionFlow__11CBasePlayer (@0x86bb0) — runs every frame from PreThink; self-gated to fire
// every 0.5s while this player has an active slow powerup (m_flSlowMotionScale != 0). It slows THIS
// player to the self-scale (0.6 slowmo / 0.8 slowpause) and every LOS-visible OTHER player to the
// others-scale (0.35 / 0.225) — so the activator outruns the slowed world. A stronger slow wins ties.
// The slowed players are cached so DeactivateSlowMotionFlow can restore them. Then the grenades,
// knives and dropped guns in sight are slowed to the others-scale too (TSSlowProjectiles).
void CBasePlayer::SlowMotionFlow()
{
	if (m_flSlowMotionScale == 0.0f || m_flNextSlowFlow > gpGlobals->time)
		return;
	m_flNextSlowFlow = gpGlobals->time + 0.5f;

	Vector myEye = pev->origin + pev->view_ofs;
	bool overridden = false;

	// Pass 1: decide which other players I slow (line of sight), honoring slow-tier conflicts.
	for (int i = 1; i <= gpGlobals->maxClients && !overridden; i++)
	{
		CBaseEntity *cached = m_hSlowPlayers[i - 1];
		if (cached)
		{
			if (((CBasePlayer *)cached)->m_flSlowMotionScale == 0.0f)
				continue;            // still just a victim → keep it cached
			m_hSlowPlayers[i - 1] = NULL; // they started self-slowing → drop & re-resolve
		}
		CBasePlayer *other = (CBasePlayer *)UTIL_PlayerByIndex(i);
		if (!other || !other->IsPlayer() || other == this)
			continue;
		if (other->m_flSlowMotionScale != 0.0f && other->m_flSlowMotionRamp == m_flSlowMotionRamp)
			continue;                // already slowed at my tier by someone
		Vector otherEye = other->pev->origin + other->pev->view_ofs;
		TraceResult tr;
		UTIL_TraceLine(myEye, otherEye, ignore_monsters, ignore_glass, edict(), &tr);
		if (tr.flFraction >= 1.0f)   // clear line of sight
		{
			if (other->m_flSlowMotionScale == 0.0f)
				m_hSlowPlayers[i - 1] = other;          // not slowing themselves → I slow them
			else if (m_flSlowMotionRamp <= other->m_flSlowMotionRamp)
				m_hSlowPlayers[i - 1] = other;          // my slow is equal/stronger → I slow them
			else
				overridden = true;                       // they're slowing harder → abort this tick
		}
	}
	if (overridden)
		return;

	// Apply: slow self, then the cached others to the others-scale.
	GoSlow(m_flSlowMotionScale, m_bSlowBullets);
	for (int i = 0; i < gpGlobals->maxClients && i < 32; i++)
	{
		CBaseEntity *e = m_hSlowPlayers[i];
		if (!e)
			continue;
		CBasePlayer *p = (CBasePlayer *)e;
		if (p->m_flSlowMotionScale == 0.0f || p->m_flSlowMotionRamp != m_flSlowMotionRamp)
		{
			p->GoSlow(m_flSlowMotionRamp, 0);
			p->m_bSlowBullets = m_bSlowBullets;
		}
		else
			m_hSlowPlayers[i] = NULL;
	}

	// @0x86ea4: the projectiles, while the list has room
	if (m_iSlowedEntCount < 32)
	{
		TSSlowProjectiles(this, "grenade", false);
		TSSlowProjectiles(this, "knife", true);
		TSSlowProjectiles(this, "WorldGun", true);
		TSSlowProjectiles(this, "rpg_rocket", true); // reTS's tank rocket (func_tank.cpp), not TS 3.0's
	}
}

// @ UsePowerUp__11CBasePlayer (@0x85980) — the `usepowerup` command: ACTIVATE the held powerup.
// The double-tap +use that issues this command is detected CLIENT-side (cl_dll/input.cpp CreateMove,
// gated by cl_usepowerup, 0.30s window). Activation just promotes held→active + sets the expire time
// + notifies the client; the actual slowing happens in UpdatePowerUpState (sets the slow triple) +
// SlowMotionFlow (propagates to LOS enemies) each frame. Returns true if it took.
bool CBasePlayer::UsePowerUp()
{
	if (pev->iuser1 != 0 || !IsAlive())   // spectating/frozen, or dead
		return false;
	if (m_iHeldPowerup == 0 || m_iPowerups != 0)
		return false; // nothing held, or one is already active

	// @0x859df: the original refuses a slow powerup with "You can't use Slow-Motion now!" while a rules
	// flag (rules+0x18) is set, but CTSGameRules::Init clears it (@0x67e22) and nothing sets it, so a
	// slow powerup is never refused; reTS keeps no such flag.

	m_iPowerups = m_iHeldPowerup;
	m_flPowerupExpireTime = (float)m_iPowerupTime + gpGlobals->time;

	// PwUpTime — the green countdown the client ticks down locally (whole seconds remaining).
	MESSAGE_BEGIN(MSG_ONE, gmsgTSPowerUpTime, NULL, ENT(pev));
	WRITE_BYTE((int)(m_flPowerupExpireTime - gpGlobals->time + 0.5f));
	MESSAGE_END();

	// TSMBlur — kick off the client motion-blur (1 = slow-motion, 2 = slow-pause). Only the two
	// slow powerups send it (double-firerate doesn't). MSG_PVS so nearby players see the wash too.
	int blur = (m_iHeldPowerup & TS_PWUP_SLOWMO) ? 1 : (m_iHeldPowerup & TS_PWUP_SLOWPAUSE) ? 2 : 0;
	if (blur)
	{
		MESSAGE_BEGIN(MSG_PVS, gmsgTSMotionBlur, pev->origin);
		WRITE_BYTE(ENTINDEX(edict()));
		WRITE_BYTE(blur);
		MESSAGE_END();
	}
	return true;
}

// A dead player's powerup goes back into the world where he lies (UpdatePowerUpState @0x858a5,
// @0x85928): CreatePowerUp(type, -1) with his origin, unless the game mode is past 1 (rules+0, the
// objective modes) or it is a slow motion of 2 s or less (m_iPowerupTime, the int at player+0x730).
static void TSRedropPowerup(CBasePlayer *pPlayer, int type)
{
	if (TSGameRules->m_iGameMode > 1)
		return;
	if ((type & TS_PWUP_SLOWMO) && pPlayer->m_iPowerupTime <= 2)
		return;
	CTSPowerUp *pDrop = CTSPowerUp::CreatePowerUp(type, -1);
	if (pDrop)
		pDrop->pev->origin = pPlayer->pev->origin;
}

// @ UpdatePowerUpState__11CBasePlayer (@0x85748) — per-frame (PreThink): apply the active
// powerup's slow-motion fields or superjump gravity, end it when its time runs out or the player
// dies, and give a dead player's powerup back to the world.
void CBasePlayer::UpdatePowerUpState()
{
	if (m_iHeldPowerup == 0)
		return;

	if (m_iPowerups == 0)
	{
		// Held but not activated: kept while alive; dropped where he dies (@0x858e4..0x85933).
		if (IsAlive())
			return;
		TSRedropPowerup(this, m_iHeldPowerup);
	}
	else
	{
		if (m_iPowerups & (TS_PWUP_SLOWMO | TS_PWUP_SLOWPAUSE))
		{
			if (m_flPowerupExpireTime < gpGlobals->time || !IsAlive())
			{
				DeactivateSlowMotionFlow();
			}
			else if (m_iPowerups & TS_PWUP_SLOWPAUSE)
			{
				// slow-pause (@0x857d0): the bullets go slower (m_bSlowBullets -> GetBulletVelocityMult's x10
				// denominator), others are slowed to 0.225 and the player himself to 0.8, less than slow
				// motion's 0.6, so he moves and fires faster than under slow motion. Measured 0.8 on TS 3.0
				// (amxxbench tsx_player, ts30 copy).
				m_bSlowBullets = 1;
				m_flSlowMotionRamp = 0.225f;
				m_flSlowMotionScale = 0.8f;
			}
			else if (m_iPowerups & TS_PWUP_SLOWMO)
			{
				// slow-motion (@0x857fc)
				m_bSlowBullets = 0;
				m_flSlowMotionRamp = 0.35f;
				m_flSlowMotionScale = 0.6f;
			}
		}
		else if (m_iPowerups & TS_PWUP_LOWGRAV)
		{
			// a running superjump powerup (@0x85825): low gravity every frame. No pickup runs one (a
			// picked-up superjump is the permanent m_bLowGravity); a plugin can.
			pev->gravity = 0.15f;
		}

		if (gpGlobals->time <= m_flPowerupExpireTime && IsAlive())
			return; // still active
		// expired, or he died with it running (@0x85861..0x858d7)
		if (!IsAlive())
			TSRedropPowerup(this, m_iPowerups);
		if (m_iPowerups & TS_PWUP_LOWGRAV)
			pev->gravity = 1.0f;
	}

	m_flPowerupExpireTime = 0;
	m_iPowerupTime = 0;
	m_iPowerups = 0;
	m_iHeldPowerup = 0;
}

// ---------------------------------------------------------------------------
// CTF flag carry (CBasePlayer::TakeFlag / DropFlag). The carry state is the 28-byte
// s_flag_status copied inline onto the player (m_flagStatus); the loose flag entity is
// CTSFlag (ts_flag.cpp). Carrying costs 20 slot-weight (heavier → slower, via the slot
// speed helper) and a flag can only be carried one at a time.
// ---------------------------------------------------------------------------

// @0x83830 TakeFlag — copy a flag's status onto this player (the pickup). Refuses if the player is
// over the slot-weight budget or is already carrying a flag. Returns true if taken.
bool CBasePlayer::TakeFlag(s_flag_status *src)
{
	if (TSPlayerFreeSlots(this) < 20 || m_flagStatus.active)
		return false; // not enough free slots, or already carrying

	m_flagStatus = *src;     // copy the full 28-byte status (incl. the source lastCarrier)
	m_flagStatus.active = 1; // now carried (slot-weight debit is folded into GetSpeedBySlots)
	SetPlayerSpeed(GetSpeedBySlots(this));
	EMIT_SOUND(ENT(pev), CHAN_ITEM, "items/gunpickup2.wav", VOL_NORM, ATTN_NORM);

	MESSAGE_BEGIN(MSG_ALL, gmsgTSHaveFlag);
	WRITE_BYTE(1); // type 1 = pickup
	WRITE_BYTE(m_flagStatus.flagID);
	WRITE_BYTE(m_flagStatus.team + 1);
	WRITE_BYTE(ENTINDEX(edict()));
	MESSAGE_END();

	// @0x838f8: everyone is told only when the carrier is new; the log line comes every time
	const int idx = ENTINDEX(edict());
	if (m_flagStatus.lastCarrier != idx)
		TSHudMessageAll(0, 0xff, 0, 4, 30,
			UTIL_VarArgs("%s picked up a briefcase containing %s!", STRING(pev->netname), STRING(m_flagStatus.contents)));
	m_flagStatus.lastCarrier = idx;
	UTIL_LogPrintf("\"%s<%i><%s><%i>\" triggered \"aquired_briefcase\" (contents \"%s\")\n",
		STRING(pev->netname), GETPLAYERUSERID(edict()), GETPLAYERAUTHID(edict()), GETPLAYERUSERID(edict()),
		STRING(m_flagStatus.contents));
	return true;
}

// @0x8334c DropFlag — give up the carried flag. bSilent (capture) just clears the carrier; a normal
// drop (death/disconnect/spectate/DropWeapon) spawns a fresh loose CTSFlag tossed from the player.
void CBasePlayer::DropFlag(bool bSilent)
{
	if (!m_flagStatus.active)
		return;

	if (!bSilent)
	{
		// @0x833a9: tossed from the eyes 16 units ahead: a dead carrier's in a random direction
		// (whole degrees) at 60..160, a living one's along his view at twice (90 - pitch), lifted by a
		// third of that, plus 0.9 of his own velocity; unturned, spinning
		CTSFlag *pFlag = CTSFlag::CreateFlag(this, &m_flagStatus);
		Vector angles;
		float push;
		if (!IsAlive())
		{
			angles = Vector(RANDOM_LONG(-60, 60), RANDOM_LONG(-180, 180), 0);
			push = RANDOM_LONG(60, 160);
		}
		else
		{
			angles = pev->v_angle;
			push = (90.0f - angles.x) + (90.0f - angles.x);
		}
		UTIL_MakeVectors(angles);
		if (pFlag)
		{
			pFlag->pev->origin = pev->origin + pev->view_ofs + gpGlobals->v_forward * 16.0f;
			pFlag->pev->angles = g_vecZero;
			pFlag->pev->avelocity.y = RANDOM_LONG(-300, 300);
			pFlag->pev->avelocity.z = RANDOM_LONG(-300, 300);
			pFlag->pev->velocity = pev->velocity * 0.9f + gpGlobals->v_forward * push + Vector(0, 0, push * 0.33f);
		}

		UTIL_LogPrintf("\"%s<%i><%s><%i>\" triggered \"dropped_briefcase\" (contents \"%s\")\n",
			STRING(pev->netname), GETPLAYERUSERID(edict()), GETPLAYERAUTHID(edict()), GETPLAYERUSERID(edict()),
			STRING(m_flagStatus.contents));

		MESSAGE_BEGIN(MSG_ALL, gmsgTSHaveFlag);
		WRITE_BYTE(2); // type 2 = drop
		WRITE_BYTE(m_flagStatus.flagID);
		WRITE_BYTE(m_flagStatus.team + 1);
		WRITE_BYTE(ENTINDEX(edict()));
		MESSAGE_END();
	}

	m_flagStatus = s_flag_status(); // clear the carried state (active = 0)
	// the original refunds the 20 slots and sets the speed first thing (@0x8338d); our slot count
	// reads m_flagStatus.active, so the speed follows the clear
	SetPlayerSpeed(GetSpeedBySlots(this));
}

// @0x83068 SetPlayerSpeed — clamp the requested base speed to [0, 330] and push it to the engine
// (client prediction) via pfnSetClientMaxspeed. Stores the clamped value.
void CBasePlayer::SetPlayerSpeed(float speed)
{
	if (speed > 330.0f)
		speed = 330.0f;
	if (speed < 0.0f)
		speed = 0.0f;
	m_flSetMaxSpeed = speed;
	g_engfuncs.pfnSetClientMaxspeed(edict(), (float)(int)(m_flSetMaxSpeed + 0.5f));
}

// @0x85b70 SetPlayerGravity — set the player's gravity, but only when they're at normal gravity
// (>= 1.0) so an already-applied low-gravity effect isn't stomped. setval arms the gravity timer
// (+0x4ec), which PreThink counts down and ends with gravity 1.0.
void CBasePlayer::SetPlayerGravity(float setval, float gravity)
{
	if (pev->gravity >= 1.0f)
	{
		pev->gravity = gravity;
		m_flGravityValue = setval;
	}
}

// @0x85f70 PunchScreen — a no-op in the shipped game (the screen-punch effect was disabled).
void CBasePlayer::PunchScreen(float magnitude, Vector dir)
{
}

// @0x856e0 TSUse — the server-side double-tap +use detector: a second +use within 0.3s activates the
// held powerup (UsePowerUp), then re-arms the window. NB: our build detects the double-tap CLIENT-side
// (cl_usepowerup -> the `usepowerup` command), so this is NOT wired into PreThink to avoid a
// double-activation; kept for source parity / as the server-side alternative.
void CBasePlayer::TSUse()
{
	if (pev->iuser1 == 0 && (m_afButtonPressed & IN_USE))
	{
		if (gpGlobals->time <= m_flUseDoubleTap)
			UsePowerUp();
		m_flUseDoubleTap = gpGlobals->time + 0.3f;
	}
}

// @0x82a08 UpdatePlayerState — derive the pose state: 2 = stunt/locked (an iuser4 lock bit set),
// 1 = ducking (FL_DUCKING = 0x4000), 0 = normal.
void CBasePlayer::UpdatePlayerState()
{
	if (pev->iuser4 & 0x30)
		m_iPlayerState = 2;
	else if (pev->flags & FL_DUCKING)
		m_iPlayerState = 1;
	else
		m_iPlayerState = 0;
}

// @ PickUpWeapon__11CBasePlayer10tsweapon_tPvibT4 (@0x8314c, decompile-exact) — the ONE gate
// every weapon acquisition goes through. Callers + args recovered from the call sites:
//   CTSWorldGun::GetWeapon   (type, &status, 0,      type != FLAG, false)  @0x91224
//   CTSGroundWeapon::GetWeapon(type, NULL,   attach,  type != FLAG, false)  @0x924ff
//   CTSKnife::Use            (type, NULL,    0,       false,        false)  @0x6de93 — NO switch!
//   CBasePlayer::BuyWeapon   (type, NULL,    attach,  true,         true)   @0x846ca — the sole owner-record
//   PlayerAddPowerUp / give  (M61,  NULL,    0,       true,         false)  @0x87d4e/@0x9dbdf
// The int param is the BOUGHT ATTACHMENT MASK (silencer 1/laser 2/flash 4/scope 8 — BuyWeapon
// prices exactly those bits), passed through to TakeWeapon — NOT a clip. Flow: refuse a direct
// akimbo-class pickup (its SINGLE merges instead); the slot gate (a carried STACK charges
// slotCost per unit); TakeWeapon (stack merge / akimbo combine / fresh install with the carried
// status); record the owner EHANDLE(s) if asked; speed recompute; gunpickup2 (CHAN_ITEM,
// attn 0.8); select the picked type — or its AKIMBO if that is owned — when asked OR bare-handed.
bool CBasePlayer::PickUpWeapon(int type, TSItem *pStatus, int iAttach, bool bSelect, bool bRecordOwner)
{
	CTSGun *pGun = GetPlayerTSGun(this); // orig: this->m_pActiveItem gated on m_iId == 0x10
	if (!pGun || type < 0 || type >= TSW_COUNT)
		return false;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)type);
	if (wi->iFireType == TSFT_AKIMBO && wi->iSingleType != -1)
		return false; // @0x83197 — pick the single up; it merges into the akimbo

	// the slot gate (@0x831bd): a dropped STACK charges slotCost per carried unit
	float weight = (float)wi->slotCost;
	if (wi->iStackable && pStatus && pStatus->clip > 0)
		weight *= (float)pStatus->clip;
	if ((float)TSPlayerFreeSlots(this) - weight < 0.0f)
		return false;

	if (!pGun->TakeWeapon((tsweapon_t)type, pStatus, iAttach))
		return false;

	if (bRecordOwner)
	{
		// item+0x30 (@0x83234): the buyer is recorded on the item — and on its akimbo pair
		// when that is owned; PlayerKilled's own-weapon kill reads it (@0x79716)
		pGun->m_items[type].hOwner = this;
		if (wi->iAkimboType != -1 && pGun->IsOwned(wi->iAkimboType))
			pGun->m_items[wi->iAkimboType].hOwner = this;
	}

	SetPlayerSpeed(GetSpeedBySlots(this));
	EMIT_SOUND_DYN(ENT(pev), CHAN_ITEM, "items/gunpickup2.wav", VOL_NORM, 0.8f, 0, PITCH_NORM);

	if (bSelect || !pGun->m_pActiveItem)
	{
		int sel = type;
		if (wi->iAkimboType != -1 && pGun->IsOwned(wi->iAkimboType))
			sel = wi->iAkimboType; // picking the single while owning the pair wields the PAIR
		pGun->SelectWeapon(sel);   // ours deploys inside (the original SelectWeapon+Deploy)
	}
	return true;
}

// @ TSInit__11CBasePlayer (@0x82a4c, decompile-exact) — the TS per-life reset, the FIRST call in
// the original's Spawn (@0x7f586). Everything TS about a player resets here: the anim/stunt
// machines, slow-motion, posture + the client change-detect caches (0xff = force the first
// resend), speed, SPAWN PROTECTION (a 1-second grace: the renderfx shimmer + the (200,200,250)
// screen fade + the m_flSpawnProtectTime deadline that TraceAttack, the melee disarm twins and
// PreThink's shimmer-clear all consume, @0x801eb/@0x81458), powerups, the map-global superjump
// mode, the spectate clear, the whole melee/whiff/block/wires/flag block, and the anim state.
void CBasePlayer::TSInit()
{
	m_TSAnim.Init();      // player+0x28c
	m_TSStunt.Init(this); // player+0x4f0
	pev->fuser1 = 1.0f;
	m_flSlowMotionGo = 1.0f;
	m_flSlowMotionTarget = 1.0f;
	m_flClientSlowAmt = -1.0f;
	m_bSlowBullets = 0;         // 0x175
	m_iClientBulletTime = 0xff; // 0x174 — force the first TSBTime send
	m_iPlayerState = 0;         // 0x498
	m_iClientTSState = 0xff;    // 0x499 — force the first TSState send
	m_iClientTSHealth = -1;     // 0x9e0 (@0x82ac9) — force the first TSHealth send
	pev->iuser4 = 0;
	SetPlayerSpeed(330.0f);
	// free-slot counter reset (the original's player+0x548 = 81; ours derives from the arsenal, so
	// only a plugin's adjustment is dropped)
	m_iFreeSlotsAdjust = 0;
	m_iClientTSSpace = -1; // 0x54c — force the first TSSpace send
	m_iClientTSCash = -1;  // 0x544 (@0x82b02) — force the first TSCash send
	m_flNextClientUpdate = 0; // 0x534 (@0x82b0c): the next UpdateClientData runs at once
	m_flSpawnProtectTime = gpGlobals->time + 1.0f; // 0x71c — the SPAWN-PROTECTION deadline
	m_flNextAttack = UTIL_WeaponTimeBase() + 1.0f; // 0x12c (the stock Spawn tail re-arms it later)
	// the spawn-protection shimmer — PreThink clears it once the deadline passes (@0x81458)
	pev->renderamt = 125.0f;
	pev->rendermode = kRenderTransColor;
	pev->rendercolor = Vector(0, 0, 0);
	pev->renderfx = kRenderFxNoDissipation; // 0xe (@0x82b83)
	UTIL_ScreenFade(this, Vector(200, 200, 250), 0.2f, m_flSpawnProtectTime - gpGlobals->time, 25, 0);
	m_flUseDoubleTap = 0; // 0x164
	m_iClientActiveItems = 0; // 0x720 (@0x82bd7) — the ActItems cache
	m_iPowerups = 0;
	m_iHeldPowerup = 0;
	m_flPowerupExpireTime = 0;
	m_iPowerupTime = 0;
	m_iClientHeldDur = 0;
	m_iClientHeldType = 0xff;
	m_iTSInit740 = 0;      // 0x740 (@0x82c1a)
	m_iKillStreak = 0;     // 0x744 (@0x82c24): a streak lasts until he spawns again, not until he dies
	m_flLastKillTime = 0;  // 0x748 (@0x82c2b)
	m_iAward = 0;          // 0x74c
	m_iClientAward = 0xff; // 0x74d — force the first TSPAward send
	m_iKungFu = 0;         // 0x74e
	// the map-global superjump mode (rules+0x5885, ts_mapglobals SF & 0x40): everyone spawns
	// with the low-gravity/superjump physinfo ("mj")
	CTSGameRules *pRules = TSGameRules; // @0x82c53: TSGameRules+0x5885
	if (pRules->m_bSrvSuperJump)
	{
		m_bLowGravity = 1;
		g_engfuncs.pfnSetPhysicsKeyValue(edict(), "mj", "1");
	}
	else
	{
		m_bLowGravity = 0;
		g_engfuncs.pfnSetPhysicsKeyValue(edict(), "mj", "0");
	}
	m_flNextPTakeDamTime = 0; // 0x750 (@0x82ca8): the PTakeDam 0.3 s throttle
	m_flSpecMsgTime = 0; // +0x754 — the spectate HUD-line 1 Hz throttle clock
	if (pev->iuser1 != 0 || pev->iuser2 != 0)
	{
		// @0x82a4c: the stock-shaped spectator-state broadcast — (BYTE playerIndex, BYTE 0).
		// (Wire reconciled cont.84: Spectator carries (idx, flag) everywhere; the spectate-wait
		// countdown rides TSHudMessageOne instead.)
		MESSAGE_BEGIN(MSG_ALL, gmsgSpectator);
		WRITE_BYTE(ENTINDEX(edict()));
		WRITE_BYTE(0);
		MESSAGE_END();
		pev->iuser1 = 0;
		pev->iuser2 = 0;
	}
	m_flBlockTime = 0;     // 0x4dc
	m_flGravityValue = 0;  // 0x4ec
	m_bHasKevlar = false;  // 0x718
	m_flTSRespawnTime = 0; // 0x770
	m_flRespawnHoldTime = 0; // 0x7bc (@0x82d5b): a re-entry window ends with the spawn that used it
	m_iTSInit788 = 0;        // 0x788 (@0x82d65)
	m_hRunThroughLast = NULL; // 0x798
	m_iWires[0] = m_iWires[1] = m_iWires[2] = 0;                   // 0x78c-0x78e
	m_iClientWires[0] = m_iClientWires[1] = m_iClientWires[2] = 0; // 0x78f-0x791
	m_flNextWiresCheck = 0; // 0x794
	m_flMeleeBar = 100.0f;  // 0x774 (0x42c80000)
	m_flWhiffLungeTime = 0;  // 0x79c
	m_flWhiffLungeForce = 0; // 0x7a0
	m_iWhiffLungeType = 0;   // 0x7a4
	m_flKungFuBlock = 0;     // 0x7a8
	m_flKnifeBlock = 0;      // 0x7ac
	m_flKatanaBlock = 0;     // 0x7b0
	m_flDisarmPressure = 0;  // 0x7b4
	if (m_bTheOne && pRules)
		pRules->m_bTheOneActive = false; // rules+0x5878 — the One left the field
	m_bTheOne = 0; // 0x7b9
	m_flHealthRegen = 0;
	memset(m_iTSInit184, 0, sizeof(m_iTSInit184)); // 0x184[33] (@0x82e21)
	m_flSlowMotionScale = 0;
	m_flSlowMotionRamp = 0;
	m_flNextSlowFlow = 0;
	memset(m_pSlowedEnts, 0, sizeof(m_pSlowedEnts)); // 0x208[32] — the packed slowed array
	m_iSlowedEntCount = 0;                           // 0x288
	m_iTSInit77c[0] = m_iTSInit77c[1] = m_iTSInit77c[2] = 0; // 0x77c/0x780/0x784 (@0x82e66)
	m_flSpreadAccum = 0;
	m_flNextChatTime = 0; // 0x7c0 (@0x82e8e): Host_Say's chat clock (@0x9d09c, @0x9d826)
	memset(&m_flagStatus, 0, sizeof(m_flagStatus)); // 0x7d0
	PlayerIsCheating(this, g_engfuncs.pfnGetInfoKeyBuffer(edict()));
	pev->gaitsequence = 0;
	pev->sequence = 0;
	pev->frame = 0;
	ResetSequenceInfo();
	m_fNextSuicideTime = 0; // 0x84c — cancel a pending `kill` (@0x82ef3)
}

// @0x865d0 GetMeleeMultiplier — melee damage scales with the block-bar stamina: full (100) -> 1.0,
// empty (0) -> 0.5. (The block bar is field_0x774 on the original player; we keep it on the TS gun.)
float CBasePlayer::GetMeleeMultiplier()
{
	float bar = m_flMeleeBar; // the bar lives on the PLAYER (+0x774), like the original
	if (bar < 0.0f)
		bar = 0.0f;
	return (bar * 0.5f + 50.0f) * 0.01f;
}

// @0x8650c UpdateMeleeBar — THE melee-stamina drain choke-point (the only writer besides the
// PreThink regen + TSInit refill): subtract, floor at 0 with a 1.5s global attack lockout on
// depletion, then ALWAYS network the bar (KFuPower MSG_ONE CHAR — one send per call, no
// change-detect). Callers: CCombatBlock(33) raise-block, Victim*Hold(attacker, 33) got-blocked,
// the jump-kick (dmg-scaled) and the GoWheel backkick (22.5). No upper clamp here.
void CBasePlayer::UpdateMeleeBar(float flAmount)
{
	m_flMeleeBar -= flAmount;
	if (m_flMeleeBar < 0.01f)
	{
		m_flMeleeBar = 0.0f;
		m_flNextAttack = 1.5f; // depleted: 1.5s global attack lockout (raw store; CLIENT_WEAPONS base 0)
	}
	MESSAGE_BEGIN(MSG_ONE, gmsgKFuPower, NULL, pev);
	WRITE_CHAR((int)(m_flMeleeBar + 0.5f));
	MESSAGE_END();
}

// @0x85b58 BlockPlayer — a movement SELF-ROOT (NOT the deflect-block; 24 bytes in the original):
// arm the stop timer and set the STOP stunt bit (pm_shared TSHandle halves the move command
// while it's set). PreThink counts the timer down and clears the bit on expiry; the stunt reset
// paths zero it. Callers: the CCombatAttack tail (roots the ATTACKER after a swing, refire*0.75)
// + KatanaFire's back/duck slash lock (delay*0.9) — both land in dossier S5/S7b.
void CBasePlayer::BlockPlayer(float flDuration)
{
	m_flBlockTime = flDuration;
	pev->iuser4 |= TSSTUNT_STOP;
}

// @0x85bac FindHullIntersectionplayer — byte-for-byte the stock crowbar FindHullIntersection,
// copied into TS_player.cpp and renamed by the original (avoids the static-linkage collision
// with crowbar.cpp's). Double the reach, straight re-trace, else the 2x2x2 corner loop keeping
// the closest hit (1e6 sentinel).
void FindHullIntersectionplayer(const Vector &vecSrc, TraceResult &tr,
	const float *mins, const float *maxs, edict_t *pEntity)
{
	float distance = 1e6f;
	const float *minmaxs[2] = {mins, maxs};
	TraceResult tmpTrace;

	Vector vecHullEnd = vecSrc + ((tr.vecEndPos - vecSrc) * 2.0f);
	UTIL_TraceLine(vecSrc, vecHullEnd, dont_ignore_monsters, pEntity, &tmpTrace);
	if (tmpTrace.flFraction < 1.0f)
	{
		tr = tmpTrace;
		return;
	}

	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 2; k++)
			{
				Vector vecEnd(vecHullEnd.x + minmaxs[i][0],
							  vecHullEnd.y + minmaxs[j][1],
							  vecHullEnd.z + minmaxs[k][2]);
				UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, pEntity, &tmpTrace);
				if (tmpTrace.flFraction < 1.0f)
				{
					float thisDistance = (tmpTrace.vecEndPos - vecSrc).Length();
					if (thisDistance < distance)
					{
						tr = tmpTrace;
						distance = thisDistance;
					}
				}
			}
}

// @0x85e28 CheckHull — the shared melee trace: a line trace first; on a miss, a hull trace
// (iHull: 3 = head_hull for most melee, 1 vs the small attack class) refined through
// FindHullIntersectionplayer, as the crowbar's: only when the hull hit the world or a brush
// entity (IsBSPModel, vtable 0xb8), with the duck hull's extents for the head hull (3) and the
// standing hull's otherwise (@0x85e28). tr.flFraction >= 1 after this = a miss.
void CBasePlayer::CheckHull(Vector vecSrc, Vector vecEnd, TraceResult &tr, int iHull)
{
	UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, edict(), &tr);
	if (tr.flFraction >= 1.0f)
	{
		UTIL_TraceHull(vecSrc, vecEnd, dont_ignore_monsters, iHull, edict(), &tr);
		CBaseEntity *pHit = CBaseEntity::Instance(tr.pHit);
		if (tr.flFraction < 1.0f && (!pHit || pHit->IsBSPModel()))
		{
			if (iHull == head_hull)
				FindHullIntersectionplayer(vecSrc, tr, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX, edict());
			else
				FindHullIntersectionplayer(vecSrc, tr, VEC_HULL_MIN, VEC_HULL_MAX, edict());
		}
	}
}

// DMG_TSCLOSECOMBAT (0x4000000) lives in ts_player.h — shared with the run-through resolver.

// NB: the -2 disarm path is now folded into the single CBasePlayer::DropWeapon(int type, float
// flDamage) in ts_gun.cpp (the faithful merge of the original's one @0x84900 function) — the split
// disarm-only shape here was removed. Disarm callers still call DropWeapon(-2, flDamage).

// @0x85f88 CloseCombatAttack — the shared melee hit-resolver (kung-fu twin; the coldcock sibling
// below sets glast_coldcock instead). Trace via CheckHull, gates, VictimKungfuHold deflect,
// disarm/knockback/sound for player victims, then the marked TraceAttack. Returns the damaged
// entity or NULL (miss/invalid/teammate/blocked). Callers (S5): CTSGun::CCombatAttack
// (hull class==3?1:3, disarm 1, playersOnly 0, knockback 1, teamSafe 0) and
// CTSStunt::CloseAttack (hull 3, iAnim -1, teamSafe 1).
CBaseEntity *CBasePlayer::CloseCombatAttack(Vector vecSrc, Vector vecEnd, Vector vecDir,
	int iHull, float flDamage, int iAnim, bool bDisarm, bool bPlayersOnly, bool bKnockback,
	bool bForceTeamSafe)
{
	if (iAnim != -1)
		m_TSAnim.Animate(this, iAnim);

	// damage-scaled motion-blur roll (dmg 35 => ~35%, > 80 => always; level 2 above 50)
	if (RANDOM_LONG(10, 80) < (int)flDamage)
	{
		MESSAGE_BEGIN(MSG_PVS, gmsgTSMotionBlur, pev->origin);
		WRITE_BYTE(ENTINDEX(edict()));
		WRITE_BYTE(flDamage > 50.0f ? 2 : 1);
		MESSAGE_END();
	}

	TraceResult tr;
	CheckHull(vecSrc, vecEnd, tr, iHull);
	if (tr.flFraction >= 1.0f)
		return nullptr; // miss

	edict_t *pHitEnt = tr.pHit ? tr.pHit : ENT(0);
	CBaseEntity *pVictim = CBaseEntity::Instance(pHitEnt);
	if (!pVictim)
		return nullptr;
	if (pVictim->pev->takedamage == DAMAGE_NO)
		return nullptr;
	if ((pVictim->pev->flags & FL_WORLDBRUSH) != 0)
		return nullptr;
	if ((pVictim->pev->effects & EF_NODRAW) != 0)
		return nullptr;

	if (pVictim->IsPlayer())
	{
		if ((bForceTeamSafe || friendlyfire.value == 0.0f) &&
			g_pGameRules->PlayerRelationship(pVictim, this) == GR_TEAMMATE)
			return nullptr;

		if (VictimKungfuHold(this, pVictim, flDamage, &tr))
			return nullptr; // deflected (the Hold plays the clack + stamps the locks)

		CBasePlayer *pPlayerVictim = (CBasePlayer *)pVictim;
		if (bDisarm && pPlayerVictim->m_flSpawnProtectTime < gpGlobals->time)
			pPlayerVictim->DropWeapon(-2, flDamage); // -2: the active weapon, once the disarm pressure is reached

		if (bKnockback && flDamage > 30.0f)
		{
			float speed = pev->velocity.Length();
			Vector dir = pVictim->pev->origin - pev->origin;
			dir = dir.Length() > 0.0f ? dir.Normalize() : Vector(0, 0, 1);
			pVictim->pev->velocity = pVictim->pev->velocity + dir * speed;
			pVictim->pev->velocity.z = 235.0f; // hard-set vertical pop (disasm 0x863a0)
		}

		EMIT_SOUND_DYN(ENT(pVictim->pev), CHAN_BODY, "player/kungfuhit.wav", VOL_NORM, 0.8f, 0,
			95 + RANDOM_LONG(0, 31));

		// PunchScreen is empty in the original (@0x85f70), so this does nothing; kept as the original calls it
		if (m_pActiveItem && flDamage > 20.0f)
			PunchScreen(flDamage * 0.5f, pev->origin - pVictim->pev->origin);
	}
	else if (bPlayersOnly)
		return nullptr;

	glast_closecombat = true;
	ClearMultiDamage();
	pVictim->TraceAttack(pev, flDamage, vecDir, &tr, DMG_CLUB | DMG_NEVERGIB | DMG_TSCLOSECOMBAT);
	ApplyMultiDamage(pev, pev);
	glast_closecombat = false;

	return pVictim;
}

// @0x86610 ColdCockAttack — the pistol-whip sibling twin: identical skeleton, sets
// glast_coldcock, and its knockback path is never exercised (the ColdCock caller passes
// bKnockback=false). The damage bits are CloseCombatAttack's, 0x4001080 (@0x86a56).
CBaseEntity *CBasePlayer::ColdCockAttack(Vector vecSrc, Vector vecEnd, Vector vecDir,
	int iHull, float flDamage, int iAnim, bool bDisarm, bool bPlayersOnly, bool bKnockback,
	bool bForceTeamSafe)
{
	if (iAnim != -1)
		m_TSAnim.Animate(this, iAnim);

	if (RANDOM_LONG(10, 80) < (int)flDamage)
	{
		MESSAGE_BEGIN(MSG_PVS, gmsgTSMotionBlur, pev->origin);
		WRITE_BYTE(ENTINDEX(edict()));
		WRITE_BYTE(flDamage > 50.0f ? 2 : 1);
		MESSAGE_END();
	}

	TraceResult tr;
	CheckHull(vecSrc, vecEnd, tr, iHull);
	if (tr.flFraction >= 1.0f)
		return nullptr;

	edict_t *pHitEnt = tr.pHit ? tr.pHit : ENT(0);
	CBaseEntity *pVictim = CBaseEntity::Instance(pHitEnt);
	if (!pVictim)
		return nullptr;
	if (pVictim->pev->takedamage == DAMAGE_NO)
		return nullptr;
	if ((pVictim->pev->flags & FL_WORLDBRUSH) != 0)
		return nullptr;
	if ((pVictim->pev->effects & EF_NODRAW) != 0)
		return nullptr;

	bool bApplyDamage = true;
	if (pVictim->IsPlayer())
	{
		if ((bForceTeamSafe || friendlyfire.value == 0.0f) &&
			g_pGameRules->PlayerRelationship(pVictim, this) == GR_TEAMMATE)
			return nullptr;

		if (VictimKungfuHold(this, pVictim, flDamage, &tr))
			bApplyDamage = false; // held: no drop/knockback/sound/damage
		else
		{
			CBasePlayer *pPlayerVictim = (CBasePlayer *)pVictim;
			if (bDisarm && pPlayerVictim->m_flSpawnProtectTime < gpGlobals->time)
				pPlayerVictim->DropWeapon(-2, flDamage);

			if (bKnockback && flDamage > 30.0f) // not exercised by coldcock (bKnockback=false)
			{
				float speed = pev->velocity.Length();
				Vector dir = pVictim->pev->origin - pev->origin;
				dir = dir.Length() > 0.0f ? dir.Normalize() : Vector(0, 0, 1);
				pVictim->pev->velocity = pVictim->pev->velocity + dir * speed;
				pVictim->pev->velocity.z = 235.0f;
			}

			EMIT_SOUND_DYN(ENT(pVictim->pev), CHAN_BODY, "player/kungfuhit.wav", VOL_NORM, 0.8f, 0,
				95 + RANDOM_LONG(0, 31));

			if (m_pActiveItem && flDamage > 20.0f)
				PunchScreen(flDamage * 0.5f, pev->origin - pVictim->pev->origin);
		}
	}
	else if (bPlayersOnly)
		return nullptr;

	if (!bApplyDamage)
		return nullptr;

	glast_coldcock = true;
	ClearMultiDamage();
	pVictim->TraceAttack(pev, flDamage, vecDir, &tr, DMG_CLUB | DMG_NEVERGIB | DMG_TSCLOSECOMBAT); // @0x86a56
	ApplyMultiDamage(pev, pev);
	glast_coldcock = false;

	return pVictim;
}

// @0x83a3c DropArmor — toss the kevlar as a CTSAmmoPack in front of the player (a dead one's in a
// random direction), then take it off through TakeArmor(-1), which gives its 10 slots back and sets
// the speed. The pack carries no armor (CreateAmmoPack ignores it), so the kevlar is lost. Nothing in
// the original calls it.
void CBasePlayer::DropArmor()
{
	if (pev->armorvalue <= 0.0f)
		return;

	Vector angles;
	float push;
	if (!IsAlive())
	{
		angles = Vector(RANDOM_LONG(-60, 60), RANDOM_LONG(-180, 180), 0);
		push = RANDOM_LONG(60, 160);
	}
	else
	{
		angles = pev->v_angle;
		push = (90.0f - angles.x) + (90.0f - angles.x);
	}
	UTIL_MakeVectors(angles);
	CTSAmmoPack *pPack = CTSAmmoPack::CreateAmmoPack(this, nullptr, (int)pev->armorvalue);
	if (pPack)
	{
		pPack->pev->origin = pev->origin + gpGlobals->v_forward * 8.0f + gpGlobals->v_right * 8.0f;
		pPack->pev->angles = g_vecZero;
		pPack->pev->avelocity.y = RANDOM_LONG(-300, 300);
		pPack->pev->avelocity.z = RANDOM_LONG(-300, 300);
		pPack->pev->velocity = pev->velocity * 0.9f + gpGlobals->v_forward * push + Vector(0, 0, push * 0.33f);
	}
	EMIT_SOUND(ENT(pev), CHAN_ITEM, "weapons/weapondrop.wav", VOL_NORM, ATTN_NORM);
	TakeArmor(-1); // @0x83d8f
}

// @0x846f0 DropAmmo — with the TS arsenal active and any reserve, drop the reserve as a CTSAmmoPack
// near the player, unturned, spinning; the reserve itself is left as it is (the player is dying).
// Killed calls it first, before the flag and the weapons.
void CBasePlayer::DropAmmo()
{
	if (!m_pActiveItem || m_pActiveItem->m_iId != TSGUN_WEAPON_ID)
		return;
	CTSGun *pGun = (CTSGun *)(CBasePlayerItem *)m_pActiveItem;

	bool any = false;
	for (int c = 0; c < TS_NUM_CALIBERS && !any; c++)
		if (pGun->m_ammoReserve[c] > 0)
			any = true;
	if (!any)
		return;

	Vector angles(RANDOM_LONG(-100, 100), RANDOM_LONG(-100, 100), 0);
	UTIL_MakeVectors(angles);
	CTSAmmoPack *pPack = CTSAmmoPack::CreateAmmoPack(this, pGun->m_ammoReserve, (int)pev->armorvalue);
	if (pPack)
	{
		pPack->pev->origin = pev->origin + gpGlobals->v_forward * 8.0f + gpGlobals->v_right * 8.0f;
		pPack->pev->angles = g_vecZero;
		pPack->pev->avelocity.y = RANDOM_LONG(-300, 300);
		pPack->pev->avelocity.z = RANDOM_LONG(-300, 300);
	}
}

// Free helper for the wire-cut console command (the cl_dll wire UI isn't built).
void CBasePlayer::CutWire(int wire)
{
	CBaseEntity *pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "ts_bomb")) != nullptr)
		((CTSBomb *)pEnt)->CutWire(wire, this);
}
