/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
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
//
// ev_ts.cpp
//
// The Specialists predicted weapon fire event (events/fire.sc → EV_TSFire). The server
// (CTSGun::StandardFire) plays the event with iparam1 = the tsweapon_t; here we play the
// per-weapon fire sound, the viewmodel muzzleflash + "shoot" animation, and eject a
// shell — the faithful event path that replaces the server-driven SendWeaponAnim +
// EMIT_SOUND slice. The sound/anim tables mirror the server s_weapon_info
// (dlls/ts_weapons.cpp).
//

#include "hud.h"
#include "ts_gunanchor.h" // TS_PlayerGunAtt — the per-player gun-anchor cache (pelvis fix)
#include "cl_util.h"
#include "const.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "event_api.h"
#include "pm_defs.h"
#include "pm_materials.h" // CHAR_TEX_* material classes
#include "pm_shared.h"    // PM_FindTextureType
#include "eventscripts.h"
#include "ts_weapon_info.h" // the SHARED s_weapon_info table (now built into the client too)
#include "ts_weapon_predict.h" // the active-weapon param cache (refreshed by each TS event)
#include "ts_recoil.h"      // TS_TriggerViewShake — the melee bounce
#include "ts_particles.h"   // TS_ImpactEffect — the byte-ported per-surface impact dispatcher
#include "ts_bullets.h"     // the client bullet simulation (realbullet / slow motion)
#include "ts_attachments.h" // TS_AttachPlayerWasAdded — the third-person gun smoke gate

#include <string.h>
#include <math.h>

void V_PunchAxis(int axis, float punch); // view recoil kick (cl_dll/view.cpp)

// Per-tsweapon_t fire sound (nullptr = silent: melee/grenade/silenced-base/akimbo).
static const char* const s_fireSound[] = {
	/* 0*/ nullptr, /* 1*/ "weapons/glock/glock-fire.wav", /* 2*/ "weapons/beretta/fire.wav",
	/* 3*/ "weapons/uzi/uzi-fire.wav", /* 4*/ "weapons/m3/m3-fire.wav", /* 5*/ "weapons/m4/m4-fire.wav",
	/* 6*/ "weapons/mp5sd/mp5sd-fire.wav", /* 7*/ "weapons/mp5k/mp5k-fire.wav", /* 8*/ "weapons/beretta/fire.wav",
	/* 9*/ "weapons/mk23/mk23-fire.wav", /*10*/ "weapons/mk23/mk23-fire.wav", /*11*/ "weapons/usas/usas-fire.wav",
	/*12*/ "weapons/deagle/fire.wav", /*13*/ "weapons/ak47/fire.wav", /*14*/ "weapons/fnh/fnh-fire.wav",
	/*15*/ "weapons/aug/aug-fire.wav", /*16*/ "weapons/uzi/uzi-fire.wav", /*17*/ "weapons/tmp/fire.wav",
	/*18*/ "weapons/barrett/fire.wav", /*19*/ "weapons/pdw/fire.wav", /*20*/ "weapons/spas12/spas12-fire.wav",
	/*21*/ "weapons/gold/gold-fire.wav", /*22*/ "weapons/glock22/glock22-fire.wav", /*23*/ "weapons/ump/ump-fire.wav",
	/*24*/ nullptr, /*25*/ nullptr, /*26*/ "weapons/mossberg/fire.wav", /*27*/ "weapons/m16/fire.wav",
	/*28*/ "weapons/ruger/fire.wav", /*29*/ nullptr, /*30*/ "weapons/fnh/fnh-fire.wav",
	/*31*/ "weapons/bull/fire01.wav", /*32*/ "weapons/m60/m60-fire.wav", /*33*/ "weapons/sawedoff/fire.wav",
	/*34*/ nullptr, /*35*/ nullptr, /*36*/ "weapons/contender/fire.wav", /*37*/ "weapons/tmp/fire.wav",
};

// Silenced fire sound, indexed by tsweapon_t — used when the weapon has a SILENCER attached
// (fire-event iparam2 bit0). nullptr = the weapon has no `-sil` variant in the binary's sound table
// (it can't take a silencer, or it's silenced-base like the mp5sd which keeps its normal — already
// quiet — fire.wav). The names are the literal `weapons/<w>/...fire-sil.wav` strings the original
// client references (verified present in ts_i386.so + client.dll). Akimbo variants reuse their
// single's `-sil` wav (beretta_akimbo/uzi_akimbo/mk23_akimbo/fnh_akimbo/skorpion_akimbo).
static const char* const s_fireSilSound[] = {
	/* 0*/ nullptr, /* 1*/ "weapons/glock/glock-fire-sil.wav", /* 2*/ "weapons/beretta/fire-sil.wav",
	/* 3*/ "weapons/uzi/uzi-fire-sil.wav", /* 4*/ nullptr, /* 5*/ "weapons/m4/m4-fire-sil.wav",
	/* 6*/ nullptr, /* 7*/ "weapons/mp5k/mp5k-fire-sil.wav", /* 8*/ "weapons/beretta/fire-sil.wav",
	/* 9*/ "weapons/mk23/mk23-fire-sil.wav", /*10*/ "weapons/mk23/mk23-fire-sil.wav", /*11*/ nullptr,
	/*12*/ "weapons/deagle/fire-sil.wav", /*13*/ nullptr, /*14*/ "weapons/fnh/fnh-fire-sil.wav",
	/*15*/ "weapons/aug/aug-fire-sil.wav", /*16*/ "weapons/uzi/uzi-fire-sil.wav", /*17*/ "weapons/tmp/fire-sil.wav",
	/*18*/ nullptr, /*19*/ "weapons/pdw/fire-sil.wav", /*20*/ nullptr,
	/*21*/ nullptr, /*22*/ "weapons/glock22/glock22-fire-sil.wav", /*23*/ "weapons/ump/ump-fire-sil.wav",
	/*24*/ nullptr, /*25*/ nullptr, /*26*/ nullptr, /*27*/ nullptr,
	/*28*/ nullptr, /*29*/ nullptr, /*30*/ "weapons/fnh/fnh-fire-sil.wav",
	/*31*/ nullptr, /*32*/ nullptr, /*33*/ nullptr,
	/*34*/ nullptr, /*35*/ nullptr, /*36*/ nullptr, /*37*/ "weapons/tmp/fire-sil.wav",
};

// Muzzle-flash grip for tsweapon_t `w`: true = one-handed (pistols + akimbo barrels) -> the small
// ts_muzzleflash1h sprite; false = two-handed (rifles/SMGs/shotguns) -> the larger ts_muzzleflash6.
// Read from the SHARED s_weapon_info pszAnimExt (onehanded/akimbo vs twohanded) — the same table
// the server builds. (RE: client.dll FUN_100474b0 loads ts_muzzleflash1h + ts_muzzleflash6.)
static bool TSWeaponOneHanded(int w)
{
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)w);
	return wi->pszAnimExt && strcmp(wi->pszAnimExt, "twohanded") != 0; // onehanded/akimbo -> 1h
}

// Draw the TS muzzle-flash flare sprite at the firing gun's muzzle attachment. `side` selects the
// attachment (0 = right/primary barrel, 1 = left barrel for akimbo). RE: client.dll FUN_100474b0
// spawns a short-lived additive sprite at the muzzle; we use the engine temp-sprite at the model's
// attachment[side]. For the local player it's the viewmodel; otherwise the player's world model.
extern "C" int CL_IsThirdPerson(); // cl_dll/in_camera.cpp (used before the later extern block)

// EV_WeaponAnimation only when the viewmodel actually has a model — while scope-zoomed the
// viewmodel is HIDDEN (model NULL) and an anim call there can abort the rest of the handler
// (round-7k: gunshot sound + impact effects vanished while zoomed).
static void TS_EvWeaponAnim(int anim, int body)
{
	cl_entity_t* vm = gEngfuncs.GetViewModel();
	if (!vm || !vm->model)
		return;
	gEngfuncs.pEventAPI->EV_WeaponAnimation(anim, body);
}

static void EV_TSMuzzleFlare(int idx, int weaponType, int side, bool silenced)
{
	// ALL weapons use the same muzzle flash sprite at the gun's barrel attachment: gun_muzzle.spr when
	// SILENCED (a dimmer flash) and gun_muzzle2.spr when not (brighter). (Replaces the old per-grip
	// ts_muzzleflash1h/6 selection.) Resolve the model index each shot — a cached index goes stale after
	// a map change (model indices are reassigned per map). EV_FindModelIndex is a cheap by-name lookup.
	const int spr = gEngfuncs.pEventAPI->EV_FindModelIndex(
		silenced ? "sprites/gun_muzzle.spr" : "sprites/gun_muzzle2.spr");

	// Local player in FIRST person -> the viewmodel's attachments. In THIRD person the viewmodel
	// isn't drawn and its attachments FREEZE at the last first-person position (playtest round 7:
	// the flares stuck at the spot where thirdperson was toggled) — use the player's world model,
	// exactly like the brass/smoke paths (dossier §0.3: third-person effects ride the PLAYER
	// attachments).
	const bool vmView = (EV_IsLocal(idx) && 0 == CL_IsThirdPerson());
	cl_entity_t* ent = vmView ? gEngfuncs.GetViewModel() : NULL;
	if (vmView && !ent)
		return;
	if (spr <= 0)
		return;

	// side -> ATTACHMENT mapping, proven by the models' embedded flash events: the akimbo
	// viewmodels pair attachments 0/1 with the RIGHT gun (5001/5011 on the rightshoot seqs)
	// and 2/3 with the LEFT (5021/5031 on leftshoot). side 1 = the left barrel -> attachment 2.
	// (The old attachment[side] indexed 1 — a RIGHT-gun attachment — for the left barrel: the
	// phantom right-side sprite on every left shot.)
	const s_weapon_info* fwi = get_weapon_info((tsweapon_t)weaponType);
	const bool akimbo = (fwi->iFireType == TSFT_AKIMBO || fwi->iFireType == TSFT_AKIMBO_ONLY);
	const int att = (akimbo && side) ? 2 : 0;

	float pos[3];
	if (vmView)
	{
		VectorCopy(ent->attachment[att], pos);
	}
	else
	{
		const float* att0 = TS_PlayerGunAtt(idx, att); // the renderer-fed anchor cache, never cl_entity.attachment
		pos[0] = att0[0]; pos[1] = att0[1]; pos[2] = att0[2];
	}
	const float scale = 0.5f;
	// A muzzle flash is a single brief pop — a very short life and NO frame-cycle (FTENT_SPRCYCLE
	// kept it animating/visible far too long, "slow enough to walk around"). One frame, gone fast.
	gEngfuncs.pEfxAPI->R_TempSprite(pos, vec3_origin, scale, spr,
		kRenderTransAdd, kRenderFxNoDissipation, 1.0f, 0.01f, 0);
}

// TS bullet impact: the byte-ported per-surface client particle dispatcher (FUN_10048f60),
// fed by a surface trace. Metal/glass -> scintille glint + sparks.spr shower (+ pellet marks);
// wood/concrete -> tumbling debris chips + smoke; dirt -> smoke; flesh/water -> nothing (the
// server's FireBulletsPlayer already does blood + the texture impact SOUND + the bullet decal,
// this only adds the missing VISUAL). Client-side + predicted via the fire event (no lag, no
// double sound/decal).
//
// EV_TSBulletHit is what client.dll's FUN_10044bf0 (impact sound and particles, `mode` going on to the
// dispatcher) and FUN_10001180 (the decal) do with a trace that hit. The fire core (TS_FireCore) and
// the client bullet simulation (ts_bullets.cpp) call it for each surface a round reaches; src/end are
// the points EV_TraceTexture probes between.
void EV_TSBulletHit(int idx, const float* src, const float* end, pmtrace_t& tr, int mode)
{
	{
		// Classify the hit surface. Brush (world/breakable) -> texture material char; anything else
		// (player/monster bbox) -> flesh.
		physent_t* pe = gEngfuncs.pEventAPI->EV_GetPhysent(tr.ent);
		char chType = CHAR_TEX_CONCRETE;
		bool isBrush = pe && (pe->solid == SOLID_BSP || pe->movetype == MOVETYPE_PUSH || tr.ent == 0);
		if (isBrush)
		{
			const char* texName = gEngfuncs.pEventAPI->EV_TraceTexture(tr.ent, (float*)src, (float*)end);
			if (texName && texName[0])
			{
				char buf[64];
				strncpy(buf, texName, sizeof(buf) - 1);
				buf[sizeof(buf) - 1] = 0;
				chType = PM_FindTextureType(buf);
			}
		}
		else
		{
			chType = CHAR_TEX_FLESH; // player/monster — server already sprays blood
		}

		// Bullet-hole decal — laid CLIENT-side, matching the ORIGINAL (its server ShootBulletsPlayer
		// lays NO bullet decal; the client picks "{shot" via DamageDecal + R_DecalShoot). This is the
		// drop-in-compatible mechanism: the decal appears whether the server is ours or the original,
		// and NO server sends a decal temp-entity, so original-client + our-server never double-decals.
		// client.dll's FUN_10001180 lays it only on a solid BSP physent (pe+0x60 == 4: the world, brush
		// entities), whatever its move type or texture; then FUN_100010e0 shoots the picked decal while
		// r_decals is not 0.
		if (pe && pe->solid == SOLID_BSP)
		{
			// The decal pick, client.dll's DamageDecal (0x10001000): a breakable (classnumber 1, glass)
			// takes "{break%i" with RandomLong(0, 2) + 1, a normal-rendermode wall "{shot%i" with
			// RandomLong(0, 4) + 1 (the `inc eax` at 0x1000102c and 0x100010be, which Ghidra's decompile
			// drops), anything else "{bproof1". decals.wad holds {break1..3} and {shot1..5}; without the
			// + 1 a fifth of the hits named {shot0, a decal that does not exist, and drew decal index 0.
			char decalName[16];
			// r_paintball (client.dll 0x10001000, cvar_t* 0x100987ac): while it is not 0 every hit takes
			// "{splat%i" with RandomLong(0, 17) instead, with no + 1 (decals.wad has {splat0..17 but no
			// {splat11).
			static cvar_t* s_paintball = nullptr;
			if (s_paintball == nullptr)
				s_paintball = gEngfuncs.pfnGetCvarPointer("r_paintball");
			if (s_paintball != nullptr && s_paintball->value != 0.0f)
				sprintf(decalName, "{splat%d", gEngfuncs.pfnRandomLong(0, 0x11));
			else if (pe->classnumber == 1)
				sprintf(decalName, "{break%d", gEngfuncs.pfnRandomLong(0, 2) + 1); // pe+0x8c == 1
			else if (pe->rendermode == kRenderNormal)
				sprintf(decalName, "{shot%d", gEngfuncs.pfnRandomLong(0, 4) + 1); // pe+0x68 == 0
			else
				strcpy(decalName, "{bproof1"); // rendermode not 0
			if (gEngfuncs.pfnGetCvarFloat("r_decals") != 0.0f)
			{
				int decalIdx = gEngfuncs.pEfxAPI->Draw_DecalIndex(
					gEngfuncs.pEfxAPI->Draw_DecalIndexFromName(decalName));
				gEngfuncs.pEfxAPI->R_DecalShoot(decalIdx,
					gEngfuncs.pEventAPI->EV_IndexFromTrace(&tr), 0, tr.endpos, 0);
			}
		}

		// Off the surface a touch so the sprites aren't clipped into the wall.
		Vector pos;
		for (int i = 0; i < 3; i++)
			pos[i] = tr.endpos[i] + tr.plane.normal[i] * 2.0f;

		TS_ImpactEffect(pos, tr.plane.normal, mode, chType);

		// Bullet ric impact sound — played CLIENT-side here (positional, predicted), NOT server-side:
		// the old server path used EMIT_AMBIENT_SOUND on the WORLD entity, which the engine spawns as a
		// PERSISTENT static sound at that point, so you'd hear buffered far-away wall hits "as you walk
		// up to them". A local EV_PlaySound is a one-shot at the impact, no static-sound spawn. Hard
		// surfaces only (the server already does blood + sound on a flesh hit). Pitched by the smoothed
		// slow factor like the gunshots (slow-mo deepens it).
		if (chType != CHAR_TEX_FLESH)
		{
			extern float g_flClientSlow; // cl_dll/ts_powerup_hud.cpp — smoothed slow factor
			// unclamped, as client.dll 0x10044db7 passes it
			const int pitch = 100 + (int)(300.0f * g_flClientSlow);
			char ric[24];
			sprintf(ric, "debris/ric%d.wav", gEngfuncs.pfnRandomLong(1, 5));
			// Entity 0 (the WORLD), so the sound is spatialised AT the impact `pos` and attenuates with
			// distance (ATTN_NORM) — NOT on the shooter's entity (idx), which would glue it to your ear.
			// It's a client-side one-shot (no server static-sound spawn), so no "hear it as you approach".
			gEngfuncs.pEventAPI->EV_PlaySound(0, pos, CHAN_STATIC, ric, VOL_NORM, ATTN_NORM, 0, pitch);
		}
	}
}

// client.dll's shared random (FUN_1001aea0): HL's UTIL_SharedRandomFloat seeding, but the range is a
// float, where the SDK's (com_weapons.cpp) truncates it to an unsigned int (so -pi..pi would draw from
// a range of 6).
unsigned int U_Random();
void U_Srand(unsigned int seed);
static float TS_SharedRandomFloat(unsigned int seed, float low, float high)
{
	U_Srand(seed + *(unsigned int*)&low + *(unsigned int*)&high);
	U_Random();
	U_Random();
	const float range = high - low;
	if (range == 0.0f)
		return low;
	return (float)(U_Random() & 0xffff) * (1.0f / 65536.0f) * range + low;
}

extern Vector v_origin; // cl_dll/view.cpp: the refdef view origin (client.dll 0x10190b28)

static float s_flCoreDamage;  // 0x1018a164: what is left of the last pellet that hit, for the pierce
static Vector s_vecCoreDir;   // 0x1018a1a8: the last pellet's direction
static int s_iWhizzCount;     // 0x1018a1c4: flips the whizz channel between 503 and 504

// The fire core, client.dll FUN_100450c0, called by every fire event with the shooter, its view basis,
// the weapon's pellets (wi+0x24), the gun position, the aim, the spread across and up (fparam1 and
// fparam2, which the server sets alike), the weapon's damage (wi+0x1c), shot mode (wi+0x20) and caliber (wi+0x8),
// and the event's seed (bparam2). Mode 2 is a pierce leg: one trace 4096 on with its impact, nothing
// else. Otherwise:
//  - one trace 4096 along the aim with no prediction set up (where the round lands, for the rest):
//    another player's round more than 200 long that passes the view inside the sphere about its middle
//    (radius half its length less 100) and within 75 of the view by the triangle-area test (Heron's
//    formula with (s - c) squared in place of (s - b), kept) cracks a weapons/whizz1..5.wav at the view,
//    volume 2 / (distance + 0.1) at most 1, attenuation 0.8, the melee pitch, channel 503/504 in turn;
//  - with cl_fx's interaction on, the pool is pushed from the gun (radius 512) and along the round's
//    path, by the damage (x 8 for caliber 3), swirling for calibers 5, 8 and 13;
//  - each pellet k = 1..pellets turns the aim by a seeded angle (seed + k, -pi..pi) and radius
//    1 - u^3 (u seeded seed + k + 1, 0..the caliber's flMult) in the right/up plane scaled by the
//    spread, and goes the caliber's iMaxTravel. With realbullet on or this client slowed it is handed
//    to the client bullet simulation; else it is traced (PM_STUDIO_BOX, the shooter not solid) and a
//    hit lays its impact and decal and leaves range multiplier x damage x the caliber's flFactor2;
//  - if that is over 5 (and the mode is not 1, and nothing was handed on), the last pellet goes on up
//    to min(it, 48) units through anything that is not solid there: the far face is traced back
//    (PM_NORMAL, the PM states already popped) for its impact, and a mode 2 leg goes on from there.
//    The leftover is client.dll's global, so a last pellet that hit nothing pierces with the value an
//    earlier one left (kept).
static void TS_FireCore(int idx, const Vector& fwd, const Vector& right, const Vector& up, int pellets,
	const Vector& src, const Vector& dir, float spreadX, float spreadY, float damage, int mode, int caliber,
	int seed)
{
	event_api_t* ev = gEngfuncs.pEventAPI;
	const bool real = TSBullets_RealBullet();
	const s_ammo_info* ai = get_ammo_info(caliber);
	pmtrace_t tr;

	if (mode == 2)
	{
		Vector end = src + dir * 4096.0f;
		ev->EV_SetUpPlayerPrediction(0, 1);
		ev->EV_PushPMStates();
		ev->EV_SetSolidPlayers(idx - 1);
		ev->EV_SetTraceHull(2);
		ev->EV_PlayerTrace((float*)&src, end, PM_STUDIO_BOX, -1, &tr);
		if (tr.fraction != 1.0f)
			EV_TSBulletHit(idx, src, end, tr, 2);
		ev->EV_PopPMStates();
		return;
	}

	Vector end = src + dir * 4096.0f;
	ev->EV_PlayerTrace((float*)&src, end, PM_STUDIO_BOX, -1, &tr);
	const Vector hit = tr.endpos;
	const Vector mid = (src + hit) * 0.5f;
	const Vector half = mid - hit;

	cl_entity_t* local = gEngfuncs.GetLocalPlayer();
	if (local && idx != local->index && (src - hit).Length() > 200.0f &&
		(mid - v_origin).Length() < half.Length() - 100.0f)
	{
		const float a = (v_origin - src).Length();
		const float b = (v_origin - hit).Length();
		const float c = (src - hit).Length();
		const float s = (b + c + a) * 0.5f;
		const float area = sqrtf((s - a) * (s - c) * (s - c) * s);
		const float d = (area + area) / c;
		if (d < 75.0f)
		{
			char sample[32];
			sprintf(sample, "weapons/whizz%d.wav", gEngfuncs.pfnRandomLong(1, 5));
			float vol = 1.0f / (d + 0.1f) * 2.0f;
			if (1.0f < vol)
				vol = 1.0f;
			extern float g_flClientSlow;
			const int pitch = gEngfuncs.pfnRandomLong(-5, 5) + 25 - (int)(g_flClientSlow * -75.0f);
			const int channel = 503 + s_iWhizzCount % 2;
			s_iWhizzCount++;
			ev->EV_PlaySound(-1, v_origin, channel, sample, vol, 0.8f, 0, pitch);
		}
	}

	const float power = (caliber == 3) ? damage * 8.0f : damage;
	if (g_tsFx.interact)
	{
		TSP_BlastPush(src, 512.0f, power);
		TSP_BulletPush(src, hit, power, caliber == 5 || caliber == 8 || caliber == 13);
	}

	for (int k = 1; k <= pellets; k++)
	{
		const float ang = TS_SharedRandomFloat(k + seed, -3.1415927f, 3.1415927f);
		const float u = TS_SharedRandomFloat(k + 1 + seed, 0.0f, ai->flMult);
		const float r = 1.0f - u * u * u;
		const float x = r * cosf(ang);
		const float y = r * sinf(ang);
		for (int i = 0; i < 3; i++)
			s_vecCoreDir[i] = x * right[i] * spreadX + y * up[i] * spreadY + dir[i];
		end = src + s_vecCoreDir * (float)ai->iMaxTravel;
		if (real)
		{
			TSBullets_Fire(idx, src, s_vecCoreDir, damage, caliber, mode);
			continue;
		}
		ev->EV_SetUpPlayerPrediction(0, 1);
		ev->EV_PushPMStates();
		ev->EV_SetSolidPlayers(idx - 1);
		ev->EV_SetTraceHull(2);
		ev->EV_PlayerTrace((float*)&src, end, PM_STUDIO_BOX, -1, &tr);
		if (tr.fraction != 1.0f)
		{
			const float range = (src - Vector(tr.endpos)).Length() / ((float)ai->iRangeUnit * 10.0f);
			s_flCoreDamage = get_range_multipler((int)(range * 10.0f)) * damage;
			EV_TSBulletHit(idx, src, end, tr, mode);
			s_flCoreDamage = s_flCoreDamage * ai->flFactor2;
		}
		ev->EV_PopPMStates();
	}

	if (real || mode == 1 || !(s_flCoreDamage > 5.0f))
		return;
	const float depth = (s_flCoreDamage < 48.0f) ? s_flCoreDamage : 48.0f;
	Vector exitPos = Vector(tr.endpos) + s_vecCoreDir * depth;
	if (gEngfuncs.PM_PointContents(exitPos, nullptr) == CONTENTS_SOLID)
		return;
	Vector face = tr.endpos;
	ev->EV_SetTraceHull(2);
	ev->EV_PlayerTrace(exitPos, face, PM_NORMAL, -1, &tr);
	if (tr.fraction != 1.0f)
		EV_TSBulletHit(idx, exitPos, face, tr, 2);
	const Vector legDir = s_vecCoreDir;
	TS_FireCore(idx, fwd, right, up, 1, exitPos, legDir, 0.0f, 0.0f, s_flCoreDamage, 2, caliber, seed);
}

// (EV_TSKnife — the faithful knife.sc handler — is defined below the shared melee kit.)

// ---- The faithful TS melee-event presentation kit (dossier-ev-melee.md §0.4–0.7) ----

// §0.6 — every melee EV_PlaySound in the original: pitch = 25 + 75*timescale + rand(-5,5)
// (100 ± 5 at normal time; deepens under bullet-time), volume = RandomFloat(0.92, 1.0),
// attenuation 0.8, CHAN_WEAPON. Our timescale equivalent is the smoothed client slow factor.
extern float g_flClientSlow; // cl_dll/ts_powerup_hud.cpp — smoothed slow factor (1.0 normal)
extern "C" int CL_IsThirdPerson(); // cl_dll/in_camera.cpp (C-linkage engine export)
static int TS_MeleeSoundPitch()
{
	return gEngfuncs.pfnRandomLong(-5, 5) + 25 + (int)(75.0f * g_flClientSlow);
}
static float TS_MeleeVolume()
{
	return gEngfuncs.pfnRandomFloat(0.92f, 1.0f);
}

// §0.4 — the original's IsLocalOrSpectated locality gate: the local player, or the player an
// in-eye spectator is riding (TS first-person spectate).
static bool TS_InEyeSpectating(int idx)
{
	return g_iUser1 == OBS_IN_EYE && g_iUser2 == idx;
}

// §0.7 — FUN_10045df0, the melee texture-impact classifier + sound. Returns the material char:
// 0 = sky (silent), CHAR_TEX_FLESH for a player hit (silent — the server owns flesh sounds),
// else the pm_materials char after playing ONE random surface note at the impact point
// (CHAN_STATIC, per-material volume, melee pitch).
static char TS_MeleePlayTextureSound(pmtrace_t* tr, const float* start, const float* end)
{
	int truecontents;
	if (gEngfuncs.PM_PointContents(tr->endpos, &truecontents) == CONTENTS_SKY)
		return 0;
	const int hitIdx = gEngfuncs.pEventAPI->EV_IndexFromTrace(tr);
	if (hitIdx >= 1 && hitIdx <= gEngfuncs.GetMaxClients())
		return CHAR_TEX_FLESH;

	char chType = CHAR_TEX_CONCRETE;
	const char* texName = gEngfuncs.pEventAPI->EV_TraceTexture(tr->ent, (float*)start, (float*)end);
	if (texName && texName[0])
	{
		if (*texName == '-' || *texName == '+')
			texName += 2;
		if (*texName == '{' || *texName == '!' || *texName == '~' || *texName == ' ')
			texName++;
		char buf[64];
		strncpy(buf, texName, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		chType = PM_FindTextureType(buf);
	}

	// one random surface note per material (the original's exact sets + volumes).
	char snd[32];
	float vol = 0.9f;
	switch (chType)
	{
	case CHAR_TEX_DIRT:
		sprintf(snd, "player/pl_dirt%d.wav", gEngfuncs.pfnRandomLong(1, 3));
		break;
	case CHAR_TEX_GRATE:
		sprintf(snd, "player/pl_grate%d.wav", gEngfuncs.pfnRandomLong(0, 1) ? 1 : 4);
		break;
	case CHAR_TEX_METAL:
		sprintf(snd, "player/pl_metal%d.wav", gEngfuncs.pfnRandomLong(1, 2));
		break;
	case CHAR_TEX_COMPUTER:
	case CHAR_TEX_GLASS:
		sprintf(snd, "debris/glass%d.wav", gEngfuncs.pfnRandomLong(1, 3));
		vol = 0.8f;
		break;
	case CHAR_TEX_SLOSH:
		sprintf(snd, "player/pl_slosh%d.wav", gEngfuncs.pfnRandomLong(1, 4));
		break;
	case CHAR_TEX_TILE:
		sprintf(snd, "player/pl_tile%d.wav", gEngfuncs.pfnRandomLong(1, 4));
		vol = 0.8f;
		break;
	case CHAR_TEX_VENT:
		strcpy(snd, "player/pl_duct1.wav");
		vol = 0.5f;
		break;
	case CHAR_TEX_WOOD:
		sprintf(snd, "debris/wood%d.wav", gEngfuncs.pfnRandomLong(1, 3));
		break;
	default:
		sprintf(snd, "player/pl_step%d.wav", gEngfuncs.pfnRandomLong(1, 2));
		break;
	}
	gEngfuncs.pEventAPI->EV_PlaySound(0, tr->endpos, CHAN_STATIC, snd, vol, 0.8f, 0, TS_MeleeSoundPitch());
	return chType;
}

// events/knife.sc → EV_TSKnife — the original LAB_10043c90 (dossier-ev-melee.md §1). The server
// (CTSGun::KnifeFire) plays it EVERY slash with iparam1 = the weaponType and iparam2 = the bloody
// flag. ALL slash presentation is client-side: 35u forward trace; miss -> knife_miss.wav; hard
// surface -> texture note + 6 metal glints + sparks + knife_hit.wav; wood/dirt -> texture note
// only; flesh -> silent + forces the blade bloody. The viewmodel swing is chosen CLIENT-side
// (RandomLong(0,2) -> swing ids 15/16/17 -> the weapon's anim block), body = bloody.
void EV_TSKnife(event_args_t* args)
{
	const int idx = args->entindex;
	const int weaponType = args->iparam1;
	int bloody = args->iparam2;

	Vector fwd, right, up;
	AngleVectors(args->angles, fwd, right, up);
	Vector gun;
	EV_GetGunPosition(args, gun, args->origin);
	Vector end;
	for (int i = 0; i < 3; i++)
		end[i] = gun[i] + fwd[i] * 35.0f;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(0, 1);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(idx - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(2);
	pmtrace_t tr;
	gEngfuncs.pEventAPI->EV_PlayerTrace(gun, end, PM_STUDIO_BOX, -1, &tr);

	if (tr.fraction >= 1.0f)
	{
		gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
			"weapons/knife/knife_miss.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
	}
	else
	{
		const char chType = TS_MeleePlayTextureSound(&tr, gun, end);
		if (chType == CHAR_TEX_FLESH)
			bloody = 1; // a flesh hit blooms the blade immediately (silent — the server owns it)
		else if (chType != 0 && chType != CHAR_TEX_DIRT && chType != CHAR_TEX_WOOD)
		{
			TS_MeleeImpact(tr.endpos, tr.plane.normal, chType, 6); // knife: 6 glints + sparks
			gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
				"weapons/knife/knife_hit.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
		}
	}

	if ((EV_IsLocal(idx) || TS_InEyeSpectating(idx)) && 0 == CL_IsThirdPerson())
	{
		const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);
		const int swing = gEngfuncs.pfnRandomLong(0, 2); // the client picks the swing (ids 15/16/17)
		TS_EvWeaponAnim(wi->iSwingAnims[swing], bloody);
	}
	if (EV_IsLocal(idx))
	{
		// per-swing "bounce-back" screen shake (RE client FUN_1000a270: amp = flDamage*0.15,
		// dur 0.2, freq 10 — live-verified earlier; presentation-only, local player).
		TS_TriggerViewShake(get_weapon_info((tsweapon_t)weaponType)->flDamage * 0.15f);
	}

	gEngfuncs.pEventAPI->EV_PopPMStates();
}

// events/katana.sc → EV_TSKatana — the original LAB_10044230 (dossier-ev-melee.md §2). The server
// (CTSGun::KatanaFire) plays it EVERY swing with iparam1 = the logical swing id (15 = the reverse
// back-slash / 16 / 17) and iparam2 = the bloody-blade flag. ALL swing presentation is client-side:
// the whoosh on a miss (miss/miss1.wav 50/50), the surface impact (texture note + 12 metal glints +
// sparks + hit/hit1.wav 50/50 on hard surfaces; silent on flesh, which forces the blade bloody),
// and the viewmodel anim — the swing id REMAPPED through the katana's anim block (body = bloody).
void EV_TSKatana(event_args_t* args)
{
	const int idx = args->entindex;
	const int animId = args->iparam1;
	int bloody = args->iparam2;

	Vector fwd, right, up;
	AngleVectors(args->angles, fwd, right, up);
	Vector gun;
	EV_GetGunPosition(args, gun, args->origin);

	// the trace follows the server's swing table: 15 sweeps 55u BEHIND, 16/17 50u forward.
	const float reach = (animId == 15) ? -55.0f : 50.0f;
	Vector end;
	for (int i = 0; i < 3; i++)
		end[i] = gun[i] + fwd[i] * reach;

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(0, 1);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(idx - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(2);
	pmtrace_t tr;
	gEngfuncs.pEventAPI->EV_PlayerTrace(gun, end, PM_STUDIO_BOX, -1, &tr);

	if (tr.fraction >= 1.0f)
	{
		gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
			gEngfuncs.pfnRandomLong(0, 1) ? "weapons/katana/miss.wav" : "weapons/katana/miss1.wav",
			TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
	}
	else
	{
		const char chType = TS_MeleePlayTextureSound(&tr, gun, end);
		if (chType == CHAR_TEX_FLESH)
			bloody = 1; // a flesh hit blooms the blade immediately (silent — the server owns it)
		else if (chType != 0 && chType != CHAR_TEX_DIRT && chType != CHAR_TEX_WOOD)
		{
			TS_MeleeImpact(tr.endpos, tr.plane.normal, chType, 12); // katana: 12 glints + sparks
			gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
				gEngfuncs.pfnRandomLong(0, 1) ? "weapons/katana/hit.wav" : "weapons/katana/hit1.wav",
				TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
		}
	}

	if ((EV_IsLocal(idx) || TS_InEyeSpectating(idx)) && 0 == CL_IsThirdPerson())
	{
		const s_weapon_info* wi = get_weapon_info(TSW_KATANA); // hardcoded katana row, like the original
		const int swing = animId - 15;
		if (swing >= 0 && swing < 3)
			TS_EvWeaponAnim(wi->iSwingAnims[swing], bloody);
	}
	if (EV_IsLocal(idx))
	{
		// per-swing "bounce-back" screen shake (RE client FUN_1000a270; presentation-only).
		TS_TriggerViewShake(get_weapon_info(TSW_KATANA)->flDamage * 0.15f);
	}

	gEngfuncs.pEventAPI->EV_PopPMStates();
}

// The close combat and coldcock hit puff (client.dll 0x100436ba, 0x10043bbf): on a dirt or wood
// texture wood_smoke_add.spr, on flesh nothing, else gun_smoke_add.spr, through the puff spawner
// FUN_100464f0 (TSP_Puff) at the hit along its normal, rendermode 5, size 12, 30 frames a second.
static void TS_MeleeSmokePuff(pmtrace_t* tr, char chType)
{
	if (chType == CHAR_TEX_FLESH)
		return;
	const char* spr = (chType == CHAR_TEX_WOOD || chType == CHAR_TEX_DIRT) ? "sprites/wood_smoke_add.spr" : "sprites/gun_smoke_add.spr";
	TSP_Puff(tr->endpos, tr->plane.normal, spr, 5, 12, 30);
}

// events/closecombat.sc → EV_TSCloseCombat — the original FUN_10043200 (dossier-ev-melee.md §5).
// Servers: CCombatBlock (iparam1=0) + CCombatAttack (moves 1/2/3). iparam2 = the ANIM-BLOCK index
// (played raw: animBlock[iparam2]); fparam1 = the weaponType as a FLOAT (kung-fu is 0 but an armed
// block carries the gun's type). iparam1 picks the reach: 3 = back move traces 55u BEHIND, 2 =
// 45u forward, else 35u. BLOCK (0) plays player/block.wav and returns — no trace, no effects.
// Otherwise the closecombat.wav whoosh ALWAYS, then the hull-2 trace; a hit adds the texture
// material note + the coldcock smoke puff. Viewmodel anim FIRST, gated local-or-in-eye with NO
// third-person check.
void EV_TSCloseCombat(event_args_t* args)
{
	const int idx = args->entindex;
	const int move = args->iparam1;
	const int animIdx = args->iparam2;
	const int weaponType = (int)args->fparam1;
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);

	if (EV_IsLocal(idx) || TS_InEyeSpectating(idx)) // NO CL_IsThirdPerson gate (§5.1)
	{
		const int anim = (animIdx >= 0 && animIdx < 29 && wi->animBlock[animIdx] >= 0)
			? wi->animBlock[animIdx] : 0;
		TS_EvWeaponAnim(anim, 0);
	}

	if (move == 0)
	{
		// BLOCK: the "hooah" and nothing else (§5.3)
		gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
			"player/block.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
		return;
	}

	Vector fwd, right, up;
	AngleVectors(args->angles, fwd, right, up);
	Vector gun;
	EV_GetGunPosition(args, gun, args->origin);
	const float reach = (move == 3) ? -55.0f : (move == 2) ? 45.0f : 35.0f;
	Vector end;
	for (int i = 0; i < 3; i++)
		end[i] = gun[i] + fwd[i] * reach;

	// the whoosh, ALWAYS (§5.5)
	gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
		"player/closecombat.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(0, 1);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(idx - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(2);
	pmtrace_t tr;
	gEngfuncs.pEventAPI->EV_PlayerTrace(gun, end, PM_STUDIO_BOX, -1, &tr);

	if (tr.fraction < 1.0f)
	{
		const char chType = TS_MeleePlayTextureSound(&tr, gun, end);
		TS_MeleeSmokePuff(&tr, chType); // same puff as coldcock (§5.7)
	}

	gEngfuncs.pEventAPI->EV_PopPMStates();
}

// events/coldcock.sc → EV_TSColdCock — the original LAB_10043800 (dossier-ev-melee.md §4). The
// server (CTSGun::ColdCock) plays it on EVERY bash with iparam1 = the weaponType and iparam2 =
// the ATTACHMENT mask (through the same body math as draw.sc — the gun keeps its attachments
// visible). Viewmodel anim FIRST — the stance block's [28] (our iColdCockAnim column), gated
// local-or-spectated with NO third-person check (unlike knife/katana). 30u forward trace; the
// closecombat.wav whoosh plays ALWAYS (hit or miss, before the trace); a hit adds the texture
// material sound + the smoke puff. No sparks, no weapon hit sound, no bloody logic.
void EV_TSColdCock(event_args_t* args)
{
	const int idx = args->entindex;
	const int weaponType = args->iparam1;
	const int attach = args->iparam2;
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);

	if (EV_IsLocal(idx) || TS_InEyeSpectating(idx)) // NO CL_IsThirdPerson gate (dossier §4.1)
	{
		const int anim = (wi->iColdCockAnim >= 0) ? wi->iColdCockAnim : 0;
		TS_EvWeaponAnim(anim, GetBodyByItems(weaponType, attach));
	}

	Vector fwd, right, up;
	AngleVectors(args->angles, fwd, right, up);
	Vector gun;
	EV_GetGunPosition(args, gun, args->origin);
	Vector end;
	for (int i = 0; i < 3; i++)
		end[i] = gun[i] + fwd[i] * 30.0f;

	// the whoosh, ALWAYS (before the trace — §4.3)
	gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
		"player/closecombat.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());

	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(0, 1);
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetSolidPlayers(idx - 1);
	gEngfuncs.pEventAPI->EV_SetTraceHull(2);
	pmtrace_t tr;
	gEngfuncs.pEventAPI->EV_PlayerTrace(gun, end, PM_STUDIO_BOX, -1, &tr);

	if (tr.fraction < 1.0f)
	{
		const char chType = TS_MeleePlayTextureSound(&tr, gun, end);
		TS_MeleeSmokePuff(&tr, chType);
	}

	gEngfuncs.pEventAPI->EV_PopPMStates();
}

// Where a fire event's gun smoke comes out (the FUN_10047130 call sites in 0x1003dfa0, 0x1003f090,
// 0x10040c20 and 0x100419a0): the shooter's own first-person view takes viewmodel attachment `vmAtt`,
// everyone else player gun anchor `plAtt`, and only for a player drawn this frame (0x100579a0).
// `firstPerson` says which.
static bool TS_SmokeOrigin(int idx, int vmAtt, int plAtt, Vector& pos, bool& firstPerson)
{
	firstPerson = (EV_IsLocal(idx) || TS_InEyeSpectating(idx)) && 0 == CL_IsThirdPerson();
	if (firstPerson)
	{
		cl_entity_t* vm = gEngfuncs.GetViewModel();
		if (!vm)
			return false;
		pos = vm->attachment[vmAtt];
		return true;
	}
	if (!TS_AttachPlayerWasAdded(idx))
		return false;
	const float* att = TS_PlayerGunAtt(idx, plAtt);
	pos = Vector(att[0], att[1], att[2]);
	return true;
}

// The fire.sc / firelinked.sc gun smoke, drifting with a tenth of the shooter's velocity: a pump
// shotgun (0x10040c20) a slow puff, 16 in first person and 24 seen from outside; a silenced shot
// (attach bit 0) a size 8 wisp along the aim and a forced size 1 trail off the eject port (the
// attachment less the aim x the shell setback) to the right; else a 16 puff, slow for the sawed-off
// (0x1003dfa0 only, not the akimbo path 0x1003f090). Then a shotgun's muzzle debris.
static void TS_FireSmoke(event_args_t* args, const s_weapon_info* wi, int weaponType, int vmAtt, int plAtt,
	bool silenced)
{
	Vector pos;
	bool firstPerson;
	if (!TS_SmokeOrigin(args->entindex, vmAtt, plAtt, pos, firstPerson))
		return;
	Vector angles, fwd, right, up, vel;
	VectorCopy(args->angles, angles);
	AngleVectors(angles, fwd, right, up);
	for (int i = 0; i < 3; i++)
		vel[i] = args->velocity[i] / 10.0f;
	const bool akimbo = (wi->iFireType == TSFT_AKIMBO || wi->iFireType == TSFT_AKIMBO_ONLY);
	if (wi->iFireType == TSFT_PUMP)
		TSP_MuzzleSmoke(firstPerson ? 0x10 : 0x18, gEngfuncs.pfnRandomLong(5, 10), pos, vel, fwd, 24.0f, false);
	else if (silenced)
	{
		TSP_MuzzleSmoke(8, gEngfuncs.pfnRandomLong(0x32, 0x3c), pos, vel, fwd, 24.0f, false);
		TSP_MuzzleSmoke(1, gEngfuncs.pfnRandomLong(0x32, 0x50), pos - fwd * (float)wi->iShellSetback, vel, right,
			4.0f, true);
	}
	else
	{
		const int frameRate = (!akimbo && weaponType == TSW_SAWEDOFF) ? gEngfuncs.pfnRandomLong(5, 10)
		                                                              : gEngfuncs.pfnRandomLong(0x2d, 0x37);
		TSP_MuzzleSmoke(0x10, frameRate, pos, vel, fwd, 24.0f, false);
	}
	// After the smoke, the shotgun muzzle debris: 0x1003dfa0 for a weapon with more than one pellet
	// (0x1003e57b in first person, 0x1003ece3 seen from outside), the pump fire 0x10040c20 always
	// (0x1004104e, 0x10041590); the akimbo path 0x1003f090 has none. It comes from the shooter's player
	// gun anchor 0 in first person and 3 from outside (0x10056be0), not the view model.
	if (!akimbo && (wi->iFireType == TSFT_PUMP || wi->iPellets > 1))
	{
		const float* att = TS_PlayerGunAtt(args->entindex, firstPerson ? 0 : 3);
		TSP_MuzzleDebris(Vector(att[0], att[1], att[2]), fwd, wi->iPellets);
	}
}

// ===================== the gun-tail event handlers (dossier-ev-guntail.md) =====================

// The stance-aware anim block (the original FUN_1003df60: the ACTIVE stance's wi+0x252 normal /
// +0x28c hold set). The hold state is the LOCAL predicted weapon's — the original reads its own
// client cache the same way.
bool TS_LocalHoldPosition(); // cl_dll/hl/hl_weapons.cpp — g_TSGun.m_bHoldPosition
static int TS_BlockAnim(const s_weapon_info* wi, int idx)
{
	if (idx < 0 || idx >= 29)
		return 0;
	if (TS_LocalHoldPosition() && wi->animBlockHold[idx] >= 0)
		return wi->animBlockHold[idx];
	return (wi->animBlock[idx] >= 0) ? wi->animBlock[idx] : 0;
}

// events/empty.sc → EV_TSEmpty — FUN_100424e0 faithful (182 bytes): ONE universal dry-click for
// every gun — weapons/pistol-empty.wav (HYPHENATED), CHAN_WEAPON, vol RF(0.92,1), attn 0.8,
// the timescale melee pitch. No anim, no locality gate; the contender's silence is server-side.
void EV_TSEmpty(event_args_t* args)
{
	gEngfuncs.pEventAPI->EV_PlaySound(args->entindex, args->origin, CHAN_WEAPON,
		"weapons/pistol-empty.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
}

// events/reload.sc → EV_TSReload — FUN_10042690 faithful. iparam1=weaponType, iparam2=attach
// mask, fparam1 = the PHASE, which IS the anim-block index (0 whole-clip -> block[1]; 8 pump
// open / 9 shell insert / 10 close play block[8/9/10]). Phase 9 shucks insert-shell.wav for
// EVERYONE (the local player too); phase 10 plays the weapon's own rack/close sound (wi+0x306)
// for NON-local viewers only; phase 0 plays generic_reload.wav + the spent brass/magazine toss
// for NON-local viewers only (the local player's reload foley rides the viewmodel anim events).
void EV_TSReload(event_args_t* args)
{
	const int idx = args->entindex;
	const int weaponType = args->iparam1;
	const int attach = args->iparam2;
	const int phase = (int)args->fparam1;
	if (weaponType < 0 || weaponType >= TSW_COUNT)
		return;
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);
	const bool localish = EV_IsLocal(idx) || TS_InEyeSpectating(idx);

	if (localish) // viewmodel anim FIRST (no 3rd-person gate in the original)
		TS_EvWeaponAnim(TS_BlockAnim(wi, phase == 0 ? 1 : phase),
			GetBodyByItems(weaponType, attach));

	if (phase == 9)
	{
		// the per-shell shuck — heard by everyone, the loader included
		gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
			"weapons/insert-shell.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
		return;
	}
	if (phase == 10)
	{
		if ((CL_IsThirdPerson() || !localish) && wi->pszAltSound)
			gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
				wi->pszAltSound, TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
		return;
	}
	if (phase != 0)
		return;

	// phase 0 — the whole-clip reload, NON-local presentation only
	if (!CL_IsThirdPerson() && localish)
		return;
	gEngfuncs.pEventAPI->EV_PlaySound(idx, args->origin, CHAN_WEAPON,
		"weapons/generic_reload.wav", TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());

	cl_entity_t* pl = gEngfuncs.GetEntityByIndex(idx);
	if (!pl)
		return;
	Vector fwd, right, up, angles, vel;
	VectorCopy(args->angles, angles);
	VectorCopy(args->velocity, vel);
	AngleVectors(angles, fwd, right, up);

	// the spent brass / magazine toss (@dossier §2.4)
	if (weaponType == TSW_SAWEDOFF || weaponType == TSW_BULL || weaponType == TSW_CONTENDER)
	{
		const char* mdl = (weaponType == TSW_SAWEDOFF) ? "models/shotgun_shell.mdl"
			: (weaponType == TSW_BULL) ? "models/9mm_shell.mdl" : "models/556_shell.mdl";
		const int n = (weaponType == TSW_SAWEDOFF) ? 2 : (weaponType == TSW_BULL) ? 5 : 1;
		const int bounce = (weaponType == TSW_SAWEDOFF) ? TE_BOUNCE_SHOTSHELL : TE_BOUNCE_SHELL;
		const int shell = gEngfuncs.pEventAPI->EV_FindModelIndex(mdl);
		for (int k = 0; k < n && shell; k++)
		{
			Vector sv;
			for (int c = 0; c < 3; c++)
				sv[c] = vel[c] + right[c] * gEngfuncs.pfnRandomLong(-30, 30) + fwd[c] * 10.0f +
					up[c] * gEngfuncs.pfnRandomLong(0, 30);
			const float* ba = TS_PlayerGunAtt(idx, 0);
			Vector brassPos(ba[0], ba[1], ba[2]);
			EV_EjectBrass(brassPos, sv, angles[1], shell, bounce);
		}
	}
	else
	{
		// the magazine(s): akimbo classes drop one each side, silent bounce, 2s life
		const int mag = gEngfuncs.pEventAPI->EV_FindModelIndex("models/generic_clip.mdl");
		const int n = (wi->iFireType == TSFT_AKIMBO || wi->iFireType == TSFT_AKIMBO_ONLY) ? 2 : 1;
		for (int k = 0; k < n && mag; k++)
		{
			Vector pos, mv, mang;
			for (int c = 0; c < 3; c++)
			{
				pos[c] = TS_PlayerGunAtt(idx, k == 0 ? 0 : 2)[c] - fwd[c] * 8.0f;
				mv[c] = vel[c] + right[c] * (k == 0 ? 60.0f : -60.0f);
				mang[c] = gEngfuncs.pfnRandomFloat(-60.0f, 60.0f);
			}
			TEMPENTITY* te = gEngfuncs.pEfxAPI->R_TempModel(pos, mv, mang, 2.0f, mag, 0 /*silent*/);
			(void)te;
		}
	}
}

// events/pump.sc → EV_TSPump — FUN_100419a0 faithful (the rack half of the pump cycle).
// iparam1=weaponType, iparam2=attach. Anim = block[13+RandomLong(0,1)] (client-random) for the
// local/spectated viewer; a shell ejects from the vm eject port (attachment 0 − fwd·setback)
// first-person or player attachment 3 third-person (a player drawn this frame), each with a thin
// smoke trail from the port; the RACK SOUND (wi+0x306 — m3-pump /
// spas12-pump) plays for EVERYONE at ent −1 so it never channel-cuts the still-playing gunshot.
void EV_TSPump(event_args_t* args)
{
	const int idx = args->entindex;
	const int weaponType = args->iparam1;
	const int attach = args->iparam2;
	if (weaponType < 0 || weaponType >= TSW_COUNT)
		return;
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);
	const bool localish = EV_IsLocal(idx) || TS_InEyeSpectating(idx);
	const int shell = wi->pszShellModel ? gEngfuncs.pEventAPI->EV_FindModelIndex(wi->pszShellModel) : 0;
	const float r1 = (float)gEngfuncs.pfnRandomLong(60, 80);
	const float r2 = (float)gEngfuncs.pfnRandomLong(50, 100);

	Vector fwd, right, up, angles, vel;
	VectorCopy(args->angles, angles);
	VectorCopy(args->velocity, vel);
	AngleVectors(angles, fwd, right, up);

	if (localish)
	{
		TS_EvWeaponAnim(
			TS_BlockAnim(wi, 13 + gEngfuncs.pfnRandomLong(0, 1)), GetBodyByItems(weaponType, attach));
		if (0 == CL_IsThirdPerson() && shell)
		{
			cl_entity_t* vm = gEngfuncs.GetViewModel();
			Vector pos, sv;
			for (int c = 0; c < 3; c++)
			{
				pos[c] = (vm ? vm->attachment[0][c] : args->origin[c]) - fwd[c] * (float)wi->iShellSetback;
				sv[c] = vel[c] + fwd[c] * r1 + up[c] * r2 + right[c] * r1;
			}
			EV_EjectBrass(pos, sv, angles[1], shell, TE_BOUNCE_SHOTSHELL);
			// the forced size 1 smoke trail off the eject port, to the right (0x10041f26)
			if (vm)
				TSP_MuzzleSmoke(1, gEngfuncs.pfnRandomLong(0x14, 0x28), pos, vel / 10.0f, right, 4.0f, true);
		}
	}
	// seen from outside only for a player drawn this frame (0x100420e2)
	if ((CL_IsThirdPerson() || !localish) && shell && TS_AttachPlayerWasAdded(idx))
	{
		cl_entity_t* pl = gEngfuncs.GetEntityByIndex(idx);
		if (pl)
		{
			Vector pos, sv;
			for (int c = 0; c < 3; c++)
			{
				pos[c] = TS_PlayerGunAtt(idx, 3)[c] - fwd[c] * (float)wi->iShellSetback;
				sv[c] = vel[c] + fwd[c] * r1 + up[c] * r2 + right[c] * r1;
			}
			EV_EjectBrass(pos, sv, angles[1], shell, TE_BOUNCE_SHOTSHELL);
			TSP_MuzzleSmoke(1, gEngfuncs.pfnRandomLong(0x14, 0x28), pos, vel / 10.0f, right, 4.0f, true); // 0x100423be
		}
	}
	// the rack — ent −1, EVERY viewer (the no-channel-cut trick; replaces our CHAN_ITEM hack)
	if (wi->pszAltSound)
		gEngfuncs.pEventAPI->EV_PlaySound(-1, args->origin, CHAN_WEAPON, wi->pszAltSound,
			TS_MeleeVolume(), 0.8f, 0, TS_MeleeSoundPitch());
}

// events/grenade.sc → EV_TSGrenadeEv — FUN_10042630 faithful (91 bytes): the IN-EYE SPECTATOR's
// view of the pin/throw ONLY (the thrower's own anim is the predicted SendWeaponAnim, like
// draw.sc). iparam1 = the anim-block index (19 pin / 20 overhand / 21 underhand); wi hardcoded
// to the m61; body hardcoded 1.
void EV_TSGrenadeEv(event_args_t* args)
{
	if (TS_InEyeSpectating(args->entindex) && !EV_IsLocal(args->entindex))
		TS_EvWeaponAnim(
			TS_BlockAnim(get_weapon_info(TSW_M61), args->iparam1), 1);
}


void EV_TSFire(event_args_t* args)
{
	const int idx = args->entindex;
	const int weaponType = args->iparam1;
	const int attachMask = args->iparam2;  // RE StandardFire@0x824a4: iparam2 = active item attach mask
	if (weaponType < 0 || weaponType >= TSW_COUNT)
		return;
	// ORIGINAL wire: bparam1 = the barrel (0 right / 1 left) — but ONLY for the akimbo classes;
	// the standard classes pack (clip==0) there. bparam2 = the FireBullets seed (the old
	// bparam2=side extension collided with it — guntail dossier §5).
	const s_weapon_info* wiClass = get_weapon_info((tsweapon_t)weaponType);
	const bool akimboClass = (wiClass->iFireType == TSFT_AKIMBO || wiClass->iFireType == TSFT_AKIMBO_ONLY);
	const int barrelSide = akimboClass ? args->bparam1 : 0;

	// Silencer (attach bit0). The original client gates the muzzle flash on `iparam2 & 1` (FUN_1003dfa0:
	// `(DAT_10189d10 & 1)`) and swaps the fire sound to the weapon's `-sil` variant. A weapon with no
	// `-sil` entry (s_fireSilSound[type] == nullptr) keeps its normal fire sound (silenced-base mp5sd,
	// or a weapon that can't take a silencer).
	const bool silenced = (attachMask & 1) != 0;

	g_tsActive.UpdateFromEvent(args); // refresh the active-weapon cache (matches the original fire handler)

	Vector origin;
	VectorCopy(args->origin, origin);

	// Muzzle flash sprite at the barrel — ALWAYS shown; silenced fire just uses the dimmer gun_muzzle.spr
	// (vs gun_muzzle2.spr). The bright dynamic light below is still skipped when silenced.
	// The flare sprite ALWAYS draws at the fired barrel (the original's fire core does — the
	// engine-side R_MuzzleFlash doubles from the models' embedded 5001-family events are
	// suppressed in HUD_StudioEvent instead; see entity.cpp).
	EV_TSMuzzleFlare(idx, weaponType, barrelSide, silenced);
	// (the both-barrels shot is a SEPARATE event — events/firelinked.sc → EV_TSFireLinked)

	// The AKIMBO single-barrel viewmodel anim comes from the EVENT (the fire core). The MODELS
	// settle the side mapping (the dossier's labels were swapped): block[22/23] = the RIGHT-gun
	// shoots (v_mk23_akimbo 1/2 = shoot_right1/2, v_berettas 3/5 = rightshoot), block[24/25] =
	// the LEFT (3/4 = shoot_left1/2, 4/6 = leftshoot). bparam1: 1 = the LEFT barrel.
	if (akimboClass && (EV_IsLocal(idx) || TS_InEyeSpectating(idx)))
		TS_EvWeaponAnim(
			TS_BlockAnim(wiClass, (barrelSide ? 24 : 22) + gEngfuncs.pfnRandomLong(0, 1)),
			GetBodyByItems(weaponType, attachMask));
	// The other classes' shoot anim comes from the event too, for the shooter and an in-eye spectator:
	// a pump shotgun block[11], block[12] for the last round (0x10040f65), the rest block[3 + RandomLong(0, 2)],
	// block[6] for the last round (0x1003e329); bparam1 = (clip == 0).
	else if (!akimboClass && (EV_IsLocal(idx) || TS_InEyeSpectating(idx)))
	{
		int blk;
		if (wiClass->iFireType == TSFT_PUMP)
			blk = args->bparam1 ? 12 : 11;
		else
			blk = args->bparam1 ? 6 : 3 + gEngfuncs.pfnRandomLong(0, 2);
		TS_EvWeaponAnim(TS_BlockAnim(wiClass, blk), GetBodyByItems(weaponType, attachMask));
	}

	if (EV_IsLocal(idx))
	{
		if (!silenced)
			EV_MuzzleFlash(); // the dynamic light / model flash on top of the sprite
		// The aim kick is the prediction's (TS_AddViewRecoil from CTSGun's fire, as client.dll's fire
		// calls 0x10008230).
	}

	// Eject a brass shell from the viewmodel/player.
	Vector angles, velocity, fwd, right, up, shellVel, shellOrigin;
	VectorCopy(args->angles, angles);
	VectorCopy(args->velocity, velocity);
	AngleVectors(angles, fwd, right, up);
	const int shell = gEngfuncs.pEventAPI->EV_FindModelIndex("models/shell.mdl");
	EV_GetDefaultShellInfo(args, origin, velocity, shellVel, shellOrigin, fwd, right, up, 20, -12, 4);
	EV_EjectBrass(shellOrigin, shellVel, angles[1], shell, TE_BOUNCE_SHELL);

	// The gun smoke at the fired barrel: client.dll's fire.sc handler sends an akimbo class to
	// 0x1003f090 (the barrel's attachment, viewmodel and player alike), a pump shotgun to 0x10040c20
	// (viewmodel 0, player 3) and the rest to 0x1003dfa0 (viewmodel 0, player 3 for fire types 6 and 7,
	// else 0).
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);
	if (akimboClass)
		TS_FireSmoke(args, wi, weaponType, barrelSide ? 2 : 0, barrelSide ? 2 : 0, silenced);
	else if (wi->iFireType == TSFT_PUMP)
		TS_FireSmoke(args, wi, weaponType, 0, 3, silenced);
	else
		TS_FireSmoke(args, wi, weaponType, 0,
			(wi->iFireType == TSFT_AUTOSHOTGUN || wi->iFireType == TSFT_SNIPER) ? 3 : 0, silenced);

	// The rounds, through the fire core from the gun position along the aim.
	{
		Vector vecSrc;
		EV_GetGunPosition(args, vecSrc, origin);
		TS_FireCore(idx, fwd, right, up, (unsigned char)wi->iPellets, vecSrc, fwd, args->fparam1, args->fparam2,
			wi->flDamage, wi->iKnifeType, (unsigned char)wi->iCaliber, args->bparam2);
	}

	// The silenced flag = iparam2 & 1 (dossier-attachments §1b/§5 — the fire-sound alt branch
	// keys on the SILENCER bit, not on linked): silenced -> wi+0x306 (the -sil sample) at vol
	// RF(0.84,0.92) and DOUBLED attenuation; unsilenced -> the normal sample.
	const s_weapon_info* swi = get_weapon_info((tsweapon_t)weaponType);
	const bool silShot = silenced && swi->pszAltSound;
	const char* snd = silShot ? swi->pszAltSound : s_fireSound[weaponType];
	if (snd)
	{
		// RE (scratchpad/SLOWMO_PRESENTATION_RE.md): the original pitches ALL weapon-fire sounds UP —
		// pitch = 100 + (int)(300 * slow), where slow is the local client's smoothed slow factor
		// (1.0 normal → pitch 400). Slow-motion lowers `slow` → the gunshots deepen toward 100.
		// Passed unclamped, as the original does: neither GoldSrc's S_StartDynamicSound nor Xash caps
		// pitch, so 400 plays at 4x. A cap at 255 played normal fire at 2.55x, audibly slowed.
		extern float g_flClientSlow; // cl_dll/ts_powerup_hud.cpp — current smoothed slow factor
		const int pitch = 100 + (int)(300.0f * g_flClientSlow);
		gEngfuncs.pEventAPI->EV_PlaySound(idx, origin, CHAN_WEAPON, snd,
			silShot ? gEngfuncs.pfnRandomFloat(0.84f, 0.92f) : VOL_NORM,
			silShot ? ATTN_NORM * 2.0f : ATTN_NORM, 0, pitch);
	}
}

// events/firelinked.sc → EV_TSFireLinked — FUN_1003de10 -> the fire core with linked=1 (guntail
// dossier §5): the akimbo BOTH-BARRELS shot. iparam1=weaponType, iparam2=attach, fparam1/2 =
// spread, bparam2 = the FireBullets seed. Divergences from fire.sc: anim = block[3+RL(0,2)]
// (the both-barrel set); muzzle flare at BOTH attachments; TWO casings; ONE bang — wi+0x306
// (the linked sample, vol RF(0.84,0.92), attn DOUBLED); two fire-core volleys at gunpos ± right·5.
void EV_TSFireLinked(event_args_t* args)
{
	const int idx = args->entindex;
	const int weaponType = args->iparam1;
	const int attachMask = args->iparam2;
	if (weaponType < 0 || weaponType >= TSW_COUNT)
		return;
	const s_weapon_info* wi = get_weapon_info((tsweapon_t)weaponType);
	const bool silenced = (attachMask & 1) != 0;
	g_tsActive.UpdateFromEvent(args);

	Vector origin, angles, velocity, fwd, right, up;
	VectorCopy(args->origin, origin);
	VectorCopy(args->angles, angles);
	VectorCopy(args->velocity, velocity);
	AngleVectors(angles, fwd, right, up);

	// BOTH muzzle flares
	EV_TSMuzzleFlare(idx, weaponType, 0, silenced);
	EV_TSMuzzleFlare(idx, weaponType, 1, silenced);

	if (EV_IsLocal(idx) || TS_InEyeSpectating(idx))
	{
		TS_EvWeaponAnim(
			TS_BlockAnim(wi, 3 + gEngfuncs.pfnRandomLong(0, 2)), GetBodyByItems(weaponType, attachMask));
		if (EV_IsLocal(idx))
		{
			if (!silenced)
				EV_MuzzleFlash();
		}
	}

	// TWO casings — the skorpion family throws its brass gently sideways (RL(-40,40) vs 120..140)
	const int shell = wi->pszShellModel ? gEngfuncs.pEventAPI->EV_FindModelIndex(wi->pszShellModel) : 0;
	if (shell)
	{
		const float r2 = (float)gEngfuncs.pfnRandomLong(50, 100);
		const bool skorp = (wi->iSingleType == TSW_SKORPION);
		for (int b = 0; b < 2; b++)
		{
			const float r1 = skorp ? (float)gEngfuncs.pfnRandomLong(-40, 40)
			                       : (float)gEngfuncs.pfnRandomLong(120, 140);
			Vector pos, sv;
			const bool vmView = (EV_IsLocal(idx) && 0 == CL_IsThirdPerson());
			cl_entity_t* src = vmView ? gEngfuncs.GetViewModel() : NULL;
			for (int c = 0; c < 3; c++)
			{
				const float* att = (src ? (const float*)src->attachment[b == 0 ? 0 : 2]
				                        : TS_PlayerGunAtt(idx, b == 0 ? 0 : 2));
				pos[c] = att[c] - fwd[c] * (float)wi->iShellSetback;
				sv[c] = velocity[c] + fwd[c] * (r2 + 50.0f) + up[c] * r2 + right[c] * r1;
			}
			EV_EjectBrass(pos, sv, angles[1], shell, TE_BOUNCE_SHELL);
		}
	}

	// One gun smoke, at the attachment of the barrel bparam1 names, as for a single akimbo shot
	// (0x1003f090 reads the barrel for the smoke on the linked shot too).
	const int smokeAtt = args->bparam1 ? 2 : 0;
	TS_FireSmoke(args, wi, weaponType, smokeAtt, smokeAtt, silenced);

	// The rounds, through the fire core from the gun position 5 units to the right (the event's seed)
	// and 5 to the left (the seed + 33), along the aim (0x10040a61, 0x10040b0c).
	{
		Vector vecSrc;
		EV_GetGunPosition(args, vecSrc, origin);
		for (int b = 0; b < 2; b++)
		{
			const Vector src2 = vecSrc + right * (b == 0 ? 5.0f : -5.0f);
			TS_FireCore(idx, fwd, right, up, (unsigned char)wi->iPellets, src2, fwd, args->fparam1, args->fparam2,
				wi->flDamage, wi->iKnifeType, (unsigned char)wi->iCaliber, args->bparam2 + (b == 0 ? 0 : 0x21));
		}
	}

	// ONE bang for both barrels. The alt-sample branch keys on the SILENCER bit (iparam2&1),
	// NOT on linked (dossier-attachments §1b corrects cont83 §5.4): silenced -> wi+0x306 at
	// RF(0.84,0.92) + attn x2; unsilenced linked -> the NORMAL sample at normal vol/attn
	// (the round-7b "akimbo double-fire sounds silenced" report was this).
	const bool silShot = silenced && wi->pszAltSound;
	const char* snd = silShot ? wi->pszAltSound : s_fireSound[weaponType];
	if (snd)
	{
		extern float g_flClientSlow;
		const int pitch = 100 + (int)(300.0f * g_flClientSlow); // unclamped, as EV_TSFire
		gEngfuncs.pEventAPI->EV_PlaySound(idx, origin, CHAN_WEAPON, snd,
			silShot ? gEngfuncs.pfnRandomFloat(0.84f, 0.92f) : VOL_NORM,
			silShot ? ATTN_NORM * 2.0f : ATTN_NORM, 0, pitch);
	}
}

