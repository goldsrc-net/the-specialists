/***
*  ts_gamerules.cpp — RECONSTRUCTED CTSGameRules (skeleton + GetRandomPowerup).
*  Functional/source-faithful (see CLAUDE.md). recovered/server/CTSGameRules.c.
***/
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "gamerules.h"
#include "client.h"     // respawn()
#include "ts_gamerules.h"
#include "ts_bullet.h"
#include "ts_utils.h"   // TSHudMessageAll
#include "ts_objectives.h" // CTSObjective / CTSObjectiveManager (the objective subsystem)
#include "ts_func_break.h" // CInfoBreakable::RespawnThink (RespawnEntities)
#include "ts_groundweapon.h" // CTSGroundWeapon::Fall (RespawnEntities)
#include "ts_flag.h"       // CTSFlag::RespawnFlag, HaveFlag (CTF)
#include "ts_weapons.h" // TSDeployArsenal (team-mode arsenal on team switch)
#include "ts_weapon_info.h" // Set_ammo_count (the SrvSett id-2 pair)
#include "UserMessages.h"
#include "ts_powerup.h"       // CTSPowerUp::Fall (RespawnEntities)
#include "teamplay_gamerules.h" // CHalfLifeTeamplay::RecountTeams (RestartRound)
#include "ts_gun.h"             // CTSGun (RestartRound's loadout refund)
#include "game.h"          // g_flWeaponCheat (sv_cheats as the map started)


// Defined in ts_objectives.cpp — fire the objective manager's targetontimeleft on round timeout.

// Round-time HUD message (remaining seconds); registered lazily on first send.
int gmsgTSRoundTime = 0;
int gmsgTSVoiceMessage = 0;   // "TSHVoice" (1): per-player capture taunt (sent from TS_objectives.cpp;
                              // DEFINED here — the original homes this global in TS_gamerulesclass.cpp)
int gmsgTSObjective = 0;      // "Objective" (-1, variable): one HUD objective entry (coord+idx+label)
int gmsgTSShowObjective = 0;  // "ShowObj" (2): toggle a HUD objective entry's visibility

// (StartRoundTimer DELETED, fold-plan G4: the original has no such method — the round-timer
// arm is INLINE in RestartRound @0x67920; Think @0x77204 never arms. See both call sites.)

//============================================================================
// Objective subsystem — the HUD objective list + the round's objective managers.
//============================================================================

// @0x6abdc — reset the HUD objective list.
void CTSGameRules::ClearObjectiveList()
{
	m_iObjectiveCount = 0;
	for (int i = 0; i < 32; i++)
		m_objectiveList[i] = ObjectiveEntry();
}

// @0x6ae20 — the slot index of `pObj` in the list, or -1.
int CTSGameRules::FindIndexByObjective(CTSObjective *pObj)
{
	for (int i = 0; i < m_iObjectiveCount; i++)
		if (m_objectiveList[i].obj == pObj)
			return i;
	return -1;
}

// @0x6ac00 — append `pObj` (with its 4 per-team label string-indices) to the list, deduped. Returns
// the (existing or new) index, or -1 if the list is full.
int CTSGameRules::AddObjectiveInList(CTSObjective *pObj, const int teamStr[4])
{
	int i = FindIndexByObjective(pObj);
	if (i >= 0)
		return i;               // already listed
	if (m_iObjectiveCount >= 32)
		return -1;              // full
	int idx = m_iObjectiveCount;
	m_objectiveList[idx].obj = pObj;
	for (int t = 0; t < 4; t++)
		m_objectiveList[idx].teamStr[t] = teamStr ? teamStr[t] : 0;
	m_iObjectiveCount++;
	return idx;
}

// @0x6ac80 — send one HUD objective entry to one client (its world position, slot id, and the
// per-team label text).
void CTSGameRules::SendObjectiveToHud(int idx, CBasePlayer *pPlayer)
{
	if (!pPlayer || idx < 0 || idx >= m_iObjectiveCount)
		return;
	const ObjectiveEntry &slot = m_objectiveList[idx];

	char label[64] = {0};
	int teamIdx = GetTeamIndex(((CBaseEntity *)pPlayer)->TeamID());
	if (teamIdx >= 0 && teamIdx < 4 && slot.teamStr[teamIdx] != 0)
		strncpy(label, STRING((string_t)slot.teamStr[teamIdx]), sizeof(label) - 1);

	Vector c = slot.obj ? slot.obj->Center() : g_vecZero;
	MESSAGE_BEGIN(MSG_ONE, gmsgTSObjective, NULL, pPlayer->edict());
	WRITE_COORD(c.x);
	WRITE_COORD(c.y);
	WRITE_COORD(c.z);
	WRITE_BYTE(idx);
	WRITE_STRING(label);
	MESSAGE_END();
}

// @0x6add4 — send the whole list to one client (e.g. on join).
void CTSGameRules::SendObjectiveList(CBasePlayer *pPlayer)
{
	if (!pPlayer)
		return;
	for (int i = 0; i < m_iObjectiveCount; i++)
		SendObjectiveToHud(i, pPlayer);
}

// @0x6aebc — toggle one objective's HUD visibility for all clients. (CTSObjective::Freeze calls this.)
void CTSGameRules::ShowObjectiveOnHud(CTSObjective *pObj, bool show)
{
	int idx = FindIndexByObjective(pObj);
	MESSAGE_BEGIN(MSG_ALL, gmsgTSShowObjective);
	WRITE_BYTE(idx);
	WRITE_BYTE(show ? 1 : 0);
	MESSAGE_END();
}

// @0x685d8 — round start: scan the map's objective managers, set the game mode, and start every
// objective DISABLED (a manager's Reset, via SetObjective, enables the chosen subset).
void CTSGameRules::InitObjectives()
{
	// @0x68602 ENTRY GATE (§4.6): the objective subsystem is the team modes 2/3, which the shipped
	// game NEVER reaches — DecideGamePlay caps at 1 and this only re-flips 2<->3 when the mode is
	// ALREADY 2/3. It does not promote plain teamplay (mode 1), so for mode 0/1 this is a no-op
	// (objectives stay dead; teamplay stays mode 1 = free respawn). CTF/CTB works at mode 1 via the
	// briefcase/flag entities.
	if ((unsigned char)(m_iGameMode - 2) > 1)
		return;

	m_iManagerCount = 0;
	for (int i = 0; i < 32; i++)
		m_managers[i] = nullptr;
	m_pActiveManager = nullptr;

	CBaseEntity *pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "ts_objective_manager")) != nullptr)
		if (m_iManagerCount < 32)
			m_managers[m_iManagerCount++] = (CTSObjectiveManager *)pEnt;

	// @0x68693 — flip 2<->3 by objective-manager presence (mode is already 2/3 to be here).
	m_iGameMode = (m_iManagerCount != 0) ? 3 : 2;
	g_teamplay = true;
	if (m_iManagerCount == 0)
		return;

	// Disable every objective entity up front (a manager's Reset, via SetObjective, enables the chosen
	// subset). Sweep all entities via the CBaseEntity::IsObjective() virtual (true only on objectives).
	for (int i = 1; i < gpGlobals->maxEntities; i++)
	{
		edict_t *pe = INDEXENT(i);
		if (!pe || pe->free || !pe->pvPrivateData)
			continue;
		CBaseEntity *e = CBaseEntity::Instance(pe);
		if (e && e->IsObjective())
			e->EnableObjective(false);
	}
}

// @0x690b4 — pick the round's active objective manager at random and Reset() it (which enables +
// announces that manager's objective subset).
void CTSGameRules::SetObjective()
{
	ClearObjectiveList();
	if (m_iManagerCount == 0)
		return;
	m_pActiveManager = nullptr;
	int r = RANDOM_LONG(0, m_iManagerCount - 1);
	if (m_managers[r])
	{
		m_managers[r]->Reset();
		m_pActiveManager = m_managers[r];
	}
}

// (SendRoundTime DELETED, fold-plan G3: the original has no such method — the joiner
// round-clock catch-up is INLINE in CTSGameRules::InitHUD @0x69134, where it now lives.)

// @0x69024 RoundTimeLeft — the round ran out of time (a round running, the round timer on): the
// active objective manager's targetontimeleft fires, or without one a draw (TeamWin 2), which
// schedules the round restart.
void CTSGameRules::RoundTimeLeft()
{
	if (m_flRoundEnd == 0.0f || !m_bRoundTimerEnabled)
		return;
	if (!m_pActiveManager || FStringNull(m_pActiveManager->end_round_target))
		TeamWin(2, 0, true, false); // 2 = draw (nobody)
	else
		FireTargets(STRING(m_pActiveManager->end_round_target), m_pActiveManager, m_pActiveManager, USE_TOGGLE, 0);
}
// Defined in ts_corpse.cpp / ts_gun.cpp — the TS death effects.
CBaseEntity *TSCreateDeadCorpse(CBasePlayer *pPlayer, float lifetime);
void TSDropWeaponsOnDeath(CBasePlayer *pPlayer);

// (CTSGameRules::PlayerKilled DELETED — R1/Y8: the original CTSGameRules is a STANDALONE class
// with NO PlayerKilled; the virtual is the TS-patched CHalfLifeMultiplay::PlayerKilled @0x795e4
// (multiplay_gamerules.cpp — TheOneIsDead + CallCheckDeads live there now). The weapon/ammo
// death-scatter moved to its @0x7ed5c home in CBasePlayer::Killed. The DropFlag(false) call was
// a DOUBLE mechanism — the original drops the carrier's flag at the StartObserver spectate
// transition (@0x8975f), which our StartObserver already does; a killed carrier's flag now drops
// ~3 s later than before — original-faithful timing. The corpse spawns in PlayerDeathThink.)

// @0x68a08 TeamWin(team, cash, bRestart, bScore) — a team won the round (team 2 is a draw). With the
// round timer on and not a score, the clock goes out with state 2; bRestart restarts the round in
// 3 s. Alone in the game, every team's score and cash are reset ("Waiting for other players.").
// Otherwise every player gets cash, the winners `cash` (cashforwin's default string, 10000, when it
// is 0), the others cashforlose's (1200), and a TSHVoice (0 won, 1 lost); a winning team's score goes
// up, "<team> Win!" or "<team> Score!", and the log names the team with the installed rules' team
// score and player count. In the objective modes every named team's score goes out (TeamScore).
extern int team_scores[]; // teamplay_gamerules.cpp
void CTSGameRules::TeamWin(int team, int cash, bool bRestart, bool bScore)
{
	if (m_bRoundTimerEnabled && !bScore)
	{
		MESSAGE_BEGIN(MSG_ALL, gmsgTSRoundTime);
		WRITE_BYTE((int)(m_flRoundEnd - gpGlobals->time));
		WRITE_BYTE(2);
		MESSAGE_END();
	}
	if (bRestart)
	{
		m_flRoundRestart = gpGlobals->time + 3.0f;
		m_flRoundEnd = 0;
	}

	if (CountPlayers() < 2)
	{
		for (int i = 0; i < TS_MAX_TEAMS; i++)
		{
			m_teamScore[i] = 0;
			m_teamCash[i] = (int)initial_cash.value;
		}
		TSHudMessageAll(0, 0, 0, 8, 30, "Waiting for other players.");
	}
	else
	{
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = (CBasePlayer *)UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer())
				continue;
			int win = cash, lose = 0;
			if (cash == 0)
				sscanf(cashforwin.string, "%i", &win);
			sscanf(cashforlose.string, "%i", &lose);
			const int lost = (team != GetTeamIndex(p->TeamID())) ? 1 : 0;
			p->GiveCash(lost ? lose : win);
			MESSAGE_BEGIN(MSG_ONE, gmsgTSVoiceMessage, NULL, p->edict());
			WRITE_BYTE(lost);
			MESSAGE_END();
		}

		if (team == 2)
		{
			TSHudMessageAll(0, 0, 0, 8, 30, "Game Draw");
			UTIL_LogPrintf("Round draw\n");
		}
		else
		{
			m_teamScore[team]++;
			char buf[40];
			sprintf(buf, bScore ? "%s Score!" : "%s Win!", m_teamNames[team]);
			TSHudMessageAll(0, 0, 0, 8, 30, buf);

			// the installed rules' index of the team and its players; a deathmatch rules object
			// answers -1 for every name (the original then reads team_scores[-1]; here 0)
			const int idx = g_pGameRules->GetTeamIndex(m_teamNames[team]);
			int players = 0;
			for (int i = 1; i <= gpGlobals->maxClients; i++)
			{
				CBaseEntity *p = UTIL_PlayerByIndex(i);
				if (p && p->IsPlayer() && g_pGameRules->GetTeamIndex(p->TeamID()) == idx)
					players++;
			}
			UTIL_LogPrintf("Team \"%s\" scored \"%d\" with \"%d\" players\n", m_teamNames[team],
				(idx >= 0 && idx < MAX_TEAMS) ? team_scores[idx] : 0, players);
		}
	}

	if ((unsigned char)(m_iGameMode - 2) < 2)
	{
		for (int i = 0; i < TS_MAX_TEAMS; i++)
		{
			if (m_teamNames[i][0] == '\0')
				continue;
			MESSAGE_BEGIN(MSG_ALL, gmsgTeamScore);
			WRITE_STRING(m_teamNames[i]);
			WRITE_SHORT(m_teamScore[i]);
			WRITE_SHORT(0);
			MESSAGE_END();
		}
	}
}

// @0x69bc0 CallCheckDeads — schedule a deads-check 1s out unless a round restart is already
// pending. Called on each player death and disconnect; a later one moves a pending check back.
void CTSGameRules::CallCheckDeads()
{
	if (m_flRoundRestart == 0.0f)
		m_flCheckDeads = gpGlobals->time + 1.0f;
}

// @0x69c00 CheckDeads — resolves a round after a death (CallCheckDeads, one second later) or when
// the clock runs out (Think). Nothing while a restart is pending or in plain teamplay (mode 1).
// Last man standing (mode 0): one player left alive with someone else in the game is the Last Man
// Standing (+10 frags, ScoreInfo, log line), alone he waits for others; either way the round
// restarts in 3 s. With more alive, or none, the round goes on ("%d men standing!") until the clock
// has run out ("Round over", restart in 3 s). Objective modes (2/3) count each team's players who
// are alive or still in the round and declare the winner, or fire the manager's dead-team target.
void CTSGameRules::CheckDeads()
{
	m_flCheckDeads = 0;
	if (m_flRoundRestart != 0.0f || m_iGameMode == 1)
		return;

	if (m_iGameMode == 0 && g_lastManStanding != 0)
	{
		int alive = 0, present = 0;
		CBasePlayer *pLast = nullptr;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *p = (CBasePlayer *)UTIL_PlayerByIndex(i);
			if (!p || !p->IsPlayer())
				continue;
			present++;
			if (p->IsAlive())
			{
				alive++;
				pLast = p;
			}
		}

		if (alive == 1)
		{
			if (present < 2)
			{
				TSHudMessageAll(0, 0xff, 0, 8, 30, "Waiting for other players.");
			}
			else
			{
				TSHudMessageAll(0, 0xff, 0, 1, 30,
					UTIL_VarArgs("%s is the Last Man Standing! %i frags", STRING(pLast->pev->netname), 10));
				pLast->pev->frags += 10;
				MESSAGE_BEGIN(MSG_ALL, gmsgScoreInfo);
				WRITE_BYTE(ENTINDEX(pLast->edict()));
				WRITE_SHORT((int)pLast->pev->frags);
				WRITE_SHORT(pLast->m_iDeaths);
				WRITE_SHORT(0);
				WRITE_SHORT(g_pGameRules->GetTeamIndex(pLast->m_szTeamName) + 1);
				MESSAGE_END();
				UTIL_LogPrintf("Player \"%s\" triggered \"last_man_standing\"\n", STRING(pLast->pev->netname));
			}
			m_flRoundRestart = gpGlobals->time + 3.0f;
			if (m_bRoundTimerEnabled)
			{
				MESSAGE_BEGIN(MSG_ALL, gmsgTSRoundTime);
				WRITE_BYTE((int)(m_flRoundEnd - gpGlobals->time));
				WRITE_BYTE(2);
				MESSAGE_END();
			}
		}
		else if ((alive > 0 && gpGlobals->time <= m_flRoundEnd && present > 1) || !m_bRoundTimerEnabled)
		{
			TSHudMessageAll(0, 0xff, 0, 1, 30, UTIL_VarArgs("%d men standing!", alive));
		}
		else
		{
			TSHudMessageAll(0, 0xff, 0, 8, 30, "Round over");
			m_flRoundRestart = gpGlobals->time + 3.0f;
		}
		return;
	}

	if ((unsigned char)(m_iGameMode - 2) >= 2)
		return;

	int count[4] = {};
	int total = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = (CBasePlayer *)UTIL_PlayerByIndex(i);
		if (!p || !p->IsPlayer())
			continue;
		if (!p->IsAlive() && p->m_flTSRespawnTime < 0.0f)
			continue;
		int team = GetTeamIndex(p->TeamID());
		if (team >= 0)
		{
			count[team]++;
			total++;
		}
	}

	if (m_iGameMode == 3 && total > 0)
	{
		// objectives: a wiped team fires the manager's targetonspecdead / targetonmercdead
		CTSObjectiveManager *m = m_pActiveManager;
		if (count[1] < 1)
		{
			if (m)
				FireTargets(STRING(m->m_targetMercDead), m, m, USE_TOGGLE, 0);
		}
		else if (count[0] < 1 && m)
		{
			FireTargets(STRING(m->m_targetSpecDead), m, m, USE_TOGGLE, 0);
		}
		return;
	}

	int winner;
	if (total < 1)
		winner = 2; // nobody left: a draw
	else if (count[1] < 1)
		winner = 0;
	else if (count[0] < 1)
		winner = 1;
	else
		return;
	TeamWin(winner, 0, true, false);
}

// Re-spawn everyone + re-arm the objective entities for a fresh round.
// @0x67d6c Init — one-time map setup. Arms the FIRST round 5s out (which, via RestartRound, runs
// InitObjectives/SetObjective when the map entities exist) and seeds the objective subsystem.
CTSGameRules::CTSGameRules()
{
	// The object is STATIC (@0x1681a0): this runs at static init (@0x67181), before gpGlobals is
	// valid, and the original's (__12CTSGameRules @0x671dc) only constructs the bullet manager at
	// +0x1864; every other field stays zero until Init (@0x67d6c), which only the multiplayer
	// rules call.
}

void CTSGameRules::Init()
{
	m_iGameMode = g_teamplay ? 1 : 0; // @0x67d83 — m_iGameMode = (byte)g_teamplay (0/1; 2/3 dead §4.6)
	// @0x67e88: every weapon category allowed, until a map's ts_mapglobals says otherwise as it spawns
	m_iAllowedCategories = m_iWeaponRestriction = InitWeaponRestriction();
	InitTeams();
	InitTeamScore(m_teamScore); // (no-op in the original)
	m_flRoundEnd = 0;
	// @0x67d9d: the global slow motion is off, and the three SrvSett caches are -1, so the first
	// Think sends realbullet, usecash and ammocount again
	m_flNextGlobalSlow = 0;
	m_flGlobalSlowActive = 0;
	m_flGlobalSlowScale = 0;
	m_flGlobalSlowEnd = 0;
	m_iSrvRealBullet = -1;
	m_iSrvAmmoCount = -1;
	m_iSrvUseCash = -1;
	// @0x67ec8..0x67ef9: no deads check pending, no One, the One count at 0, IsCTB to look again
	m_flCheckDeads = 0;
	m_bTheOneActive = false;
	m_iTheOneOthers = 0;
	m_bHasBriefcase = false;
	m_bCTBChecked = false;
	memset(m_votes, 0, sizeof(m_votes)); // @0x67e4f — a new map starts with no votes
	m_flRoundRestart = gpGlobals->time + 5.0f; // first round fires 5s into the map
	m_bRoundTimerEnabled = true;
	m_iRandomPowerups = -1;    // @0x67d96 (rules+1) — undecided until a map powerup spawns or Think runs
	m_flNextPowerupSpawn = 0;  // @0x67e14 (rules+4) — the first dynamic powerup comes at once
	m_bSrvSuperJump = 0; // @0x67ec2 — a map's ts_mapglobals sets it again as it spawns
	m_bWeaponParamsCustom = 0; // @0x67ebb (+0x5884) — a new map is not "custom" until loadweapons runs again
	m_flNextTheOneSelect = gpGlobals->time + 10.0f; // @0x67eab (+0x587c) — first The-One crown 10s in
	m_szEndMap[0] = '\0';                            // @0x67e73 (+0x1824) — clear any pending forced map
	g_BulletManager.Reset();                         // @0x67e83 (+0x1864) — no bullet flies on into a new map
	// @0x67ede/@0x67ee8: every map starts on the stock absorb values, until loadweapons reads a file
	m_flHealthAbsorb = 0.1f;    // +0x5888 (written, never read)
	m_flArmorAbsorb = 0.2125f;  // +0x588c — kevlar takes 21.25% off a hit (CBasePlayer::TakeDamage)
	InitObjectives(); // (finds nothing at construction-time; the +5s RestartRound re-runs it)
	SetObjective();
	// @0x67e6d (+0xf84): the first vote count comes 10 s into the map. The clock is the rules
	// object's, which outlives the map, so without this a map that ran longer than the next one
	// has run so far kept the count waiting for the old map's time.
	m_flNextVoteCheck = gpGlobals->time + 10.0f;
}

// @0x6a154 CountPlayers — number of connected players (no team/alive filter).
int CTSGameRules::CountPlayers()
{
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity *p = UTIL_PlayerByIndex(i);
		if (p && p->IsPlayer())
			n++;
	}
	return n;
}

// @0x67764 RespawnEntities — re-spawn the map's resettable entities by classname: breakables + flags
// always; ground weapons in the objective modes (2/3) or last man standing; powerups (their Fall) in
// last man standing.
void CTSGameRules::RespawnEntities(bool bFirst)
{
	const bool lms = g_lastManStanding != 0; // DecideGamePlay's flag (the old "lms" cvar read was dead — never registered)
	CBaseEntity *e;

	for (e = nullptr; (e = UTIL_FindEntityByClassname(e, "info_breakable")) != nullptr;)
		((CInfoBreakable *)e)->RespawnThink();

	if ((unsigned char)(m_iGameMode - 2) < 2 || lms) // @0x677db
		for (e = nullptr; (e = UTIL_FindEntityByClassname(e, "ts_groundweapon")) != nullptr;)
			((CTSGroundWeapon *)e)->Fall();

	if (lms)
		for (e = nullptr; (e = UTIL_FindEntityByClassname(e, "ts_powerup")) != nullptr;)
			((CTSPowerUp *)e)->Fall(); // @0x67889

	for (e = nullptr; (e = UTIL_FindEntityByClassname(e, "ts_flag")) != nullptr;)
		((CTSFlag *)e)->RespawnFlag(); // loose flags go home (the briefcase re-seeds via FlagFall)
}

// @0x67920 RestartRound — the actual round reset: re-arm the round timer, reset the objectives,
// refund every player's loadout and send those in play to spectate, recount teams, then respawn
// the map entities.
void CTSGameRules::RestartRound()
{
	// No player-count or game-mode gate (@0x67920): the clock is armed on an empty server too. Think
	// calls this only in last man standing and the objective modes 2/3 (@0x67204), so plain
	// deathmatch and teamplay never get here.
	m_flRoundRestart = 0; // disarm the pending restart (always)

	// (a)+(b) @0x67920 (fold-plan G4, decompile-exact): arm the round timer INLINE — the
	// `roundtime` cvar clamped [30,180]; the ARM is unconditional, only the clock BROADCAST
	// is gated on the round-timer flag (field_0x10). First, before the objectives (InitObjectives @0x67a14).
	{
		float t = CVAR_GET_FLOAT("roundtime");
		if (t < 30.0f) t = 30.0f;
		if (t > 180.0f) t = 180.0f;
		m_flRoundEnd = gpGlobals->time + t;

		if (m_bRoundTimerEnabled)
		{
			MESSAGE_BEGIN(MSG_ALL, gmsgTSRoundTime); // "RoundTime" 2 — registered centrally in LinkUserMessages
			WRITE_BYTE((int)(m_flRoundEnd - gpGlobals->time)); // seconds remaining
			WRITE_BYTE(0);                                     // flag: 0 = round start/restart
			MESSAGE_END();
		}
	}

	// (c) reset the objective subsystem — a no-op on a map with no objective entities (e.g. plain DM).
	InitObjectives();
	SetObjective();

	// (d) the banner, only in the team mode without objectives (@0x67a28: mode byte == 2).
	if (m_iGameMode == 2)
		TSHudMessageAll(0, 0, 0, 8, 30, "Defeat the opposite team"); // black, @0x67a45

	// (e) @0x67a4d: every player starts the round from spectate. Each gets the re-entry hold
	// (+10 s; in modes 2/3 only with +0x5874 set or in mode 2), the respawn gate (+3 s), the
	// respawning award, a fresh grenade-buy count and respawn budget, no powerups, and the
	// value of the loadout he holds as cash; then a player in play goes to spectate where he
	// stands. Nobody is respawned here: he enters play from spectate once the gate has passed.
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = (CBasePlayer *)UTIL_PlayerByIndex(i);
		if (!p || !p->IsPlayer())
			continue;
		if ((unsigned char)(m_iGameMode - 2) >= 2 || m_iGameMode == 2) // @0x67abf (+0x5874 is never set)
			p->m_flRespawnHoldTime = gpGlobals->time + 10.0f;
		p->m_flTSRespawnTime = gpGlobals->time + 3.0f; // @0x67b1a
		p->m_iAward = 11;
		p->m_iGrenadeBuys = 0;  // 0x714
		p->m_iRespawnsUsed = 0; // 0x7ba, @0x67b2e
		p->m_flPowerupExpireTime = 0;
		p->m_iPowerupTime = 0;
		p->m_iPowerups = 0;
		p->m_iHeldPowerup = 0;

		// @0x67b5d: the refund, read off the active item when it is the TS gun (id 16): each owned
		// weapon but the first and the flag at its price plus its attachments, and each caliber
		// holding more than one magazine at the magazine price per magazine held.
		int refund = 0;
		if (p->m_pActiveItem && p->m_pActiveItem->m_iId == TSGUN_WEAPON_ID)
		{
			CTSGun *gun = (CTSGun *)p->m_pActiveItem;
			for (int w = 0; w < TSW_COUNT; w++)
			{
				if (!gun->m_items[w].owned || w <= 0 || w == TSW_FLAG)
					continue;
				refund += get_weapon_info((tsweapon_t)w)->iPrice;
				const int attach = gun->m_items[w].attach;
				if (attach & 1)
					refund += 900;  // silencer
				if (attach & 2)
					refund += 800;  // laser sight
				if (attach & 4)
					refund += 500;  // flashlight
				if (attach & 8)
					refund += 1800; // scope
			}
			for (int c = 0; c < TS_NUM_CALIBERS; c++)
			{
				const s_ammo_info *ai = get_ammo_info(c);
				if (gun->m_ammoReserve[c] > ai->iRoundsPerMag)
					refund += ai->iPrice * gun->m_ammoReserve[c] / ai->iRoundsPerMag;
			}
		}
		p->GiveCash(refund);
		// @0x67cc9: StartObserver(origin, v_angle) for a player in play. reTS's spectate wait
		// (TSEnterSpectate) is StartObserver plus the wait state the "respawn" command checks; the
		// loadout he chose stays (the original resets it only for a joiner, InitHUD @0x6958d).
		if (p->pev->iuser1 == 0)
			p->TSEnterSpectate(false);
	}

	// @0x67d40: the team roster recount. The original calls it on g_pGameRules in every mode,
	// including last man standing, where the rules are not CHalfLifeTeamplay (nothing it writes
	// is read there); here only on team rules.
	if (g_pGameRules->IsTeamplay())
		((CHalfLifeTeamplay *)g_pGameRules)->RecountTeams(false);

	// (f) respawn the map's resettable entities.
	RespawnEntities(true);
}

// @0x68750 ResetRestartRound — in the objective modes (2/3) or last man standing (the
// lastmanstanding cvar as the map started, g_lastManStanding) arm a 1s restart + announce;
// otherwise nothing. With depth > 1 it also wipes the per-team round scores/cash.
void CTSGameRules::ResetRestartRound(int depth)
{
	if ((unsigned char)(m_iGameMode - 2) >= 2 && g_lastManStanding == 0)
		return;
	m_flRoundRestart = gpGlobals->time + 1.0f;
	m_flRoundEnd = 0;

	if (m_bRoundTimerEnabled && gmsgTSRoundTime != 0)
	{
		MESSAGE_BEGIN(MSG_ALL, gmsgTSRoundTime);
		WRITE_BYTE(1);
		WRITE_BYTE(0);
		MESSAGE_END();
	}
	// depth > 1 (the restartround command): every team's score and cash go back to the start, and
	// each named team's score of 0 goes out (TeamScore, @0x688a0). The cash is initial_cash's value
	// field, which stays 0 because the cvar is never registered.
	if (depth > 1)
		for (int i = 0; i < TS_MAX_TEAMS; i++)
		{
			m_teamScore[i] = 0;
			m_teamCash[i] = (int)initial_cash.value;
			if (m_teamNames[i][0] != '\0')
			{
				MESSAGE_BEGIN(MSG_ALL, gmsgTeamScore);
				WRITE_STRING(m_teamNames[i]);
				WRITE_SHORT(m_teamScore[i]);
				WRITE_SHORT(0);
				MESSAGE_END();
			}
		}
	TSHudMessageAll(0, 0, 0, 8, 30, "Restarting Round"); // black, @0x689f7
}

//============================================================================
// "The One" juggernaut mode — one player is buffed into The One; killing them inherits the role.
//============================================================================

// @0x67f08 IsCTB — capture-the-briefcase mode: in mode 1 only, a ts_briefcase exists (looked up once
// per map; Init clears the cache).
bool CTSGameRules::IsCTB()
{
	if (m_iGameMode != 1)
		return false;
	if (!m_bCTBChecked)
	{
		m_bHasBriefcase = UTIL_FindEntityByClassname(nullptr, "ts_briefcase") != nullptr;
		m_bCTBChecked = true;
	}
	return m_bHasBriefcase;
}

// @0x6af28 SelectRandomTheOne — every 10 s (its own clock, +0x587c), unless a player is already the
// One, draw one at random from the living players who are not observing, make him the One and move
// him to his team: SetDefaultPlayerTeam, then ChangePlayerTeam(him, TeamID(), no kill, no gib)
// (@0x6b030-0x6b063), which in The One mode puts him on "The ONE".
void CTSGameRules::SelectRandomTheOne()
{
	if (gpGlobals->time < m_flNextTheOneSelect)
		return;
	m_flNextTheOneSelect = gpGlobals->time + 10.0f;

	CBasePlayer *eligible[33];
	int n = 0;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = (CBasePlayer *)UTIL_PlayerByIndex(i);
		if (!p || !p->IsPlayer())
			continue;
		if (p->m_bTheOne)
			return; // someone is already The One
		if (p->IsAlive() && p->pev->iuser1 == 0 && n < 33)
			eligible[n++] = p;
	}
	if (n > 0)
	{
		CBasePlayer *pOne = eligible[RANDOM_LONG(0, n - 1)];
		SetPlayerTheOne(pOne);
		g_pGameRules->SetDefaultPlayerTeam(pOne);
		g_pGameRules->ChangePlayerTeam(pOne, pOne->TeamID(), false, false);
	}
}

// @0x6b078 SetPlayerTheOne — make a living, not observing `pPlayer` the One: 150 health (with the
// regeneration clock), kung fu and superjump (the "mj" physics key and a PwUp message for each), full
// speed, a slow motion powerup to hold if he has none, the award byte 10, and the announcement and
// log line.
void CTSGameRules::SetPlayerTheOne(CBasePlayer *pPlayer)
{
	if (!pPlayer->IsAlive() || pPlayer->pev->iuser1 != 0)
		return;
	pPlayer->m_bTheOne = 1;
	pPlayer->pev->health = 150.0f;
	pPlayer->m_flHealthRegen = gpGlobals->time + 1.0f;
	pPlayer->m_iKungFu = 1;
	pPlayer->m_bLowGravity = 1;
	g_engfuncs.pfnSetPhysicsKeyValue(pPlayer->edict(), "mj", "1"); // "The One": the DUCK+JUMP high jump (PM_Jump)
	pPlayer->SetPlayerSpeed(330.0f); // @0x83068 — the One moves at full speed
	extern int gmsgTSPowerUp; // ts_player.cpp
	MESSAGE_BEGIN(MSG_ONE, gmsgTSPowerUp, NULL, pPlayer->pev); // @0x6b123: his HUD shows kung fu
	WRITE_SHORT(TS_PWUP_KUNGFU);
	WRITE_BYTE(0);
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_ONE, gmsgTSPowerUp, NULL, pPlayer->pev); // and superjump
	WRITE_SHORT(TS_PWUP_LOWGRAV);
	WRITE_BYTE(0);
	MESSAGE_END();
	if (pPlayer->m_iPowerups == 0 && pPlayer->m_iHeldPowerup == 0)
	{
		pPlayer->m_iPowerups = 0;
		pPlayer->m_iHeldPowerup = 8; // the slow motion powerup (@0x6b1b5)
		pPlayer->m_flPowerupExpireTime = 0;
		pPlayer->m_iPowerupTime = 3;
	}
	pPlayer->m_iAward = 10; // TSPAward: The One (0xa) — never overwritten by kill tiers
	m_bTheOneActive = true; // rules+0x5878 (and rules+0x5877 = 0, a byte nothing reads)
	TSHudMessageAll(100, 0x96, 0xff, 1, 30,
		UTIL_VarArgs("%s is The One!", STRING(pPlayer->pev->netname)));
	UTIL_LogPrintf("Player \"%s\" triggered \"the_one\"\n", STRING(pPlayer->pev->netname));
}

// @0x6b258 TheOneIsDead — the One died (PlayerKilled @0x7a2fa, after the DeathMsg and the log line):
// he is no longer the One; in The One mode, killed by the world or himself "The One is dead." and
// the next 10 s check draws a new One; killed by another player, the dead One goes back to the other
// team (SetDefaultPlayerTeam, ChangePlayerTeam) and his killer becomes the One and moves to "The ONE".
void CTSGameRules::TheOneIsDead(CBasePlayer *pDead, CBasePlayer *pKiller)
{
	m_bTheOneActive = false;
	if (!pDead)
		return;
	pDead->m_bTheOne = 0;
	if (g_theOneMode == 0)
		return;
	if (!pKiller || pDead == pKiller)
	{
		TSHudMessageAll(100, 0x96, 0xff, 1, 30, "The One is dead.");
		return;
	}
	TSHudMessageAll(100, 0x96, 0xff, 1, 30,
		UTIL_VarArgs("The One was killed by %s!", STRING(pKiller->pev->netname)));
	g_pGameRules->SetDefaultPlayerTeam(pDead);
	g_pGameRules->ChangePlayerTeam(pDead, pDead->TeamID(), false, false);
	SetPlayerTheOne(pKiller);
	g_pGameRules->SetDefaultPlayerTeam(pKiller);
	g_pGameRules->ChangePlayerTeam(pKiller, pKiller->TeamID(), false, false);
}

//============================================================================
// Voting — per-player map / friendly-fire / teamplay votes, tallied to a majority.
//============================================================================

// @0x6a1b0 CalcMiniminVotants — votes needed to pass = half the votants (at least 1).
static int CalcMiniminVotants(int votants)
{
	int n = votants / 2;
	return n < 1 ? 1 : n;
}

// @0x6a1c8 PlayerVote — a player's `vote <...>` console command: reset / stats / map <name>. The
// vote is kept in the block at the player's ENTINDEX; past 31 it is not a vote (returns 0).
int CTSGameRules::PlayerVote(edict_t *pEntity, const char *pCmd)
{
	if (!pEntity || !pCmd)
		return 0;
	int idx = ENTINDEX(pEntity); // @0x6a1e9: IndexOfEdict, not 0-based
	if (idx < 0 || idx > 31)
		return 0;
	entvars_t *pev = &pEntity->v;

	if (strstr(pCmd, "reset"))
	{
		memset(&m_votes[idx], 0, sizeof(m_votes[idx])); // @0x6a24d
		m_votes[idx].owner = idx + 1;
		ClientPrint(pev, HUD_PRINTTALK, "Your votes are resetted.\n");
		return 1;
	}
	if (strstr(pCmd, "stats"))
	{
		// @0x6a2a7: the last count's tally, one line per map voted for, against the votes needed
		const int need = CalcMiniminVotants(m_iVotants);
		bool none = true;
		for (int i = 0; i < 32; i++)
		{
			if (!m_voteMaps[i][0])
				continue;
			ClientPrint(pev, HUD_PRINTTALK, UTIL_VarArgs("(%i/%i) votes for %s\n", m_voteMapCount[i], need, m_voteMaps[i]));
			none = false;
		}
		if (none)
			ClientPrint(pev, HUD_PRINTTALK, "No votes in progress.\n");
		return 1;
	}
	const char *pMap = strstr(pCmd, "map");
	if (!pMap)
		return 0;
	// @0x6a3c0: the first word after "map" — skip what is not a printing, non-space character, then
	// stop at the next one that is not
	char name[sizeof(m_votes[0].map)] = {0};
	strncpy(name, pMap + 3, sizeof(name) - 1);
	auto word = [](char c) { return isprint((unsigned char)c) && !isspace((unsigned char)c); };
	int start = 0;
	while (name[start] && !word(name[start]))
		start++;
	memmove(name, name + start, strlen(name + start) + 1);
	int end = 0;
	while (name[end] && word(name[end]))
		end++;
	name[end] = '\0';

	if (!strcmp(name, STRING(gpGlobals->mapname))) // case-sensitive, as the original
		ClientPrint(pev, HUD_PRINTTALK, UTIL_VarArgs("%s is the current map!\n", name));
	else if (IS_MAP_VALID(name))
	{
		strcpy(m_votes[idx].map, name);
		UTIL_ClientPrintAll(HUD_PRINTTALK, UTIL_VarArgs("%s vote for map %s\n", STRING(pev->netname), name));
	}
	else
		ClientPrint(pev, HUD_PRINTTALK, UTIL_VarArgs("%s is an invalid map\n", name));
	return 1;
}

// @0x6a5c4 CheckVotes — every 10 s, count the votes of the players in the game (ENTINDEX 0..31) into
// the tally the "vote stats" command lists (cleared first, @0x6a5f4), and
// apply the first category that has a majority, in the original's order: slowmatch on/off, friendly
// fire on/off, teamplay on/off (which ends the map: "nextmap"), else the most-voted map becomes the
// next map. The on/off votes are dead (nothing sets them), and where the original clears one it
// clears only the last counted player's, 32 times over (@0x6a930: the address is taken outside the
// loop); reTS does the same, but not for a last player past 31, whose block the original would
// write past the array.
void CTSGameRules::CheckVotes()
{
	if (gpGlobals->time < m_flNextVoteCheck)
		return;
	m_flNextVoteCheck = gpGlobals->time + 10.0f;

	memset(m_voteMaps, 0, sizeof(m_voteMaps));
	memset(m_voteMapCount, 0, sizeof(m_voteMapCount));
	m_voteSlowOn = m_voteSlowOff = m_voteTeamOn = m_voteTeamOff = m_voteFFOn = m_voteFFOff = 0;
	int votants = 0;
	int &slowOn = m_voteSlowOn, &slowOff = m_voteSlowOff, &tpOn = m_voteTeamOn, &tpOff = m_voteTeamOff;
	int &ffOn = m_voteFFOn, &ffOff = m_voteFFOff;
	char (&maps)[32][64] = m_voteMaps;
	int (&mapVotes)[32] = m_voteMapCount;
	int  best = -1, bestVotes = 0;
	int  last = 0; // the last counted player's ENTINDEX

	CBaseEntity *p = NULL;
	while ((p = UTIL_FindEntityByClassname(p, "player")) != NULL)
	{
		if (FNullEnt(p->edict()))
			break;
		if (!p->pev || !p->IsNetClient())
			continue;
		const int idx = ENTINDEX(p->edict());
		last = idx;
		if (idx > 31)
			continue;
		votants++;
		const VoteBlock &v = m_votes[idx];
		if (v.slowmatch == 1) slowOn++;  else if (v.slowmatch == 2) slowOff++;
		if (v.friendlyfire == 1) ffOn++; else if (v.friendlyfire == 2) ffOff++;
		if (v.teamplay == 1) tpOn++;     else if (v.teamplay == 2) tpOff++;
		if (!v.map[0])
			continue;
		bool counted = false;
		for (int m = 0; m < 32 && !counted; m++)
			if (!strcmp(maps[m], v.map))
			{
				counted = true;
				if (++mapVotes[m] > bestVotes) { bestVotes = mapVotes[m]; best = m; }
			}
		for (int m = 0; m < 32 && !counted; m++)
			if (!maps[m][0])
			{
				strcpy(maps[m], v.map);
				counted = true;
				if (++mapVotes[m] > bestVotes) { bestVotes = mapVotes[m]; best = m; }
			}
	}

	m_iVotants = votants;
	const int need = CalcMiniminVotants(votants);
	if (slowOn >= need || slowOff >= need)
	{
		CVAR_SET_FLOAT("mp_slowmatch", slowOn >= need ? 1.0f : 0.0f);
		UTIL_ClientPrintAll(HUD_PRINTCENTER, slowOn >= need ? "SlowMatch enabled." : "SlowMatch disabled.");
		for (int i = 0; i < 32 && last <= 31; i++)
			m_votes[last].slowmatch = 0;
		return;
	}
	if (ffOn >= need || ffOff >= need)
	{
		CVAR_SET_FLOAT("mp_friendlyfire", ffOn >= need ? 1.0f : 0.0f);
		UTIL_ClientPrintAll(HUD_PRINTCENTER, ffOn >= need ? "Friendly fire enabled." : "Friendly fire disabled.");
		for (int i = 0; i < 32 && last <= 31; i++)
			m_votes[last].friendlyfire = 0;
		return;
	}
	if (tpOn >= need || tpOff >= need)
	{
		const float want = tpOn >= need ? 1.0f : 0.0f;
		if (last <= 31)
			m_votes[last].teamplay = 0;
		if (CVAR_GET_FLOAT("mp_teamplay") == want)
			return;
		CVAR_SET_FLOAT("mp_teamplay", want);
		UTIL_ClientPrintAll(HUD_PRINTCENTER, want != 0.0f ? "TeamPlay in DM maps enabled, wait next map."
			: "TeamPlay in DM maps disabled, wait next map.");
		strcpy(m_szEndMap, "nextmap"); // end this map, the next from the mapcycle
		return;
	}

	// the most-voted map, if it reached the majority -> the next map (applied at map end).
	if (best != -1 && bestVotes >= need)
		strcpy(m_szEndMap, maps[best]);
}


// @ CTSGameRules::Think — the global slow motion, the deads check, then by rules mode the dynamic
// powerups, the One check and the rounds (modes 0/1) or the objective round timer (2/3), the votes
// and the server-setting broadcasts.

void CTSGameRules::Think()
{
	// (R1 phase 2: no base call — this is the original's NON-VIRTUAL Think @0x67204, invoked
	// by the TS-patched CHalfLifeMultiplay::Think @0xbf302 / CHalfLifeTeamplay::Think @0xd439a.)

	// `globalslow` command / SetGlobalSlowMotion driver (RE CTSGameRules::Think @0x67204, the FIRST
	// block): expire a timed slow, restore normal speed ONCE when it goes inactive (and clear
	// scale/end), else re-apply the scale each Think (GlobalSlowMotion2 self-throttles ~0.5s).
	if (m_flGlobalSlowEnd != 0.0f && m_flGlobalSlowEnd < gpGlobals->time)
		m_flGlobalSlowActive = 0;
	if (m_flGlobalSlowActive == 0.0f)
	{
		if (m_flGlobalSlowScale != 0.0f)
		{
			GlobalSlowMotion2(0.0f, 0); // restore normal speed once
			m_flGlobalSlowScale = 0.0f;
			m_flGlobalSlowEnd = 0.0f;
		}
	}
	else
	{
		GlobalSlowMotion2(m_flGlobalSlowScale, 0); // maintain the global slow
	}

	// (No per-frame slowmatch block: the original's Think has none. slowmatch is read where the
	// powerups are drawn, below.)

	// A scheduled deads-check (from CallCheckDeads, on a player death) resolves the round, once its
	// time has passed (@0x672ac: rules+0x5880 not 0 and below the time). CheckDeads clears it itself.
	if (m_flCheckDeads != 0 && m_flCheckDeads < gpGlobals->time)
		CheckDeads();

	// Round control — MODE-GATED exactly like the original Think (@0x67204, tsthink.c lines 96-121).
	// The powerup spawn, the One check and the round system (RestartRound / round timer /
	// CheckDeads-on-expiry) run, in that order, ONLY for rules mode 0/1 (@0x672e0: rules+0 <= 1); the
	// round system there only for last-man-standing (m_iGameMode==0 + g_lastManStanding). The dead
	// objective modes (m_iGameMode>=2, §4.6) run their own round timer and nothing else. PLAIN
	// DEATHMATCH (mode 0) and PLAIN TEAMPLAY/TDM (mode 1) are FREE-RESPAWN — no rounds. (Fixes
	// cont.94's over-broad `roundBased = g_teamplay || lms` gate, which made mode-1 TDM round-based:
	// "wait til next round" while alive, timer restarts.)
	if (m_iGameMode < 2)
	{
		// @0x672e9-0x673d2: the dynamic powerups. A map's own powerup (CTSPowerUp::Spawn) turned them off
		// before the first Think; otherwise they are on from here (rules+1: -1 undecided, 0 off, 1 on).
		// With nopowerup not 1, one is put at a random deathmatch spot when the time has come: a
		// slow motion one a quarter of the time while slowmatch is off (not 1), else any. The next
		// comes 20 s later, or 1 s when it could not be placed; a time more than 21 s off is cleared.
		if (m_iRandomPowerups == -1)
			m_iRandomPowerups = 1;
		if (CVAR_GET_FLOAT("nopowerup") != 1.0f && m_iRandomPowerups == 1)
		{
			if (m_flNextPowerupSpawn < gpGlobals->time)
			{
				int type = TS_PWUP_SLOWMO;
				if (RANDOM_FLOAT(0, 1) >= 0.25f || CVAR_GET_FLOAT("slowmatch") == 1.0f)
					type = GetRandomPowerup(TS_PWUP_ALL);
				m_flNextPowerupSpawn = gpGlobals->time + (PutPowerUp(type) ? 20.0f : 1.0f);
			}
			else if (m_flNextPowerupSpawn - gpGlobals->time > 21.0f)
				m_flNextPowerupSpawn = 0;
		}

		// @0x673d9: in The One mode (DecideGamePlay's g_theOneMode; there is no "the_one" cvar), every
		// 10 s look for the One among the players and draw one when nobody is. The scan runs from index
		// 0 to maxClients - 1 as the original's does, so it never sees the last slot; SelectRandomTheOne
		// looks at every slot again before it draws. The players it sees who are not the One are counted
		// (rules+0x5876, @0x67408-0x67468) for the One's health regeneration in PreThink @0x80bfe.
		if (g_theOneMode != 0 && gpGlobals->time > m_flNextTheOneSelect)
		{
			bool bHaveTheOne = false;
			m_iTheOneOthers = 0;
			for (int i = 0; i < gpGlobals->maxClients; i++)
			{
				CBaseEntity *p = UTIL_PlayerByIndex(i);
				if (p && p->IsPlayer() && !p->IsDormant())
				{
					if (((CBasePlayer *)p)->m_bTheOne)
						bHaveTheOne = true;
					else
						m_iTheOneOthers++;
				}
			}
			if (!bHaveTheOne)
				SelectRandomTheOne();
			m_flNextTheOneSelect = gpGlobals->time + 10.0f;
		}

		// The round logic comes after the One check, as in the original (@0x673d9, then @0x674a8).
		if (g_lastManStanding != 0 && m_iGameMode == 0) // last-man-standing only
		{
			if (m_flRoundRestart != 0 && gpGlobals->time >= m_flRoundRestart)
				RestartRound(); // arms m_flRoundEnd + broadcasts RoundTime + respawns; clears m_flRoundRestart
			if (gpGlobals->time >= m_flRoundEnd && m_flRoundRestart == 0)
				CheckDeads();
		}
	}
	else // m_iGameMode >= 2 — dead objective modes (unreachable in 2.1.x, §4.6; kept for fidelity)
	{
		if (gpGlobals->time >= m_flRoundEnd && m_flRoundRestart == 0 && m_bRoundTimerEnabled)
		{
			if (m_iGameMode == 2)
				TeamWin(2, 0, true, false);
			else
				RoundTimeLeft();
		}
		if (m_flRoundRestart != 0 && gpGlobals->time >= m_flRoundRestart)
			RestartRound();
	}

	CheckVotes(); // tally the players' map/ff/teamplay votes (self-throttled to ~10s)

	// --- SrvSett on-change broadcasts (the tail of the original Think; star_SrvSett.md) ---
	// @0x675b3: realbullet and usecash are compared as floats, the cached int against the cvar's value,
	// and the value is stored chopped toward zero (fistp with the chop mode), so a fractional value is
	// sent again every frame, truncated. ammocount is chopped first and compared as an int (@0x676d8).
	if (gmsgTSServerSetting != 0)
	{
		if ((float)m_iSrvRealBullet != realbullet.value)
		{
			m_iSrvRealBullet = (int)realbullet.value;
			MESSAGE_BEGIN(MSG_ALL, gmsgTSServerSetting);
			WRITE_BYTE(0);
			WRITE_SHORT(m_iSrvRealBullet);
			MESSAGE_END();
		}
	}

	// @0x67635: the slow-motion bullets fly here, after the realbullet setting and only while the
	// manager counts any (rules+0x5864, its +0x4000).
	if (g_BulletManager.m_iActive != 0)
		g_BulletManager.Think();

	if (gmsgTSServerSetting != 0)
	{
		// @0x67650: the usecash setting is the dmcashbased cvar (registered as "usecash").
		if ((float)m_iSrvUseCash != dmcashbased.value)
		{
			m_iSrvUseCash = (int)dmcashbased.value;
			MESSAGE_BEGIN(MSG_ALL, gmsgTSServerSetting);
			WRITE_BYTE(1);
			WRITE_SHORT(m_iSrvUseCash != 0 ? 1 : 0); // cached chopped, WIRED as a bool
			MESSAGE_END();
		}
		const int v = (int)cv_ammocount.value;
		if (v != m_iSrvAmmoCount)
		{
			m_iSrvAmmoCount = v;
			Set_ammo_count(v); // the shared max-carry column update, BEFORE the send
			MESSAGE_BEGIN(MSG_ALL, gmsgTSServerSetting);
			WRITE_BYTE(2);
			WRITE_SHORT(v);
			MESSAGE_END();
		}
	}
}

// @0x69720 GlobalSlowMotion2 — apply (or clear, scale 0) a global slow motion to every player and to
// every grenade, knife and dropped gun. Throttled to 0.5 s except when turning off. Whatever is not
// slowed already takes the scale; turning off puts everything back to 1. The search of each
// classname stops at an entity with no edict or the world's.
static void TSGlobalSlowClass(const char *pszClassname, float scale)
{
	CBaseEntity *pEnt = NULL;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, pszClassname)) != NULL)
	{
		if (!pEnt->pev->pContainingEntity || ENTINDEX(pEnt->pev->pContainingEntity) == 0)
			return;
		if (!pEnt->InSlow())
			pEnt->GoSlow(scale, 0);
		if (scale == 0.0f)
			pEnt->GoSlow(1.0f, 0);
	}
}

void CTSGameRules::GlobalSlowMotion2(float scale, int slowBullets)
{
	if (m_flNextGlobalSlow > gpGlobals->time && scale != 0.0f)
		return;
	m_flNextGlobalSlow = gpGlobals->time + 0.5f;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity *pEnt = UTIL_PlayerByIndex(i);
		if (!pEnt || !pEnt->IsPlayer())
			continue;
		CBasePlayer *pPlayer = (CBasePlayer *)pEnt;
		if (!pPlayer->InSlow())
		{
			pPlayer->GoSlow(scale, 0);
			pPlayer->m_bSlowBullets = slowBullets != 0;
		}
		if (scale == 0.0f)
			pPlayer->GoSlow(1.0f, 0); // restore
	}

	TSGlobalSlowClass("grenade", scale);
	TSGlobalSlowClass("knife", scale);
	TSGlobalSlowClass("WorldGun", scale);
	TSGlobalSlowClass("rpg_rocket", scale); // reTS's tank rocket (func_tank.cpp), not TS 3.0's
}

// @ SetGlobalSlowMotion__12CTSGameRulesff — start/stop a timed global slow-motion.
void CTSGameRules::SetGlobalSlowMotion(float scale, float duration)
{
	if (scale == m_flGlobalSlowScale)
		return;

	if (scale == 0.0f)
	{
		m_flGlobalSlowActive = 0;
		return;
	}

	GlobalSlowMotion2(0, 0); // reset everyone first
	m_flNextGlobalSlow = gpGlobals->time;
	m_flGlobalSlowScale = scale;
	m_flGlobalSlowActive = 1.0f;
	m_flGlobalSlowEnd = (duration == 0.0f) ? 0.0f : duration + gpGlobals->time;
}


// The original's team gate at each site is the global `g_teamplay != 0` (== m_iGameMode>=1, since
// m_iGameMode = (byte)g_teamplay and modes 2/3 are dead §4.6). The team-play virtual is stock
// CHalfLifeTeamplay::IsTeamplay (g_pGameRules->IsTeamplay()).

// GetGameDescription is inherited from CHalfLifeMultiplay (theOne->LMS->DM, no teamplay branch) —
// the original defines no CTSGameRules override. See multiplay_gamerules.cpp.

// @0x67f60 InitTeams — the four default teams: "Team1".."Team4", each spawning at
// "info_player_team<N>" with "info_player_deathmatch" as the fallback, with the models of
// defTeamModels (@0x1023e0). Teamplay's constructor then puts the teamlist cvar's names and, when
// it is set, the teammodels cvar's models over them. The objective modes (2/3) get "Specialists;
// Mercenaries" in seal/merc and team 1's fallback "info_player_start", but Init sets the mode
// (0/1) just before calling this, so that branch never runs. Each team starts with no score and
// initial_cash's value field in its cash pool (@0x67fba): 0, as the cvar is never registered. Only
// GetTeamCash reads it, in those dead modes.
static const char *const defTeamModels[TS_MAX_TEAMS] = {"seal;merc", "gordon;laurence", "agent", "hitman;castor"};

void CTSGameRules::InitTeams()
{
	memset(m_teamNames, 0, sizeof(m_teamNames));
	memset(m_teamModels, 0, sizeof(m_teamModels));
	memset(m_teamSpot, 0, sizeof(m_teamSpot));
	memset(m_teamSpotFallback, 0, sizeof(m_teamSpotFallback));
	for (int i = 0; i < TS_MAX_TEAMS; i++)
	{
		m_teamScore[i] = 0;                                    // +0x24 (and +0x20, no reTS member)
		m_teamCash[i] = (int)initial_cash.value;              // +0x1c
		snprintf(m_teamNames[i], sizeof(m_teamNames[i]), "Team%i", i + 1);
		snprintf(m_teamSpot[i], sizeof(m_teamSpot[i]), "info_player_team%i", i + 1);
		strcpy(m_teamSpotFallback[i], "info_player_deathmatch");
		SetTeamModelList(i, defTeamModels[i]);
	}
	m_numTeams = TS_MAX_TEAMS;
	if ((unsigned char)(m_iGameMode - 2) < 2) // @0x6807a
	{
		SetTeamModelList(0, "seal");
		SetTeamModelList(1, "merc");
		SetTeamList("Specialists;Mercenaries");
		strcpy(m_teamSpotFallback[0], "info_player_start");
	}
}

// @0x680dc SetTeamList — parse a ";"-separated team list DIRECTLY into the 4 name slots (R1 item-9
// convergence, dossier §4.5): clear the slots, strtok on ";" (a name listed twice takes two slots,
// @0x681ec copies each one without a lookup), an ALERT per team, and cap = g_theOneMode ? 2 : 4.
// In The One mode the list's first name is not used: "The ONE" takes slot 0 in its place (@0x681ae)
// and the next name goes to slot 1, so "Specialists;Mercenaries" gives "The ONE" and "Mercenaries".
// The ONE is set only when the list has a first name. (m_numTeams is reTS's count of the slots
// filled, which is what the original's GetTeamsNum @0x685a8 counts from the names.)
void CTSGameRules::SetTeamList(const char *pList)
{
	for (int i = 0; i < TS_MAX_TEAMS; i++)
		m_teamNames[i][0] = '\0';
	m_numTeams = 0;

	const int cap = g_theOneMode ? 2 : TS_MAX_TEAMS;

	char buf[256];
	strncpy(buf, pList ? pList : "", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (char *pName = strtok(buf, ";"); pName && *pName && m_numTeams < cap; pName = strtok(nullptr, ";"))
	{
		if (m_numTeams == 0 && g_theOneMode)
			strcpy(m_teamNames[0], "The ONE");
		else
		{
			strncpy(m_teamNames[m_numTeams], pName, TS_TEAMNAME_LEN - 1);
			m_teamNames[m_numTeams][TS_TEAMNAME_LEN - 1] = '\0';
		}
		ALERT(at_console, "created team %s\n", m_teamNames[m_numTeams]);
		m_numTeams++;
	}
}

// @0x68258 SetTeamModelList — parse a "|"-separated list into the four teams' models: up to four
// pieces, then the per-team SetTeamModelList(i, piece) for every team ("" for a team the list
// leaves out, which clears it).
void CTSGameRules::SetTeamModelList(const char *pList)
{
	char buf[2048];
	strncpy(buf, pList ? pList : "", sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	memset(m_teamModels, 0, sizeof(m_teamModels));
	char piece[TS_MAX_TEAMS][512] = {};
	int n = 0;
	for (char *p = strtok(buf, "|"); p && *p && n < TS_MAX_TEAMS; p = strtok(nullptr, "|"))
	{
		strncpy(piece[n], p, sizeof(piece[n]) - 1);
		n++;
	}
	for (int i = 0; i < TS_MAX_TEAMS; i++)
		SetTeamModelList(i, piece[i]);
}

// @0x68438 SetTeamModelList(team, models) — one team's ";"-separated list of up to three models,
// each announced on the console.
void CTSGameRules::SetTeamModelList(int team, const char *pModels)
{
	if (team < 0 || team >= TS_MAX_TEAMS || !pModels)
		return;
	memset(m_teamModels[team], 0, sizeof(m_teamModels[team]));
	char buf[512];
	strncpy(buf, pModels, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	int n = 0;
	for (char *p = strtok(buf, ";"); p && *p && n < TS_TEAMMODELS; p = strtok(nullptr, ";"), n++)
	{
		strncpy(m_teamModels[team][n], p, TS_TEAMMODEL_LEN - 1);
		ALERT(at_console, "set model %s for team %s\n", m_teamModels[team][n], m_teamNames[team]);
	}
}

// @0x685a8 GetTeamsNum — number of active (named) teams.
int CTSGameRules::GetTeamsNum()
{
	return m_numTeams;
}

// @0x69aac GetTeamCash — the player's team cash pool, in the objective modes (2/3) only; 0 in any
// other mode or without a team. Init never sets those modes, so this returns 0 in the shipped game.
int CTSGameRules::GetTeamCash(CBasePlayer *pPlayer)
{
	if ((unsigned char)(m_iGameMode - 2) >= 2 || !pPlayer)
		return 0;
	int team = GetTeamIndex(pPlayer->TeamID());
	if (team < 0 || team >= TS_MAX_TEAMS)
		return 0;
	return m_teamCash[team];
}

// @0x69130 InitTeamScore / @0x67908 UpdateTeamScores — no-ops in the original (kept for parity).
void CTSGameRules::InitTeamScore(int *pScores) {}
void CTSGameRules::UpdateTeamScores(int a, int b) {}

// @0x69a08 GetPlayerModel — one of the player's team models (up to three), at random; "" without a
// team or models. The pick is kept at player+0x758 (m_iPlayerModel, @0x69a7c), which nothing in the original reads
// (reTS's mp_teammodels_force 2 does, reference/DEVIATIONS.md).
const char *CTSGameRules::GetPlayerModel(CBasePlayer *pPlayer)
{
	if (!pPlayer)
		return "";
	int team = GetTeamIndex(pPlayer->TeamID());
	if (team < 0 || team >= TS_MAX_TEAMS)
		return "";
	int count = 0;
	for (int i = 0; i < TS_TEAMMODELS; i++)
		if (m_teamModels[team][i][0] != '\0')
			count++;
	if (count == 0)
		return "";
	pPlayer->m_iPlayerModel = RANDOM_LONG(0, count - 1);
	return m_teamModels[team][pPlayer->m_iPlayerModel];
}

// @0x696c8 TogglePlayerFromSlow — take a player out of slow-motion. The original removes them from
// the gamerules' slowed-player registry; our GlobalSlowMotion2 re-applies each frame (no persistent
// array), so we just restore the player to normal speed.
void CTSGameRules::TogglePlayerFromSlow(CBasePlayer *pPlayer)
{
	if (pPlayer)
		pPlayer->GoSlow(1.0f, 0);
}

// The team roster is stable after SetTeamList; the team recount is CHalfLifeTeamplay::RecountTeams
// on the installed Teamplay object.

// @ GetTeamIndex — 0-based index of `pTeamName` in the roster, or -1. Case-insensitive,
// matching the .so's strcasecmp over the team-name fields.
int CTSGameRules::GetTeamIndex(const char *pTeamName)
{
	if (pTeamName && *pTeamName)
	{
		for (int tm = 0; tm < m_numTeams; tm++)
			if (!stricmp(m_teamNames[tm], pTeamName))
				return tm;
	}
	return -1;
}

const char *CTSGameRules::GetIndexedTeamName(int teamIndex)
{
	if (teamIndex < 0 || teamIndex >= m_numTeams)
		return "";
	return m_teamNames[teamIndex];
}

// Team assignment / auto-balance lives on the installed TS-patched CHalfLifeTeamplay
// (SetDefaultPlayerTeam/ChangePlayerTeam/TeamWithFewestPlayers); an entity's team string is read via
// its own TeamID().

// (AssignToTeam DELETED — R1 phase 3: the original assigns teams in the TS-patched
// CHalfLifeTeamplay::SetDefaultPlayerTeam @0xd44ac / ChangePlayerTeam @0xd488c
// (teamplay_gamerules.cpp); jointeam dispatches there @0x9e4b0. The pev->team stamp our
// version carried is gone: the objectives gates read TeamID(), as the original's do.)

// (CTSGameRules::PlayerSpawn DELETED — R1/Y9: the original CTSGameRules has NO PlayerSpawn;
// the virtual is the TS-patched CHalfLifeMultiplay::PlayerSpawn @0x790ac (multiplay_gamerules.cpp,
// where the equip core now lives). initial_cash moved to its ClientPutInServer home @0xacf44;
// the auto-balance tail is the tagged Y9-4/R1 interim in MP::PlayerSpawn.)

// @ InitHUD — send the team roster to a connecting client (so a real HUD can colour the
// scoreboard). The MP base does the rest of the HUD setup.
// "SrvSett" (3) — BYTE setting id + SHORT value. Ids: 0 realbullet, 1 usecash (bool on the
// wire), 2 ammocount, 3 weaponrestriction bitmask. InitHUD pushes ALL FOUR to a joiner
// (MSG_ONE); Think broadcasts ids 0-2 on cvar change (MSG_ALL); cmd_weaponrestriction
// broadcasts id 3 on change. star_SrvSett.md (decompile-settled).
int gmsgTSServerSetting = 0;

void CTSGameRules::InitHUD(CBasePlayer *pPlayer)
{
	// (R1 phase 2: no base call — this is the original's NON-VIRTUAL InitHUD @0x69134, invoked
	// from CBasePlayer::UpdateClientData's InitHUD block @0x81d00 AFTER g_pGameRules->InitHUD.)

	pPlayer->m_iRespawnsUsed = 0; // @0x694d1 — a joiner starts with a fresh respawn budget
	pPlayer->m_flTeamKillTime = 0; // @0x694f3, @0x694fd — and no team kills counted
	pPlayer->m_iTeamKills = 0;
	pPlayer->m_flKickTime = 0;     // @0x69504 — and no kick pending

	// items 1-2 of the original @0x69134 (dossier §2.4) — the cheats/damage notices come FIRST.
	// The damage notice sits inside the cheats test (@0x6918b), as damagemult itself applies only on
	// a map started with sv_cheats on.
	if (g_flWeaponCheat != 0.0f) // @0x69147: sv_cheats as the map started
	{
		TSHudMessageOne(0x2d, 0xc3, 0x2d, 8, 100, "This server has cheats enabled.", pPlayer->pev);
		const float dm = CVAR_GET_FLOAT("damagemult");
		if (dm > 0.0f)
			TSHudMessageOne(0x2d, 0xc3, 0x2d, 8, 100,
				UTIL_VarArgs("This server has custom weapon damages (%.2f)", dm), pPlayer->pev);
	}
	// custom-weapons catch-up (@0x691d8, item 2): a joiner on a customized server gets the table.
	if (m_bWeaponParamsCustom)
		InformClientsOnWeaponsParams(pPlayer);

	// SrvSett catch-up: the joiner gets all four settings (no late-joiner hole).
	// item 3: byte-0 sends the THINK-CACHED value (+0x578), not a fresh cvar read (Init
	// seeds it -1; Think refreshes on change) — dossier §2.4.
	MESSAGE_BEGIN(MSG_ONE, gmsgTSServerSetting, NULL, pPlayer->edict());
	WRITE_BYTE(0);
	WRITE_SHORT(m_iSrvRealBullet);
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_ONE, gmsgTSServerSetting, NULL, pPlayer->edict());
	WRITE_BYTE(1);
	WRITE_SHORT(CVAR_GET_FLOAT("usecash") != 0.0f ? 1 : 0); // read DIRECT, sent as a bool
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_ONE, gmsgTSServerSetting, NULL, pPlayer->edict());
	WRITE_BYTE(2);
	WRITE_SHORT((int)(CVAR_GET_FLOAT("ammocount") + 0.5f)); // read DIRECT (no Set_ammo_count here)
	MESSAGE_END();
	MESSAGE_BEGIN(MSG_ONE, gmsgTSServerSetting, NULL, pPlayer->edict());
	WRITE_BYTE(3);
	WRITE_SHORT(m_iWeaponRestriction);
	MESSAGE_END();

	// Sent once, as the original (@0x691fe..0x69316); nothing sends them again to a joiner.

	// (the TeamNames + TeamInfo roster catch-up is GONE from here — R1 phase 3: it lives in
	// the STOCK CHalfLifeTeamplay::InitHUD @0xd45d8, which runs on team maps now that Teamplay
	// is installed as g_pGameRules. Dossier §2.3/§2.5.)

	// @0x69134 (fold-plan G3, decompile-exact — inv_C lines 163-177): catch the new client up on
	// the round clock (the round-start broadcast only happens at arm-time). Gated on the timer
	// being ARMED (m_flRoundEnd != 0) AND the round-timer flag (field_0x10); an EXPIRED clock
	// sends a CLAMPED 0, it is not skipped. No game-mode gate (@0x6931f..0x6933b test only the
	// two fields), so a last man standing joiner, whose rules are not teamplay, gets the clock too.
	if (m_flRoundEnd != 0.0f && m_bRoundTimerEnabled)
	{
		float left = m_flRoundEnd - gpGlobals->time;
		if (left < 0)
			left = 0;
		MESSAGE_BEGIN(MSG_ONE, gmsgTSRoundTime, NULL, pPlayer->edict());
		WRITE_BYTE((int)left); // truncated (@0x69384..0x693a5: fistp in round-toward-zero)
		WRITE_BYTE(0);
		MESSAGE_END();
	}

	// @0x693c8: then the joiner catches up on EVERY player's spectator state — MSG_ONE per
	// connected player: (BYTE idx, BYTE iuser1 != 0). After the round clock, as in the original.
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity *p = UTIL_PlayerByIndex(i);
		if (!p)
			continue;
		MESSAGE_BEGIN(MSG_ONE, gmsgSpectator, NULL, pPlayer->edict());
		WRITE_BYTE(i);
		WRITE_BYTE(p->pev->iuser1 != 0 ? 1 : 0);
		MESSAGE_END();
	}

	// @0x694c8: the joiner, alive since ClientPutInServer's Spawn, goes to spectate here, on his
	// first UpdateClientData: StartObserver(origin, v_angle), then the respawn gate from respawntime
	// (SetPlayerRespawnTime @0x6956b, unconditional — it overwrites the 5 s a team assignment just
	// set) and ResetArmament (@0x6958d). TSEnterSpectate keeps a future gate, so clear it first;
	// that also drops a deadline left from the previous map's clock (cont.96).
	pPlayer->m_flTSRespawnTime = 0.0f;
	pPlayer->TSEnterSpectate();

	// @0x69592: in the objective modes (g_teamplay 2 or 3 in the original; reTS keeps the mode in
	// m_iGameMode, its rules+0 copy, and g_teamplay as a bool) the random model index is cleared and
	// the joiner is sent the objective list.
	if ((unsigned char)(m_iGameMode - 2) <= 1)
	{
		pPlayer->m_iPlayerModel = 0; // player+0x758
		SendObjectiveList(pPlayer);
	}

	// @0x695e3: and the joiner starts with no votes (his block reset, as "vote reset" does).
	const int idx = ENTINDEX(pPlayer->edict());
	if (idx >= 0 && idx <= 31)
	{
		memset(&m_votes[idx], 0, sizeof(m_votes[idx]));
		m_votes[idx].owner = idx + 1;
	}
}
