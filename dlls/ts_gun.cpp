/***
*  ts_gun.cpp — RECONSTRUCTED CTSGun (recovered/server/CTSGun.c).
*  Functional/source-faithful goal (see CLAUDE.md).
*
*  CTSGun is the original's single unified weapon (entity `weapon_tsgun`, id 16): the player's whole
*  arsenal, one item per tsweapon_t (m_items, the original's items[]), with the held one in
*  m_weaponType and its item and table row in m_pActiveItem / m_pActiveInfo (gun+0xcc / +0xd0). Fire
*  dispatches on the row's fire-type byte (s_weapon_info+0x348); the client predicts the same code.
***/
#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "ts_weapons.h"
#include "ts_bullet.h"
#include "ts_player.h" // DoubleFireRate (powerup accessor)
#ifndef CLIENT_DLL
#include "ts_gamerules.h" // VictimKatanaHold/VictimKnifeHold (the melee block gates)
#include "ts_powerup.h"   // CTSPowerUp::CreatePowerUp (a dead player's grenades)
#endif
#include "ts_worldgun.h" // CTSWorldGun::CreateWeapon / CTSAmmoPack::CreateAmmoPack (drop side)
#include "ts_shared.h"   // FixedSlowMul (DropKickback)

// WEAPON TIMING: the build defines CLIENT_WEAPONS (both targets), so UTIL_WeaponTimeBase() == 0 — a
// 0-based RELATIVE clock that gets DECREMENTED each frame by gpGlobals->frametime, on the CLIENT by
// the prediction engine and on the SERVER by CBasePlayer::ItemPostFrame's decrement loop (player.cpp,
// `#if defined(CLIENT_WEAPONS)` + `gun->UseDecrement()`). CTSGun is now CLIENT-PREDICTED (UseDecrement()
// == true), so its attack/idle/reload timers use UTIL_WeaponTimeBase() like every predicted weapon —
// set/compared against that base, decremented on both sides, consistent for prediction.
// (cont.69q earlier used absolute gpGlobals->time as a workaround for "fire-once-stuck"; that was only
// needed because UseDecrement() was false → the server decrement loop above skipped CTSGun. With
// UseDecrement()==true the server decrements it, so the 0-base elapses correctly. See EVENTS.md.)
// NOTE: real-time DELTAS (m_lastCCTime combo window, melee-bar regen now in PreThink) keep gpGlobals->time —
// they measure wall-clock elapsed, not a decremented weapon timer.

// The single weapon_tsgun (id 16) IS the player's whole TS arsenal. The original
// keeps an `s_weapon_status items[]` array INDEXED BY tsweapon_t (0x38-byte entries
// at gun+type*0x38+0x114), with m_pActiveItem(@0xcc)/m_pActiveInfo(@0xd0) pointing at
// the wielded one + per-item akimbo barrel clips. This is a clean MODERN reconstruction
// of that layout (recovered from TakeWeapon@0x80a04 / DropWeapon@0x84900 /
// SwitchItems@0x81564 / GetBetterWeapon@0x80f40 — see reference/WEAPON_SELECT.md):
// one TSItem per tsweapon_t. The faithful akimbo OWNERSHIP model — a single and its
// akimbo are SEPARATE owned items (combine keeps both), the akimbo carries the two
// barrel clips, and a single can be wielded/dropped independently of its akimbo pair.

#include "ts_gun.h" // the CTSGun class (extracted so the client prediction harness can instantiate it)
#ifdef CLIENT_DLL
#include "../cl_dll/ts_recoil.h" // TS_TriggerViewShake and TS_AddViewRecoil (client only)
extern bool g_runfuncs;          // cl_dll/hl/hl_weapons.cpp: this run of the command is its first
#endif

// TS weapon-info HUD message (the ammo readout) — registered centrally in LinkUserMessages
// (UserMessages.cpp): "WeaponInfo", 6. GLOBALS like the original's (TS_gun.cpp home).
int gmsgWeaponInfo = 0;
// The weapon-select bar message: "WStatus", size 3 = BYTE type, BYTE owned, BYTE clip.
int gmsgTSWeaponStatus = 0;
int gmsgTSActiveItems = 0; // per-player attach broadcast (laser/flashlight visible to all clients)
// gmsgKFuPower moved to ts_player.cpp — sent ONLY from CBasePlayer::UpdateMeleeBar (extern via ts_player.h)

#ifdef CLIENT_DLL
// `damagemult` cvar (default 0.0 = off → 1x) for the client's predicted bullets, whose damage is
// never applied. On the server only ShootBulletsPlayer and UpdateBullet read it, cheat-gated.
static float TSDamageMult()
{
	const float m = CVAR_GET_FLOAT("damagemult");
	return (m > 0.0f) ? m : 1.0f;
}
#endif

// The world slow the player PERCEIVES (drives sound pitch): their applied dilation pev->fuser1, or the
// others-scale ramp while they're the slow-motion activator. 1.0 normal, ~0.35 slow-mo, ~0.225 slow-pause.
static float TSPerceivedSlow(CBasePlayer *p)
{
	float s = p->pev->fuser1;
	if ((p->m_iPowerups & 9) && p->m_flSlowMotionRamp > 0.0f && p->m_flSlowMotionRamp < s)
		s = p->m_flSlowMotionRamp; // slowmo 0.35 / slowpause 0.225
	return s;
}

// NORMAL-recorded combat sounds (close-combat/knife/katana, thrown-knife impacts) sit at 100 normally and
// are SLOWED (lower pitch) in slow-motion — never sped up. pitch = 100 * perceivedSlow, capped at 100.
int TSSlowPitch(CBasePlayer *p)
{
	int pitch = (int)(100.0f * TSPerceivedSlow(p));
	return pitch < 1 ? 1 : (pitch > 100 ? 100 : pitch);
}

// RECORDED-SLOW sounds (the gun-fire WAVs + bullet-hit ric are recorded already slowed) must be pitched UP
// for normal play and only deepen toward natural in slow-motion. pitch = 100 + 300 * perceivedSlow (the
// same formula the client uses for gunshots in ev_ts.cpp): normal ~400→clamp 255, slow-mo ~205. 1..255.
int TSRecordedSlowPitch(CBasePlayer *p)
{
	int pitch = (int)(100.0f + 300.0f * TSPerceivedSlow(p));
	return pitch < 1 ? 1 : (pitch > 255 ? 255 : pitch);
}

LINK_ENTITY_TO_CLASS(weapon_tsgun, CTSGun);

// @ GetWeaponType__F10tsweapon_t @0x7705c — the weapon's restriction CATEGORY code:
// the fire type, with the special cases 34 katana -> 3 (knife cat), 33 sawed-off -> 1
// (shotgun), 16/37 akimbo SMGs -> 9, 10/30/8/21 akimbo pistols -> 8.
char GetWeaponType(tsweapon_t w)
{
	switch (w)
	{
	case TSW_KATANA: return 3;
	case TSW_SAWEDOFF: return 1;
	case TSW_UZI_AKIMBO:
	case TSW_SKORPION_AKIMBO: return 9;
	case TSW_MK23_AKIMBO:
	case TSW_FNH_AKIMBO:
	case TSW_BERETTA_AKIMBO:
	case TSW_GOLD: return 8;
	default: return (char)get_weapon_info(w)->iFireType;
	}
}

// @ IsRestrictedWeapon__F10tsweapon_ti @0x770c8 — true = the weapon is NOT purchasable
// under the allowed-categories mask (mask 0 = everything restricted; categories:
// knives 0x1, pistols 0x2, SMGs 0x4, rifles 0x8, shotguns 0x10; anything uncategorized
// is never restricted).
bool IsRestrictedWeapon(tsweapon_t w, int mask)
{
	if (w < 1 || w > 37)
		return false;
	if (mask == 0)
		return true;
	switch (GetWeaponType(w))
	{
	case 1: return (mask & 0x10) == 0; // shotguns
	case 3: return (mask & 0x1) == 0;  // knives
	case 6:
	case 7: return (mask & 0x8) == 0;  // rifles
	case 8: return (mask & 0x2) == 0;  // pistols
	case 9: return (mask & 0x4) == 0;  // SMGs
	default: return false;
	}
}

// @ IsWeaponAkimbo__Fi @0x7019c — fire-type 4 (the combinable akimbo pairs).
bool IsWeaponAkimbo(int type)
{
	return get_weapon_info((tsweapon_t)type)->iFireType == TSFT_AKIMBO;
}

// @ IsOnlyAkimbo__Fi @0x701d4 — fire-type 5 (akimbo-only, e.g. the golden colts).
bool IsOnlyAkimbo(int type)
{
	return get_weapon_info((tsweapon_t)type)->iFireType == TSFT_AKIMBO_ONLY;
}

// @ CalcSpread__FRfP11CBasePlayerf @0x7020c — velocity-scaled spread multiplier:
// spread *= clamp(|vel| / (100/f), 1, 3f). Nothing in ts_i386.so calls it (ScaleSpread is the
// live one); kept compiled-and-uncalled for source parity. Whether client.dll calls its copy is
// the client's question.
void CalcSpread(float &spread, CBasePlayer *pPlayer, float f)
{
	float mul = pPlayer->pev->velocity.Length() / (100.0f / f);
	if (mul < 1.0f)
		mul = 1.0f;
	if (mul > 3.0f * f)
		mul = 3.0f * f;
	spread *= mul;
}

// @ DropKickback__FPfT0ff @0x76ea0 — 2-axis kick decay toward the align vector, step
// 15*amt/FixedSlowMul(slowFactor). DEAD server-side (the producer VKickback is live but
// never calls it in the .so); source-parity only.
void DropKickback(float *kick, float *align, float amt, float slowFactor)
{
	if (amt < 0.0f)
		return;
	const float klen = sqrtf(kick[0] * kick[0] + kick[1] * kick[1]);
	if (klen <= 0.0f)
		return;
	const float d0 = kick[0] - align[0];
	const float d1 = kick[1] - align[1];
	const float dlen = sqrtf(d0 * d0 + d1 * d1);
	const float step = 15.0f * (amt / FixedSlowMul(slowFactor));
	// literal transcription: the align axes advance by d*step when the step fits inside the
	// distance, else by d*dlen (the original's own asymmetric else-branch — kept as-is).
	align[0] += (step <= dlen) ? d0 * step : d0 * dlen;
	align[1] += (step <= dlen) ? d1 * step : d1 * dlen;
	kick[0] /= klen;
	kick[1] /= klen;
	float rem = klen - step;
	if (rem < 0.0f)
		rem = 0.0f;
	kick[0] *= rem;
	kick[1] *= rem;
}

// @ GetCloseCombatVectors__FiR6VectorN31 @0x76e2c — out = src + 32*forward (first + last
// args unused). DEAD in the original (the math re-inlined in KnifeFire); source parity.
void GetCloseCombatVectors(int a0, Vector &vecSrc, Vector &vecOut, Vector &vecForward, Vector &a4)
{
	(void)a0;
	(void)a4;
	vecOut = vecSrc + vecForward * 32.0f;
}



// @ g_iWeaponPriority (filled in compile_weapons_info@0x6fe28) — the auto-select order
// GetBetterWeapon walks after a drop (highest priority first). Last entry (kung-fu, 0)
// is the always-owned fallback. Recovered verbatim from the .so.
static const int g_iWeaponPriority[TSW_COUNT] = {
	18, 32, 13, 5, 15, 27, 11, 20, 4, 26, 33, 16, 37, 6, 23, 7, 3, 17, 19,
	10, 8, 21, 30, 31, 12, 36, 22, 9, 2, 14, 28, 1, 34, 25, 35, 24, 29, 0,
};

// The one weapon_tsgun in a player's inventory (the whole TS arsenal), or NULL.
CTSGun *GetPlayerTSGun(CBasePlayer *pPlayer)
{
	for (int i = 0; i < MAX_ITEM_TYPES; i++)
		for (CBasePlayerItem *p = pPlayer->m_rgpPlayerItems[i]; p; p = p->m_pNext)
			if (FClassnameIs(p->pev, "weapon_tsgun"))
				return (CTSGun *)p;
	return NULL;
}

// INTERIM (fold-plan Y2): the original has NO slots recompute anywhere — TSSpace is a STORED
// counter (player+0x548, init 81 in TSInit @0x82a4c) mutated inline in PickUpWeapon @0x8314c
// (cost = info+0xd0 x units, newSpace < 0 -> refuse) and the drop path; GetSpeedBySlots @0x66bd4
// just READS it. Our self-healing recompute (derives used slots from the owned set) is demoted
// to this file-static helper until the stored-counter swap lands with the buy/pickup pass
// (counter-drift risk; ledger). Sum of slotCost over owned items = 81 - player+0x548.
static int TSSlotsUsed(const CTSGun *pGun)
{
	int used = 0;
	for (int t = 0; t < TSW_COUNT; t++)
		if (pGun->m_items[t].owned)
		{
			// Kung-fu (TSW_MELEE) is the always-free unarmed weapon BuyArmament gives every player —
			// it does NOT consume inventory slots (the buy menu's "slots left" excludes it too). Counting
			// its slotCost(10) wrongly ate 10 slots, pushing a full loadout over 81 and silently dropping
			// the last bought weapon on spawn (e.g. sawed-off). Skip it. [[map-change-state-reset]]
			if (t == TSW_MELEE)
				continue;
			const s_weapon_info *wi = get_weapon_info((tsweapon_t)t);
			// STACKABLES (knife/sealknife/m61) charge slotCost PER UNIT — the original's
			// PickUpWeapon multiplies the deduction by the stack's clip (@0x8314c) and
			// MultiplyClips funds added units from free slots (@0x71308).
			int units = 1;
			if (wi->iStackable)
			{
				const int count = (t == pGun->m_weaponType) ? pGun->m_iClip : pGun->m_items[t].clip;
				units = (count > 1) ? count : 1;
			}
			used += wi->slotCost * units;
		}
	return used;
}

// @ GetSpeedBySlots__FP11CBasePlayer (@0x66bd4) — slot-based base movement speed. The player has 81
// inventory slots (CBasePlayer+0x548 init 0x51); every CARRIED weapon deducts its slotCost, and the
// remaining capacity sets the base maxspeed = 210 + (120/81)*remaining → empty ~330, fully loaded ~210.
// More/heavier guns = slower. Like the original, SetPlayerSpeed applies it on loadout events only
// (pickup, drop, armor, flag, throws); the remaining count is TSPlayerFreeSlots.
float GetSpeedBySlots(CBasePlayer *pPlayer)
{
	// @0x66be8: the One, whatever he carries, and empty hands (free > 80) return a fixed 330; only
	// a laden loadout ramps down, to a whole number: free x (1/81) x 120 + 210, the fistp chopping
	// (@0x66c04-0x66c40), so SetPlayerSpeed's own rounding has nothing left to round.
	if (pPlayer->m_bTheOne)
		return 330.0f;
	const int remaining = TSPlayerFreeSlots(pPlayer);
	if (remaining > 80)
		return 330.0f;
	return (float)(int)((double)(1.0f / 81.0f) * remaining * 120.0f + 210);
}

// Remaining inventory-slot capacity (CBasePlayer+0x548, init 81): lighter loadout =
// more free slots. The original keeps it as a counter that pickups, drops, the flag and kevlar move;
// reTS derives it from the arsenal, the flag and kevlar. Read by the pickup gate, the throws, the
// dives (CTSStunt::CanDive*), GetSpeedBySlots/GetAccelBySlots and the client's slot display.
int TSPlayerFreeSlots(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	int remaining = pGun ? (81 - TSSlotsUsed(pGun)) : 81;
	if (pPlayer->m_flagStatus.active)
		remaining -= 20; // carrying the CTF flag costs 20 slot-weight (CBasePlayer::TakeFlag -0x14)
	if (pPlayer->m_bHasKevlar)
		remaining -= 10; // kevlar costs 10 (CBasePlayer::TakeArmor -10 / +10 @0x83da4)
	// A plugin's setting, as the counter would hold it. Not clamped: with more slots set, the
	// arsenal can weigh over 81, and the counter only ever subtracts what each pickup weighs.
	return remaining + pPlayer->m_iFreeSlotsAdjust;
}

// @0x705e0 Reset — wipe the WHOLE TS arsenal + active state back to bare kung-fu. The original is a
// bulk zero of items[]/m_ammoReserve + the active pointers + the combo/timer/change-detect block.
// Callers: Spawn (clean init) and CBasePlayer::StartObserver (round-restart / death→spectate wipe).
void CTSGun::Reset()
{
	for (int t = 0; t < TSW_COUNT; t++)
		m_items[t] = TSItem();
	for (int c = 0; c < TS_NUM_CALIBERS; c++)
		m_ammoReserve[c] = 0;

	m_weaponType = TSW_MELEE;
	m_lastType = TSW_MELEE;
	m_pActiveItem = nullptr;
	m_pActiveInfo = nullptr;
	m_iClip = 0;
	m_fireMode = TSFM_SEMI;
	m_bHoldPosition = false;

	// combo / melee state
	m_comboStep = 0;
	m_kickCombo = 0;
	m_lastMeleeAnim = 0;
	m_lastCCTime = 0.0f;

	// per-fire transient timers
	m_burstOrPhase = 0;      // item+0x8 (burst/pump-phase/cook)
	m_fInSpecialReload = 0;  // item pumpToggle (rack step / akimbo side)
	m_flNextAlternateAttack = 0;
	m_flLastShotTime = 0.0f;

	// change-detect caches. The WeaponInfo type and reserve go to 0 (@0x70626, @0x7063a), so a gun
	// in this state (kung fu, nothing in reserve) sends no WeaponInfo until one of them changes: a
	// player who joins and holds kung fu gets none, as on the original.
	m_iClientWeaponType = TSW_MELEE;
	m_iClientReserve = 0;
	m_bWStatusInit = false;

	// the active item and its weapon info stay cleared (@0x70612 gun+0xcc, @0x7061c gun+0xd0 = 0)
}

// @ Spawn__6CTSGun — classname weapon_tsgun, id 16, fall-init as a pickup.
void CTSGun::Spawn()
{
	// @0x7053c, as the original: Reset (every item unowned, no reserve, type 0 = kung fu), classname,
	// Precache, the generic world model, then the id and the stock clip and default ammo of 1. It
	// seeds nothing: what a player carries comes from BuyArmament, TakeWeapon and the pickups.
	Reset();
	pev->classname = MAKE_STRING("weapon_tsgun");
	Precache();

	// @0x7057e: the glock world model is the generic CTSGun pickup/drop model (not a per-type
	// pszWorldModel lookup). It is precached by Precache() (PrecacheAllTSWeapons, the whole roster);
	// w_weaponbox is never precached, so a null-world-model type SET_MODEL'd there was a Host_Error
	// on a strict engine.
	SET_MODEL(ENT(pev), "models/w_glock18.mdl");

	m_iId = TSGUN_WEAPON_ID;  // +0x88
	m_iClip = 1;              // +0xb4
	m_iDefaultAmmo = 1;       // +0xc4
	m_weaponType = TSW_MELEE; // +0xc8 (Reset has already set it)
	// +0x9e4 = 0 (written only here, read nowhere) and +0x9e8 (m_flLastShotTime) = 0.
	m_flLastShotTime = 0.0f;
	FallInit();
}

// (OwnedCount DELETED, fold-plan Y1: the original has no arsenal-count helper — the `lastinv`
// dispatch @0x9e3bc-0x9e454 gates only on "holding a real weapon"; see TSLastInv.)

// Park the live active clip/mode back into its item (HolsterWeapon side).
void CTSGun::SyncActiveToItem()
{
	if (m_weaponType < 0 || m_weaponType >= TSW_COUNT)
		return;
	TSItem &it = m_items[m_weaponType];
	it.clip = m_iClip;
	it.mode = m_fireMode;
	it.holdPosition = m_bHoldPosition;
	it.pumpToggle = m_fInSpecialReload; // the rack/barrel step SURVIVES a switch (holster clears
	                                    // reloading+burstOrPhase but NOT this byte, @0x85698)
}

// Load the active item's clip/mode into the live state (Deploy side).
void CTSGun::LoadActiveFromItem()
{
	if (m_weaponType < 0 || m_weaponType >= TSW_COUNT)
		return;
	const TSItem &it = m_items[m_weaponType];
	m_iClip = it.clip;
	m_fireMode = it.mode;
	// @0x81030 (Deploy) CLEARS m_pActiveItem->holdPosition = '\0' — the sideways/upright weapon-hold
	// orientation RESETS to upright on EVERY deploy/switch; it is NOT restored from the parked item. Ours
	// used to restore it (m_bHoldPosition = it.holdPosition), so a weapon parked sideways redeployed
	// sideways and the switch "blinked" the stance. LoadActiveFromItem is shared code, so clearing here
	// fixes the server AND the client's predicted stance on the deploy frame (no blink). cont.89.
	m_items[m_weaponType].holdPosition = false; // item+0x2c
	m_bHoldPosition = false;
	m_burstOrPhase = 0; // holster cleared the item's burst/pump-phase slot (@0x85698)
	m_fInSpecialReload = it.pumpToggle; // restore the parked rack/barrel step
}

// (UpdateActivePointers DELETED, fold-plan G5: the original has no helper — Deploy @0x71030
// sets m_pActiveItem/m_pActiveInfo inline and HolsterWeapon @0x75698 nulls them inline; the
// two assignments now live at each m_weaponType-change site, both libs.)

// @ TakeWeapon__6CTSGun (@0x80a04) — add a weapon to the arsenal (fresh world pickup / buy),
// then wield it. The faithful akimbo OWNERSHIP model (reference/WEAPON_SELECT.md):
//  - own the single + pick up another of the same single -> fill the AKIMBO item (the combine):
//    items[akimbo].leftClip = items[single].clip, rightClip = weapon_info[single].clip, the
//    single item is NOT cleared (both stay owned: scroll skips the single, slot1-5 still picks it).
//  - a fresh akimbo (fire-type 4) reads its single base; a gold-colts only-akimbo (fire-type 5)
//    splits its clip into two barrels; a normal single/weapon just loads its full clip.
// (CTSGun::SlotsUsed DELETED, fold-plan Y2: demoted to the file-static TSSlotsUsed above —
// the original has no such method.)

bool CTSGun::TakeWeapon(tsweapon_t type, const TSItem *pStatus, int iAttach)
{
	if (type < 0 || type >= TSW_COUNT)
		return false;
	const s_weapon_info *wi = get_weapon_info(type);
	if (!wi->pszViewModel)
		return false; // invalid weapon (e.g. the unused flag slot 29)

	// NB (@0x70a04, decompile-exact): the third arg is an ATTACHMENT MASK (BuyWeapon passes the
	// bought silencer/laser/flash/scope bits; the ground weapon its spawnflag attachments) —
	// and TakeWeapon grants NO reserve ammo AT ALL. The reserve comes ONLY from BuyAmmo (the
	// loadout, fill-to-cap in free mode), ammo packs, ground ammo, and the ground weapon's
	// empty-reserve top-up.

	// The active item's live clip lives in m_iClip; park it so the combine/merge reads the
	// current rounds when the affected slot is the one we're holding.
	SyncActiveToItem();

	int fill = type; // the item slot we will fill
	if (m_items[type].owned)
	{
		// The STACKABLE merge (knife/sealknife/m61): an owned stack gains exactly ONE unit per
		// take (@0x70b9b -> +1), gated on the carry cap — wi->iPellets (+0x24: knife 5,
		// sealknife 10, m61 2) (@0x70a8e). Slot cost charges per unit through TSSlotsUsed.
		if (wi->iStackable)
		{
			TSItem &it = m_items[type];
			if (it.clip >= wi->iPellets)
				return false; // the stack is at its carry cap
			it.clip += 1;
			if (type == m_weaponType)
				m_iClip = it.clip;
			return true;
		}
		// If it upgrades to an akimbo we don't own yet, fill the akimbo (the combine — the ONE
		// other case picking up an owned weapon is allowed). Otherwise it's a duplicate: refuse
		// (leave the world weapon on the floor).
		if (wi->iAkimboType >= 0 && wi->iAkimboType < TSW_COUNT &&
			get_weapon_info((tsweapon_t)wi->iAkimboType)->pszViewModel && !m_items[wi->iAkimboType].owned)
		{
			fill = wi->iAkimboType;
		}
		else
		{
			return false;
		}
	}
	else
	{
		// A fresh single REFUSES while its akimbo is already owned (@0x70a41 — no third copy).
		if (wi->iAkimboType >= 0 && wi->iAkimboType < TSW_COUNT && m_items[wi->iAkimboType].owned)
			return false;
		// No slot check here, as in the original: the gate is PickUpWeapon's (@0x831bd), on the
		// free slots, which count kevlar, the flag and a plugin's setting.
	}

	// --- fill items[fill] ---
	const s_weapon_info *fwi = get_weapon_info((tsweapon_t)fill);
	TSItem &it = m_items[fill];

	if (pStatus && fill == type)
	{
		// a world pickup carrying its full parked state (clip/mode/attach/barrels/hold)
		it = *pStatus;
		it.owned = true;
		it.attach &= fwi->attachSupported; // @0x70d43 — clamp to what the gun can mount
		return true;
	}

	it.owned = true;
	it.mode = TSDefaultFireMode(fwi->iFireModes);
	it.attach = iAttach | fwi->attachBuiltin; // bought bits + the integral ones (@0x70c41)
	it.active = 0;
	it.barrelMask = 0;

	if (fwi->iFireType == TSFT_AKIMBO && fwi->iSingleType >= 0 && fwi->iSingleType < TSW_COUNT)
	{
		// combinable akimbo: right barrel = a fresh single's full clip; left barrel = the single
		// you already hold (its current clip), or a full clip when bought directly.
		const int single = fwi->iSingleType;
		const int singleClip = get_weapon_info((tsweapon_t)single)->iClip;
		it.leftClip = m_items[single].owned ? m_items[single].clip : singleClip;
		it.rightClip = singleClip;
		it.clip = it.leftClip + it.rightClip;
		// the combine UNIONS the attach/active masks across BOTH slots (@0x70c52-0x70c89)
		const int attachUnion = iAttach | it.attach | m_items[single].attach | fwi->attachBuiltin;
		it.attach = attachUnion;
		m_items[single].attach = attachUnion;
		const int activeUnion = it.active | m_items[single].active;
		it.active = activeUnion;
		m_items[single].active = activeUnion;
	}
	else if (fwi->iFireType == TSFT_AKIMBO_ONLY)
	{
		// gold colts: two barrels, each half the full clip.
		it.leftClip = fwi->iClip / 2;
		it.rightClip = fwi->iClip / 2;
		it.clip = fwi->iClip;
	}
	else
	{
		it.clip = fwi->iClip; // a fresh gun comes with one full clip — nothing else
	}

	it.attach &= fwi->attachSupported; // @0x70d43 — clamp to the mountable set
	// attached laser/flash start ACTIVE (@0x70d55-0x70d76)
	if (it.attach & 2)
		it.active |= 2;
	if (it.attach & 4)
		it.active |= 4;

	// NO auto-wield here — PickUpWeapon selects per its bSelect arg (@0x83318), so e.g.
	// recovering a thrown knife does NOT switch you off your gun.
	return true;
}

// @ AddAmmo__6CTSGun (@0x808ac) — add `count` rounds to caliber's reserve, capped at the
// caliber max. Reserves are per-caliber (gun+0xd4) and shared across same-caliber weapons.
bool CTSGun::AddAmmo(int caliber, int count)
{
	if (caliber <= 0 || caliber >= TS_NUM_CALIBERS || count <= 0)
		return false;
	int max = TSAmmoMax(caliber);
	if (m_ammoReserve[caliber] >= max)
		return false;
	m_ammoReserve[caliber] += count;
	if (m_ammoReserve[caliber] > max)
		m_ammoReserve[caliber] = max;
	return true;
}

// @ MultiplyClips__6CTSGun (@0x71308, decompile-exact) — the ammo (infinite-clip) powerup.
// Gates: a wielded item that isn't the m61. Interrupts any reload (StopReload), then:
//  - STACKABLE (knife/sealknife — wi+0xdc): ADD units up to 2x the default carry (wi->iPellets),
//    FUNDED by the player's free inventory slots (slotCost per unit; speed recomputed);
//  - normal: clip = 2x the max; an AKIMBO splits it across the barrels and mirrors HALF into its
//    single base's parked clip; a SINGLE with an akimbo variant adds half into the akimbo's parked
//    clip (leftClip = the single's clip).
bool CTSGun::MultiplyClips()
{
	if (!m_pActiveItem || !m_pActiveInfo)
		return false;
	if (m_weaponType == TSW_M61)
		return false;
	StopReload();
	const s_weapon_info *wi = m_pActiveInfo;
	TSItem &it = *m_pActiveItem;
	if (wi->iStackable)
	{
		const int wantSlots = (wi->iPellets * 2 - m_iClip) * wi->slotCost;
		const int freeSlots = TSPlayerFreeSlots(m_pPlayer);
		const int take = (wantSlots < freeSlots) ? wantSlots : freeSlots;
		if (wi->slotCost > 0 && take > 0)
		{
			m_iClip += take / wi->slotCost; // the free-slot deduction is implicit: stackables
			it.clip = m_iClip;              // charge slotCost PER UNIT in TSSlotsUsed
			m_pPlayer->SetPlayerSpeed(GetSpeedBySlots(m_pPlayer));
		}
	}
	else
	{
		it.clip = wi->iClip * 2;
		if (wi->iAkimboType == -1)
		{
			if (wi->iFireType == TSFT_AKIMBO || wi->iFireType == TSFT_AKIMBO_ONLY)
			{
				it.leftClip = (int)(0.5f * it.clip + 0.5f);
				it.rightClip = (int)(0.5f * it.clip + 0.5f);
				if (wi->iSingleType != -1 && wi->iSingleType != m_weaponType)
					m_items[wi->iSingleType].clip = (int)(0.5f * it.clip + 0.5f);
			}
		}
		else
		{
			m_items[wi->iAkimboType].clip += (int)(0.5f * it.clip + 0.5f);
			m_items[wi->iAkimboType].leftClip = it.clip;
		}
		m_iClip = it.clip;
	}
	return true;
}

// Per-tsweapon_t draw sequence in the v_*.mdl (the anim block entry [2], baked from the model
// sequence names — the original looks it up at init into the weapon anim set, item+0x28 +4).
static const int s_drawSeq[TSW_COUNT] = {
	1, 2, 4, 2, 5, 2, 4, 2, 2, 4, 5, 2, 2, 2, 2, 1, 4, 1, 2, 2,
	5, 2, 2, 1, 1, 2, 7, 2, 3, 2, 1, 3, 1, 1, 1, 1, 1, 1};
// [16] uzi_akimbo draw = v_uzi_akimbo.mdl seq 4 "akimbo_draw" (was 2 = "left_idle", a one-uzi looping
// idle → the akimbo deployed showing ONE left-hand uzi for ~1s until the idle corrected). cont.89.

// @ HolsterWeapon__6CTSGun (@0x85698, decompile-exact: dossiers/cont81/holster.c) — holster the
// wielded weapon TO BARE HANDS: strip the view/weapon models, leave any zoom, drop the blade
// 3rd-person idle override, stamp the switch delay (m_flNextAttack +0.5) + idle +3.0, KILL any
// armed knife/katana whiff-lunge (the whiff-clear-on-switch), park the active item's transient
// state (grenade -> GrenadeIdle(1) put-away; else the burst/phase + reload bytes), reset to
// kung-fu (type 0, active pointers cleared), present the kung-fu viewmodel (draw anim + draw.sc
// FEV_NOTHOST iparam1=0), and zero the kung-fu combo counters. SelectWeapon calls this, then the
// caller Deploy()s the incoming weapon (so every switch carries the kungfu draw on the wire,
// exactly like the original).
// @ Holster__6TSGuni (@0x75674, 0x21 bytes) — the virtual Holster is a pure forwarder to
// HolsterWeapon(). Without the override we fell through to the STOCK CBasePlayerWeapon::Holster
// (viewmodel strip only — no state park, no kung-fu reset).
void CTSGun::Holster()
{
	HolsterWeapon();
}

void CTSGun::HolsterWeapon()
{
	SyncActiveToItem(); // park the live clip/fire-mode (the original mutates the item in place)
	m_pPlayer->pev->viewmodel = 0;
	m_pPlayer->pev->weaponmodel = 0;
#ifndef CLIENT_DLL
	Unzoom();
#endif
	m_pPlayer->m_szAnimOverride[0] = 0; // player+0x478 (+0xb58 twin byte) — drop the blade idle override
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5f; // player+0x12c — the switch delay
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;

	// an armed whiff-lunge dies with the switch (only the knife/katana flavors, +0x7a4 != 0).
	if (m_pPlayer->m_flWhiffLungeTime != 0.0f && m_pPlayer->m_iWhiffLungeType != 0)
	{
		m_pPlayer->m_flWhiffLungeTime = 0;
		m_pPlayer->m_flWhiffLungeForce = 0;
		m_pPlayer->m_iWhiffLungeType = 0;
	}

	if (m_pPlayer && !m_pPlayer->IsAlive())
		return;

	if (m_pActiveItem && m_pActiveInfo)
	{
		if (m_pActiveInfo->iFireType == TSFT_GRENADE)
		{
			GrenadeIdle(1); // a cooked grenade is THROWN by the switch (the put-away force path)
		}
		else
		{
			m_fInReload = false;  // item+0xc
			m_burstOrPhase = 0;   // item+0x8 (burst / pump-phase / cook — one slot)
		}
	}

	m_weaponType = TSW_MELEE; // bare hands
	// @0x75698 nulls the active pointers inline (cont81 holster.c; fold-plan G5)
	m_pActiveInfo = get_weapon_info((tsweapon_t)m_weaponType); // gun+0xd0
	m_pActiveItem = nullptr;                                   // gun+0xcc — kung-fu has no item
	LoadActiveFromItem();
	const s_weapon_info *kf = get_weapon_info(TSW_MELEE);
	m_pPlayer->pev->viewmodel = MAKE_STRING(kf->pszViewModel); // v_melee
	// kungfu anim block [2] (draw) — @0x75698 passes skiplocal 0 and body 0 EXPLICITLY (fold-plan Y4):
	// sent even to a client predicting its weapons (cl_lw 1), which the base's own skiplocal would skip
	m_ForceSendAnimations = true;
	SendWeaponAnim(s_drawSeq[TSW_MELEE], 0);
	m_ForceSendAnimations = false;
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usDraw, 0.0, g_vecZero, g_vecZero,
		0.0, 0.0, m_weaponType, 0, 0, 0);
	m_kickCombo = 0;
	m_comboStep = 0;
	m_lastMeleeAnim = 0;
}

// @ SelectWeapon__6CTSGun (@0x80fb8) + the ClientCommand commit (@0xae433): holster the
// current weapon (HolsterWeapon — resets to bare hands), set the new active type, then deploy.
// Switching therefore cancels a reload and re-plays the draw animation/delay. Type 0 (kung-fu)
// is always selectable; any other type must be owned (the .so gates on items[type].owned).
void CTSGun::SelectWeapon(int type)
{
	if (type < 0 || type >= TSW_COUNT || type == m_weaponType)
		return;
	if (type != TSW_MELEE && !m_items[type].owned)
		return;

	const int outgoing = m_weaponType;
	HolsterWeapon();           // @0x85698 — park state + reset to bare hands (kungfu draw on the wire)
	m_lastType = outgoing;     // remember the outgoing type (for `lastinv`)

	m_weaponType = type;
	// @0x71030 (Deploy) sets the active pointers inline (cont81 deploy.c; fold-plan G5)
	m_pActiveInfo = get_weapon_info((tsweapon_t)m_weaponType); // gun+0xd0 (inline, fold-plan G5)
	m_pActiveItem = (m_weaponType > TSW_MELEE && m_weaponType < TSW_COUNT) ? &m_items[m_weaponType] : nullptr; // gun+0xcc — NULL = kung-fu
	LoadActiveFromItem();

	// --- Deploy (@0x81030): re-deploy the new active weapon (draw anim + deploy delay). ---
	// The tsgun is the player's ONE weapon entity, so it is (or should be) the active item; if
	// nothing is active yet (e.g. the bare kung-fu spawn the engine's auto-deploy skips), claim
	// it so the viewmodel shows + ItemPostFrame runs (firing). Otherwise only redeploy if active.
	if (m_pPlayer && (m_pPlayer->m_pActiveItem == (CBasePlayerItem *)this || !m_pPlayer->m_pActiveItem))
	{
		m_pPlayer->m_pActiveItem = (CBasePlayerItem *)this;
		Deploy();
	}
}

// @ GetBetterWeapon__6CTSGun (@0x80f40) — after a drop, auto-select the highest-priority
// OWNED weapon (g_iWeaponPriority order), skipping `exclude` (the just-dropped type). Holster->deploy
// via SelectWeapon. When nothing qualifies the walk ends on the list's last entry, kung-fu, which is
// selected whether or not it is owned. A bought loadout leaves the kung-fu item unowned, so this is
// what puts the hands up when the last gun is dropped.
void CTSGun::GetBetterWeapon(int exclude)
{
	int t = g_iWeaponPriority[0];
	for (int i = 1; (!IsOwned(t) || t == exclude) && i < TSW_COUNT; i++)
		t = g_iWeaponPriority[i];
	SelectWeapon(t);
}

// @ Precache__6CTSGun — faithful precaches 11 event scripts. Models for the whole
// roster are precached at map load (CWorld::Precache -> PrecacheAllTSWeapons), so
// a runtime give of any weapon type finds them cached (no precache-in-spawn error).
void CTSGun::Precache()
{
	// roster models/sounds/events are precached at map load (CWorld::Precache);
	// this re-call just resolves the cached indices for a runtime give.
	PrecacheAllTSWeapons();
	// @0x70660: the eleven events in the original's order, which fixes their indices in the server's
	// precache list (fire 1, coldcock 2 ... grenade 11 on a map with no other event before them)
	m_usFire = PRECACHE_EVENT(1, "events/fire.sc"); // predicted fire (sound/flash/anim + shell)
	m_usColdCock = PRECACHE_EVENT(1, "events/coldcock.sc"); // the bash presentation (gun+0x966)
	m_usFireLinked = PRECACHE_EVENT(1, "events/firelinked.sc"); // akimbo both-barrels (gun+0x96e)
	m_usEmpty = PRECACHE_EVENT(1, "events/empty.sc");   // dry-fire click (gun+0x968)
	m_usPump = PRECACHE_EVENT(1, "events/pump.sc");     // the rack half (gun+0x96a)
	m_usCloseCombat = PRECACHE_EVENT(1, "events/closecombat.sc"); // kung-fu/block presentation (gun+0x96c)
	m_usReload = PRECACHE_EVENT(1, "events/reload.sc"); // reload presentation (gun+0x970)
	m_usMelee = PRECACHE_EVENT(1, "events/knife.sc"); // melee impact spark/decal on a hard surface (gun+0x972)
	m_usKatana = PRECACHE_EVENT(1, "events/katana.sc"); // katana swing presentation (gun+0x974)
	m_usDraw = PRECACHE_EVENT(1, "events/draw.sc"); // deploy: seed the client active-weapon param cache (gun+0x976)
	m_usGrenade = PRECACHE_EVENT(1, "events/grenade.sc"); // pin/throw presentation (gun+0x978)
	PRECACHE_SOUND("player/kungfuhit.wav");   // kung-fu flesh hit (also the coldcock bash)
	PRECACHE_SOUND("player/closecombat.wav"); // kung-fu swing whoosh (miss / surface)
	PRECACHE_SOUND("player/block.wav");       // kung-fu block "hooah" (reload)
	PRECACHE_SOUND("player/breathe2.wav");    // exhausted breathing (block spam, stamina empty)
}

// @ GetItemInfo__6CTSGun (disasm-verified @0x75dd0, fold-plan Y3): pszAmmo1 = NULL,
// iMaxAmmo1 = -1, pszAmmo2 = NULL, iMaxAmmo2 = -1, iMaxClip = -1 (WEAPON_NOCLIP), slot 1,
// pos 2, flags 0, id 0x10. NO ammo types: TS runs its own per-caliber reserve, and with
// pszAmmo1 == NULL the stock CanDeploy (weapons_shared.cpp) returns true unconditionally —
// which is why the original CTSGun does NOT override CanDeploy (VTABLES.md slot 0x124 =
// CBasePlayerWeapon::CanDeploy). This also puts the gmsgWeaponList wire on the original values.
bool CTSGun::GetItemInfo(ItemInfo *p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = -1; // WEAPON_NOCLIP — the TS clip rides WeaponInfo, not CurWeapon
	p->iSlot = 1;
	p->iPosition = 2;
	p->iFlags = 0;
	p->iId = m_iId = TSGUN_WEAPON_ID;
	p->iWeight = TSGUN_WEIGHT;
	return true;
}

// No GetWeaponData/SetWeaponData override: the original client reads the stock weapon_data slots as its
// stock fire state (m_chargeReady/m_fInAttack/m_fireState/m_flStartThrow/m_flReleaseThrow). The TS
// active state travels on the clientdata vuser block (dlls/client.cpp UpdateClientData), which the
// client reads in HUD_WeaponsPostThink.

// Per-tsweapon_t viewmodel DRAW (unholster) sequence index — recovered by parsing each viewmodel's
// sequence names (the "draw"/"drawb" sequence). The original looks this up at init into the weapon
// anim set (item+0x28 +4); we bake the result. Passed to DefaultDeploy so switching to a weapon plays
// its unholster animation. Index = tsweapon_t.
// (s_drawSeq moved above HolsterWeapon — the anim block entry [2] per weapon.)

bool CTSGun::Deploy()
{
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	if (!wi->pszViewModel)
		return false;
	// @0x71064: deploying a weapon lets the next UpdateClientData run at once (kung fu holsters
	// instead in the original, before this).
	if (m_weaponType != TSW_MELEE)
		m_flNextClientUpdate = 0;
	// NB: Deploy does not touch the ActItems cache (CBasePlayer::m_iClientActiveItems, player+0x720):
	// the client keeps its g_playerAttach array across a level change (cl_dll/ts_attachments.cpp), in
	// step with that cache. See [[map-change-state-reset]].
	// Faithful Deploy: view/player model via DefaultDeploy + the attachment BODY via
	// GetBodyByItems below (the weapon-param message ride-along is WeaponInfo, sent by
	// UpdateClientData's change-detect).
	// Kung-fu (TSW_MELEE) has NO 3rd-person weapon model — it's bare hands, so the player must
	// hold NOTHING (empty weaponmodel) and just play the kung-fu body animations. Falling back to
	// the VIEW model here put v_melee on the player's back/hands (a phantom held weapon). For any
	// real weapon with no p_model we likewise show nothing rather than the viewmodel.
	// @0x71030: the p_ model = the SILENCED variant iff the silencer is attached (wi+0x1b2).
	const char *pmodel = wi->pszPlayerModel ? wi->pszPlayerModel : "";
	if ((m_items[m_weaponType].attach & 1) != 0 && wi->pszPlayerModelSil)
		pmodel = wi->pszPlayerModelSil;
	const int drawSeq = (m_weaponType >= 0 && m_weaponType < TSW_COUNT) ? s_drawSeq[m_weaponType] : 0;
	// Kung-fu (TSW_MELEE) uses an EMPTY anim-ext (grip) — the original keys the bare-hands
	// combat_idle stance + the close-combat attack set off m_szAnimExtention being empty.
	// Real weapons carry onehanded/twohanded/akimbo.
	const char *animExt = (m_weaponType == TSW_MELEE) ? "" : (wi->pszAnimExt ? wi->pszAnimExt : "onehanded");
	// @0x710c4: the draw anim CARRIES the attachment body — DefaultDeploy's 5th arg reaches the
	// viewmodel via the weaponanim message (pev->body alone never does; the missing-silencer/
	// scope-submodel bug).
	bool ok = DefaultDeploy((char *)wi->pszViewModel, (char *)pmodel, drawSeq, (char *)animExt,
		GetBodyByItems(m_weaponType, m_items[m_weaponType].attach));
	// @0x710c4: the deployed viewmodel BODY comes from the EQUIPPED attachment mask through
	// GetBodyByItems (silencer/laser/flash/scope submodels; the client draw.sc handler runs
	// the same math via FUN_10007110).
	pev->body = GetBodyByItems(m_weaponType, m_items[m_weaponType].attach);
	// player+0x478 3rd-person body-anim OVERRIDE (RE DefaultDeploy@0x90178, weapon_tsgun switch on m_weaponType):
	// the dedicated melee idles. CTSAnimation::Animate plays this directly when non-empty (else idle_/aim_<grip>).
	const char *ov = (m_weaponType == TSW_KNIFE || m_weaponType == TSW_SEALKNIFE) ? "knife_idle"
		: (m_weaponType == TSW_KATANA) ? "katana_idle"
		: (m_weaponType == TSW_FLAG) ? "briefcase_idle" : "";
	strncpy(m_pPlayer->m_szAnimOverride, ov, sizeof(m_pPlayer->m_szAnimOverride) - 1);
	m_pPlayer->m_szAnimOverride[sizeof(m_pPlayer->m_szAnimOverride) - 1] = 0;
	if (ok)
		m_pPlayer->m_TSAnim.Animate(m_pPlayer, TSA_DRAW); // 3rd-person draw_<grip>

	// events/draw.sc → EV_TSDraw: seed the CLIENT active-weapon param cache for the predicted
	// fire/reload handlers (iparam1 = type, iparam2 = active barrel mask: akimbo 2/4/6, else 1).
	// Additive — the deploy view/anim above stays server-driven; this only primes client state.
	if (ok && m_usDraw)
	{
		// iparam2 = the EQUIPPED attach mask (silencer/laser/flash/scope) — the ORIGINAL Deploy
		// (@0x71030) passes attachEquipped here, and the client draw.sc handler (FUN_100425b0) feeds
		// it to GetBodyByItems to assert the viewmodel BODY on the deploy frame (cont.89 §3, fixes the
		// single->akimbo body flicker). Was the akimbo barrelMask, which is dead — the barrel side
		// rides item+0x24 / vuser4.x now.
		PLAYBACK_EVENT_FULL(0, m_pPlayer->edict(), m_usDraw, 0.0, g_vecZero, g_vecZero,
			0.0, 0.0, m_weaponType, m_items[m_weaponType].attach, 0, 0);
	}
	return ok;
}

// @ SecondaryAttack__6CTSGun @0x717a8 — the right-click dispatcher (decompile-exact). Bare hands ->
// kung-fu secondary; grenade/knife write the item's alt-mode byte (item+0x24 = 1) then share the
// primary's fire function; the katana has its own KatanaThrow; the flag (fire-type 11) does nothing.
// The akimbo fire-modes route right-click to Fire() — the OTHER hand shoots (mode 4 alternating needs
// a fresh press unless the double-fire powerup); everything else cycles the scope when one is equipped
// (attach & 8), otherwise toggles the hold-position stance, then re-arms SECONDARY +0.3 and idle +1.0
// (the original leaves m_flNextPrimaryAttack alone here — our old inline block had the two swapped).
void CTSGun::SecondaryAttack()
{
	if (m_weaponType == TSW_MELEE) // original: m_pActiveItem == NULL (bare-hands kung-fu)
	{
		CCombatAttack();
		return;
	}
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	TSItem &it = m_items[m_weaponType];

	if (wi->iFireType == TSFT_GRENADE)
	{
		it.altMode = 1; // item+0x24 = 1 -> the UNDERHAND roll (GrenadeIdle's release branch)
		GrenadeFire();
		return;
	}
	if (wi->iFireType == TSFT_KNIFE)
	{
		it.altMode = 1; // item+0x24 = 1 -> KnifeFire's THROW branch (@0x717d3)
		KnifeFire();
		return;
	}
	if (wi->iFireType == TSFT_KATANA)
	{
		KatanaThrow();
		return;
	}
	if (wi->iFireType == TSFT_FLAG)
		return; // @0x717e6: fire-type 11 has no secondary

	if (m_fireMode == TSFM_AKIMBO_ALT) // "Free Semi": right click = the RIGHT barrel, per press
	{
		if (!(m_pPlayer->m_afButtonPressed & IN_ATTACK2) && !DoubleFireRate(m_pPlayer))
			return;
	}
	else if (m_fireMode != TSFM_AKIMBO_BOTH) // plain modes: an akimbo right-click is the HOLD
	                                          // toggle below (guns oriented facing each other),
	                                          // exactly like the singles
	{
		if (it.attach & 8)
		{
			// scope cycle: 0 (none) -> zoomFov1 -> zoomFov2 -> back to 0. The view FOV + viewmodel-hide
			// are player pev fields the server networks to the client (server-authoritative).
#ifndef CLIENT_DLL
			const float fov = m_pPlayer->pev->fov;
			if (fov == 0.0f)
			{
				if (wi->zoomFov1 != 0)
					Zoom(wi->zoomFov1);
			}
			else if (wi->zoomFov2 == 0 || (float)wi->zoomFov2 == fov)
				Unzoom();
			else
				Zoom(wi->zoomFov2);
#endif
		}
		else if (wi->iHoldChange >= 0)
		{
			// hold-position stance toggle (wi+0x28c != -1): play the change/rechange transition and
			// re-point the active anim set (the original swaps item+0x28 between info+0x252/+0x28c).
			m_bHoldPosition = !m_bHoldPosition;
			it.holdPosition = m_bHoldPosition;
			SendWeaponAnim(m_bHoldPosition ? wi->iHoldChange : wi->iHoldRechange, pev->body);
		}
		m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.3f; // dc_0_3 @LAB_000819f3
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 1.0f;      // dc_1
		return;
	}
	// the FREE modes (4 "Free Semi" / 5 "Free Full"): a right-click (IN_ATTACK2) fires the LEFT
	// barrel — but the barrel is picked from the HELD button INSIDE AkimboFire (item+0x24), NOT
	// preset here (the original @0x717a8 just Fire()s the akimbo, leaving item+0x24 to AkimboFire).
	Fire();
}

// @ PrimaryAttack__6CTSGun -> Fire -> {Standard,Shotgun,Knife,Katana,Grenade,Akimbo}Fire.
// Faithful dispatch by the weapon's fire-type, using the recovered per-weapon
// rate/spread (StandardFire reads s_weapon_info+0x18 rate / +0x28 spread / +0x348
// fire-type). The original tests for kung fu by having no active item (gun+0xcc) and reads the
// fire-type from the active row (gun+0xd0); the held m_weaponType picks the same row here.
// Melee/grenade/akimbo split out to MeleeAttack/GrenadeFire/AkimboFire.
void CTSGun::PrimaryAttack()
{
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);

	// Kung-fu (TSW_MELEE) = bare hands. The original has NO active weapon item here (gun+0xcc==0)
	// and routes PrimaryAttack -> CCombatAttack (@0x86078): a contextual punch/kick chosen from the
	// movement buttons + flags. Knife/katana are real weapon items (their own fire-types below).
	if (m_weaponType == TSW_MELEE)
	{
		CCombatAttack();
		return;
	}

	// Fire-types dispatched directly (outside the fire-mode trigger system, like the
	// original PrimaryAttack @0x81a20): melee hull-traces and the grenade throw.
	if (wi->iFireType == TSFT_KNIFE)
	{
		m_items[m_weaponType].altMode = 0; // @0x71a89: item+0x24 = 0 (slash) before KnifeFire
		KnifeFire();
		return;
	}
	if (wi->iFireType == TSFT_KATANA)
	{
		KatanaFire(); // @0x71aaf: fire-type 10 has its OWN fire fn (never through KnifeFire)
		return;
	}
	if (wi->iFireType == TSFT_GRENADE)
	{
		m_items[m_weaponType].altMode = 0; // @0x71a61: item+0x24 = 0 (overhand) before GrenadeFire
		GrenadeFire();
		return;
	}
	// @0x71ac2 fire-type 11 (the CTF BRIEFCASE — knife models, Write_flag): primary is a KNIFE
	// SLASH, gated on a FRESH attack press (orig player+0x814 & 1 = the press-edge bitfield),
	// altMode=0 (slash, never throw). cont.96: without this branch the flag fell through to the
	// standard-gun path — one phantom bullet then endless dry-click ("knife" pickup corruption).
	if (wi->iFireType == TSFT_FLAG)
	{
		if ((m_pPlayer->m_afButtonPressed & IN_ATTACK) == 0)
			return;
		m_items[m_weaponType].altMode = 0;
		KnifeFire();
		return;
	}

	// Bullet/akimbo/pellet weapons: the current FIRE-MODE gates the trigger (@0x71b0d..0x71b95, client.dll
	// 0x100089a0 the same); the fire *function* is still chosen by fire-type inside Fire():
	//  - semi (1) and free semi (4): a fresh press, or held with the double fire rate powerup;
	//  - auto (0) and free full (5): every call (ItemPostFrame calls with the trigger held);
	//  - burst (2): a fresh press with no burst queued arms it (3 rounds, 2 for the MAC-10) and fires
	//    nothing; WeaponIdle fires the queued rounds;
	//  - pump (3): while the trigger is held;
	//  - anything else: nothing.
	const bool held = (m_pPlayer->pev->button & IN_ATTACK) != 0;
	const bool pressed = (m_pPlayer->m_afButtonPressed & IN_ATTACK) != 0;
	bool fire = false;
	switch (m_fireMode)
	{
	case TSFM_SEMI:
	case TSFM_AKIMBO_ALT:
		fire = pressed || DoubleFireRate(m_pPlayer);
		break;
	case TSFM_AUTO:
	case TSFM_AKIMBO_BOTH:
		fire = true;
		break;
	case TSFM_BURST:
		if (pressed && m_burstOrPhase == 0)
			m_burstOrPhase = (m_weaponType != TSW_MAC10) + 2;
		return;
	case TSFM_PUMP:
		fire = held;
		break;
	default:
		return;
	}

	if (fire)
	{
		// The akimbo barrel side is owned ENTIRELY by AkimboFire (item+0x24 flip / held-button
		// select). PrimaryAttack does NOT touch it — the original @0x71a20 only sets item+0x24 for
		// grenade/knife. Resetting it here every pull is what killed the per-shot alternation.
		Fire();
	}
}

// @ Fire__6CTSGun (@0x71c78, decompile-exact) — fire ONE shot, dispatching by fire-type (the
// trigger gating happens in PrimaryAttack/WeaponIdle). Fully underwater = the dry click; an empty
// clip clicks too UNLESS the InfiniteClips powerup or the pump's non-firing RACK half (toggle set,
// pump mode, pump gun — it must run even empty, to rack on the last shell). 3rd-person body pose:
// the AKIMBO plays 8+(barrel side) and only if it actually fired; pump/standard play TSA_FIRE(0).
// (cont.83: the 8/9 pose was on the PUMP here and TSA_FIRE on the akimbo — the binary has it the
// other way around.)
void CTSGun::Fire()
{
	if (!m_pActiveItem)
		return;
	if (m_pPlayer->pev->waterlevel == 3)
	{
		EmptySound(); // @0x71caf — guns don't fire underwater
		return;
	}
	const s_weapon_info *wi = m_pActiveInfo;
	const bool pumpRackStep = (m_fInSpecialReload != 0 && m_fireMode == TSFM_PUMP &&
		wi->iFireType == TSFT_PUMP);
	if (!InfiniteClips(m_pPlayer) && m_iClip < 1 && !pumpRackStep)
	{
		EmptySound(); // @0x71cfc — the 0.1s re-arm happens inside
		return;
	}

	int shootanim = -1;
	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	if (wi->iFireType == TSFT_AKIMBO || wi->iFireType == TSFT_AKIMBO_ONLY)
	{
		if (AkimboFire(wi))
			shootanim = 8 + (m_items[m_weaponType].altMode != 0 ? 1 : 0); // fired barrel (0 right/1 left)
	}
	else
	{
		if (wi->iFireType == TSFT_PUMP)
			ShotgunFire(wi);
		else
			StandardFire(wi);
		shootanim = TSA_FIRE;
	}
	if (shootanim >= 0)
		m_pPlayer->m_TSAnim.Animate(m_pPlayer, shootanim); // direct Animate (TS's SetAnimation is empty)
}

// @ StandardFire/ShotgunFire — a single bullet (or a pump-shotgun pellet spread), with the
// weapon's real damage/spread/rate. Burst rounds fire fast then slow on the last (the .so
// uses 0.03 / 0.2; StandardFire @0x824a4).
void CTSGun::StandardFire(const s_weapon_info *wi)
{
	const int cPellets = wi->iPellets > 0 ? wi->iPellets : 1; // 8 for shotguns (s_weapon_info+0x24)
	float flSpread = wi->flSpread > 0 ? wi->flSpread : 0.01f;
	// TS dynamic accuracy (RE StandardFire @0x824a4): for weapons flagged bUsesSpread (s_weapon_info+0x2c —
	// the firearms; shotguns/melee fire at the static base cone), grow the per-player spread accumulator and
	// fold it into this shot's cone, so sustained fire walks the spread out (it recovers in PreThink).
	// silenced = attach bit0 present and NOT integral (an MP5SD's built-in silencer doesn't count)
	const bool sfSilenced = (m_items[m_weaponType].attach & 1) != 0 && (wi->attachBuiltin & 1) == 0;
	// Two weapons fire worse on the move (StandardFire 0x8256c-0x82674); every other gun keeps its
	// standing growth however fast the player goes.
	// - the Barrett: over 35 u/s the accumulator jumps to 0.3, so even the first shot misses
	if (m_weaponType == TSW_BARRETT && m_pPlayer->pev->velocity.Length() > 35.0f)
		m_pPlayer->m_flSpreadAccum = 0.3f;
	// - the M60: over 175 u/s the growth is x5, applied whatever bUsesSpread says
	if (m_weaponType == TSW_M60 && m_pPlayer->pev->velocity.Length() > 175.0f)
		ScaleSpread(flSpread, 5.0f * wi->flRecoil, sfSilenced);
	else if (wi->bUsesSpread)
		ScaleSpread(flSpread, wi->flRecoil, sfSilenced);

	m_iClip--;
	// @0x824a4 StandardFire: a single that HAS an akimbo variant mirrors its shot into the parked akimbo —
	// decrement the akimbo's TOTAL clip and set its leftClip = the single's (decremented) clip, clamped >=0.
	// This is the reverse of AkimboFire's single-mirror (2305); without it, draining the single left the
	// parked akimbo stuck at N/N. The original gates the pair under !InfiniteClips; we mirror our own
	// (unconditional) single decrement so the two stay in lockstep. cont.89.
	if (wi->iAkimboType >= 0 && wi->iAkimboType < TSW_COUNT)
	{
		TSItem& ak = m_items[wi->iAkimboType];
		ak.clip--;                 // item[iAkimboType]+0x04 (field_0x118)
		ak.leftClip = m_iClip;     // item[iAkimboType]+0x1c (field_0x130) = the single's live clip
		if (ak.clip < 0) ak.clip = 0;
	}
	m_pPlayer->pev->effects |= EF_MUZZLEFLASH; // 3rd-person muzzleflash on the player model
	// (the 3rd-person shoot_ body pose is set by CTSGun::Fire's direct Animate call — RE Fire@0x71c78 — not
	// here; TS's SetAnimation is empty.)

	// No SendWeaponAnim: the shoot animation is the fire.sc handler's (EV_TSFire), as in both originals
	// (neither StandardFire @0x724a4 nor client.dll's 0x100091e0 calls it). A predicted SendWeaponAnim
	// here left the client's current anim at the shoot while TS 3.0's server kept pev->weaponanim at
	// the draw, and the prediction's anim check then played the draw again after every shot.

	// events/fire.sc → EV_TSFire: predicted fire sound + brass shell + viewmodel muzzleflash. Played
	// with FEV_NOTHOST so the predicting host plays its OWN predicted copy (and isn't sent the server
	// copy too — otherwise the muzzle/shell would double); other clients get the server copy.
	// Arg packing is byte-faithful to the original StandardFire@0x824a4 pfnPlaybackEvent:
	//   iparam1 = weaponType, iparam2 = ATTACH MASK (active item+0x10; bit0 = silencer),
	//   bparam1 = (clip == 0).  The silencer bit drives the client's fire-sil sound + muzzle
	//   suppression (EV_TSFire). Barrel side is always 0 here (non-akimbo); we carry it in bparam2
	//   for the merged akimbo path (see AkimboFire) — our extension, not read by the original client.
	// The view kick. Pump shotguns kick less: ShotgunFire@0x72108 scales it to 0.35x when the weapon has an
	// auto/semi mode bit (iFireModes & 3) and is not in semi mode, so the dual-mode benelli/spas (mask 0xa)
	// get 0.35x in pump mode (full in semi), while the pump-only mossberg (mask 0x8) keeps the full kick.
	// The server's VKickback below punches bots only; a player's kick is the client's (CLIENT_DLL below).
	float flViewKick = wi->flViewKick;
	if (wi->iFireType == TSFT_PUMP && (wi->iFireModes & 3) != 0 && m_fireMode != TSFM_SEMI)
		flViewKick *= 0.35f;
	// fparam1 = fparam2 = the spread (StandardFire @0x728cd and ShotgunFire @0x72405 push the same value
	// twice; the client's fire core spreads the pellets across by fparam1 and up by fparam2);
	// bparam1 = (clip==0) for the standard classes (the akimbo classes read it as the barrel);
	// bparam2 = the FireBullets SEED (the original client's pellet pattern reads it).
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usFire, 0.0, g_vecZero, g_vecZero,
		flSpread, flSpread, m_weaponType, m_items[m_weaponType].attach, (m_iClip == 0) ? 1 : 0,
		m_pPlayer->random_seed);
#ifdef CLIENT_DLL
	// The aim kick, from the prediction as client.dll does: its StandardFire 0x100091e0 passes the
	// weapon's kick with the silencer flag, its ShotgunFire 0x10008e50 the pump-scaled kick unsilenced
	// (both only for a weapon whose byte +0x38 is set, which every firearm's Write_ sets).
	if (wi->iFireType == TSFT_PUMP)
		TS_AddViewRecoil(flViewKick, wi->iFireType, false, DoubleFireRate(m_pPlayer), gpGlobals->time);
	else
		TS_AddViewRecoil(wi->flViewKick, wi->iFireType, sfSilenced, DoubleFireRate(m_pPlayer), gpGlobals->time);
#endif

	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecAiming = gpGlobals->v_forward;
#ifndef CLIENT_DLL
	// @0x72759 (the StandardFire call site): (iPellets, src, aim, (s,s,s), flDamage,
	// wi->iKnifeType (0 for firearms), iCaliber, random_seed). damagemult applies INSIDE
	// (cheat-gated) — the old outer TSDamageMult was double-applying it.
	// @0x8256c: a (non-integral) silencer costs damage — x0.775
	m_pPlayer->ShootBulletsPlayer((unsigned char)cPellets, vecSrc, vecAiming,
		Vector(flSpread, flSpread, flSpread),
		sfSilenced ? wi->flDamage * 0.775f : wi->flDamage, wi->iKnifeType,
		wi->iCaliber, m_pPlayer->random_seed);
#else
	// Client prediction: spread/recoil only (damage + decals are server-authoritative).
	m_pPlayer->FireBulletsPlayer(cPellets, vecSrc, vecAiming,
		Vector(flSpread, flSpread, flSpread), 8192, BULLET_PLAYER_9MM,
		0, (int)(wi->flDamage * TSDamageMult()), m_pPlayer->pev, m_pPlayer->random_seed);
#endif

	// Server-side view-punch (RE StandardFire: VKickback when weaponinfo+0x38 != 0 — all firearms). Uses the
	// same flViewKick computed above (pump 0.35x folded in). NB: VKickback's punchangle only affects BOTS; real
	// players get the client aim-recoil from the prediction above.
	VKickback(flViewKick, wi->iFireType);

	float flRate;
	if (m_fireMode == TSFM_BURST)
	{
		// @0x72902: 0.03 between a burst's rounds, 0.2 after its last (the round WeaponIdle fires with
		// the count at 1; WeaponIdle takes the count down after the shot, not this).
		flRate = (m_burstOrPhase == 1) ? 0.2f : 0.03f;
	}
	else if (m_fireMode == TSFM_SEMI && (wi->iFireModes & (1 << TSFM_AUTO)))
	{
		// @0x824a4 (cont.89): a DUAL-MODE weapon (one that also has an AUTO mode) fired in SEMI
		// re-arms at a FIXED 0.1s, NOT flFireRate. The original client predicts this exact rate
		// (client fork FUN_100091e0); without it our cadence diverged by one shot, and the client's
		// per-frame re-seed of the predicted clip from vuser4[1] reverted it → the 20↔19 flicker.
		flRate = 0.1f;
	}
	else
	{
		flRate = wi->flFireRate > 0 ? wi->flFireRate : 0.15f;
		// DoubleFireRate's 1.5x speedup is applied ONCE, by the PostThink weapon-timer decrement
		// (player.cpp: slowFrametime *= 1.5, RE @0x81610 player+0x724 & 0x10). Do NOT also divide
		// flRate here — that compounded to 2.25x (the "fires far too fast" bug). The powerup's
		// fire-WHILE-held effect is handled separately at the full-auto gate above.
	}
	m_flNextPrimaryAttack = (UTIL_WeaponTimeBase() + flRate);
	// @0x72972: the idle 2 s on, 0.5 s for an emptied Contender without infinite clips.
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() +
		((m_weaponType == TSW_CONTENDER && m_iClip < 1 && !InfiniteClips(m_pPlayer)) ? 0.5f : 2.0f);
}

// @ ShotgunFire__6CTSGun (@0x72108) — the pump shotgun's primary attack. Unlike StandardFire (one shot per
// trigger event), this ALTERNATES per trigger-frame: a FIRE-step (a real pellet shot) then a RACK-step (the
// pump animation + sound, no bullet), each gated by the fire rate. So holding the trigger gives
// shot→rack→shot→rack at half the raw rate — and you visibly pump between shots. The original keys the
// alternation off m_pActiveItem+0x24; we keep it in m_fInSpecialReload (predicted — see ts_gun.h) so the
// client and server stay in step (a non-predicted toggle desyncs → the host fires every frame = infinite-fire).
//
// The fire-step delegates to StandardFire (identical pellet/spread/muzzle/event/recoil/clip/rate logic). The
// rack-step plays the viewmodel "pump" sequence (parsed from the models: v_m3/v_spas12 "pump"=8,
// v_mossberg "pump"=2) + the per-weapon rack sound, and re-arms the fire timer at the same rate.
void CTSGun::ShotgunFire(const s_weapon_info *wi)
{
	// A shell-by-shell reload in progress: a fire press interrupts it — close the pump (phase 2), no shot.
	// (orig: `if (item+8 != 0) { item+8 = 2; return; }`.) ItemPostFrame's ShotgunReload then plays the close.
	if (m_burstOrPhase != 0)
	{
		m_burstOrPhase = 2;
		return;
	}

	// RACK half of the cycle: pump mode (3) with the toggle set. No bullet, no shell consumed.
	// The WHOLE presentation rides events/pump.sc (EV_TSPump: the block[13/14] anim, the ejected
	// shell, and the rack sound — wi+0x306, played at ent -1 so it never channel-cuts the still-
	// playing gunshot). RE: all 3
	// pump shotguns DEFAULT to PUMP mode (TakeWeapon: AUTO>PUMP>BURST>SEMI) — held = auto
	// fire/rack alternation, a tap = one step.
	if (m_fireMode == TSFM_PUMP && m_fInSpecialReload != 0)
	{
		float flRate = wi->flFireRate > 0 ? wi->flFireRate : 0.15f;
		// DoubleFireRate 1.5x comes from the PostThink timer decrement only (see StandardFire note) —
		// no flRate division here (compounded to 2.25x).
		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + flRate;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;

		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usPump, 0.0, g_vecZero, g_vecZero,
			0.0, 0.0, m_weaponType, m_pActiveItem ? m_pActiveItem->attach : 0, 0, 0);
		m_fInSpecialReload = 0; // next trigger-frame is a FIRE-step
		return;
	}

	// FIRE half of the cycle: a normal shotgun shot (pellets/muzzle/event/recoil/clip). ShotgunFire's
	// own timers (@0x72192, client.dll 0x10008e50): 0.15 s in semi, else the weapon's rate; the idle
	// 2 s on.
	StandardFire(wi);
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() +
		((m_fireMode == TSFM_SEMI) ? 0.15f : (wi->flFireRate > 0 ? wi->flFireRate : 0.15f));
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
	// Arm the rack for the next trigger-frame — but only in PUMP mode (in SEMI mode a benelli just fires per
	// pull, no auto-rack). Matches the original toggling item+0x24 each ShotgunFire.
	m_fInSpecialReload = (m_fireMode == TSFM_PUMP) ? 1 : 0;
}

// @ ScaleSpread__6CTSGun (@0x802a8) — TS dynamic accuracy. Folds the per-player spread accumulator
// (CBasePlayer::m_flSpreadAccum, the .so's player+0x778) into THIS shot's cone, then grows the accumulator
// for the next shot — so sustained fire walks the spread out. The accumulator recovers over time in
// CBasePlayer::PreThink. `increment` = the weapon's flRecoil (s_weapon_info+0x34) — x5 for the M60
// moving faster than 175 u/s, x2 for the akimbo LINKED shot (the callers fold those in).
// ⚠ ARG IDENTITY (round-6 RE): the 4th arg is the SILENCED flag (attach bit0 present and NOT
// integral — both original call sites @0x82674/@0x82ad4 compute exactly that), NOT ducking as we
// had mislabeled — a SILENCER tightens the base cone x0.875 and the growth x0.775.
void CTSGun::ScaleSpread(float &spread, float increment, bool silenced)
{
	if (silenced)
		spread *= 0.875f;                 // silenced -> tighter base cone
	if (increment <= 0.0f)
		return;                           // weapon has no dynamic recoil
	float factor = 2.0f;
	if (DoubleFireRate(m_pPlayer)) factor *= 0.5f;    // bullet-time fires softer
	if (silenced)                  factor *= 0.775f;
	const float dt = gpGlobals->time - m_flLastShotTime; // time since this gun last fired
	if (dt >= 0.001f)
	{
		spread += m_pPlayer->m_flSpreadAccum;            // effective spread = base + accumulator
		float recovery = m_pPlayer->m_flSlowMotionGo * dt; // GiveSlowMul(player) * dt
		if (recovery >= 1.0f) recovery = 0.5f;           // RE: cap the per-shot recovery at 0.5
		float accum = (1.0f - recovery) * increment * factor + m_pPlayer->m_flSpreadAccum;
		if (accum > 0.1f) accum = 0.1f;                  // RE: capped at 0.1
		m_pPlayer->m_flSpreadAccum = accum;
	}
	m_flLastShotTime = gpGlobals->time;
}

// @ VKickback__6CTSGun (@0x70414) — the firearm recoil view-punch. Kicks pev->punchangle, which the engine
// renders onto the view and decays back over time — so the aim jolts up like a mouse nudge and the player
// pulls down to compensate (spraying walks the aim). Vertical = 0.85*magnitude (magnitude = wi->flViewKick,
// s_weapon_info+0x30); the horizontal sign/size is per fire-type. Ducking and bullet-time (DoubleFireRate)
// damp it. The server applies it to bots only (FL_FAKECLIENT, @0x70414); a real player's kick is the
// client's (client.dll's twin, FUN_10008230).
void CTSGun::VKickback(float magnitude, int firetype)
{
	float vert = 0.85f * magnitude;
	float horiz;
	switch (firetype)
	{
	case 8:         horiz = vert *  0.35f; break; // pistol (semi)
	case 4: case 5: horiz = vert *  0.15f; break; // akimbo / akimbo-only
	case 1:         horiz = vert * -0.25f; break; // pump shotgun
	case 6: case 7: horiz = vert * -0.30f; break; // autoshotgun / sniper
	case 9:         horiz = vert * -0.35f; break; // auto SMG
	default:        horiz = 0.0f;          break;
	}
	if (m_pPlayer->pev->flags & FL_DUCKING) { vert *= 0.775f;  horiz *= 0.775f; }  // ducked → tighter
	if (DoubleFireRate(m_pPlayer))          { vert *= 0.6667f; horiz *= 0.6667f; } // bullet-time → softer
	// RE @0x70414: the punchangle is applied ONLY to FL_FAKECLIENT (bots). Real players' recoil is the
	// angle-based aim kick (client-side), NOT punchangle — so this is effectively a no-op for human players.
	if (m_pPlayer->pev->flags & FL_FAKECLIENT)
	{
		m_pPlayer->pev->punchangle.x -= vert;  // kick the (bot's) view UP
		m_pPlayer->pev->punchangle.y += horiz; // and sideways per the recoil pattern
	}
}

// @ KnifeFire__6CTSGun @0x74044 — the knife/sealknife SLASH and THROW in one method, split on the
// item's alt-mode byte (item+0x24: PrimaryAttack writes 0 = slash, SecondaryAttack 1 = throw).
// No button/timer gates here (ItemPostFrame + the dispatcher gate); both attack timers re-arm at
// flFireRate (x1.5 stunting) so holding either button auto-repeats. SLASH: 3rd-person anim 29-31
// random, dmg x(2/3 stunting) x GetMeleeMultiplier, CheckHull hull-3 at 35u, VictimKnifeHold
// deflect, DMG_SLASH|DMG_NEVERGIB, player hit bloodies the blade + victim-edict knife_hitbody.wav,
// miss arms the whiff-lunge; knife.sc plays EVERY slash (FEV_NOTHOST, iparam1=weaponType,
// iparam2=bloody — ALL swing presentation is client-side, no server whoosh, no SendWeaponAnim).
// THROW: wall-clipped eye trace spawn, CreateKnife @1900, roll 90 / spin -1300, anim 32, server
// knife_miss.wav whoosh, consume gated !InfiniteClips; 0 knives -> DropWeapon(type, true), else a
// model-derived redraw + draw.sc + idle 2.0.
void CTSGun::KnifeFire()
{
	if (m_weaponType == TSW_MELEE) // original gate: m_pActiveItem == NULL
		return;
	entvars_t *ppev = m_pPlayer->pev;
	TSItem &it = m_items[m_weaponType];
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);

	const bool stunting = (ppev->iuser4 & 0x70c30) != 0;
	float rate = wi->flFireRate;
	if (stunting)
		rate *= 1.5f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + rate;
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + rate;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
	m_pPlayer->m_flLastMeleeRate = rate; // player+0x4f8
	UTIL_MakeVectors(ppev->v_angle);

	bool consumed = false;

	if (it.altMode == 0)
	{
		// ---- SLASH ----
		const int r = RANDOM_LONG(0, 2);
		if (!stunting && (ppev->flags & FL_ONGROUND))
			m_pPlayer->m_TSAnim.Animate(m_pPlayer, TSA_CC_KNIFE1 + r); // body anims 29-31

		float dmg = wi->flDamage;
		if (stunting)
			dmg *= 2.0f / 3.0f;

#ifndef CLIENT_DLL // hit resolution is server-authoritative (house pattern)
		dmg *= m_pPlayer->GetMeleeMultiplier();
		Vector vecSrc = m_pPlayer->GetGunPosition();
		Vector vecEnd = vecSrc + gpGlobals->v_forward * 35.0f; // slash reach 35, hull-3 sweep
		TraceResult tr;
		m_pPlayer->CheckHull(vecSrc, vecEnd, tr, 3);

		bool whiff = true;
		if (tr.flFraction < 1.0f)
		{
			CBaseEntity *pEnt = tr.pHit ? CBaseEntity::Instance(tr.pHit) : nullptr;
			if (pEnt && pEnt->IsAlive() && !(pEnt->pev->effects & EF_NODRAW))
			{
				// world/props take NO slash damage — only living, drawn victims resolve.
				whiff = false;
				if (!VictimKnifeHold(m_pPlayer, pEnt, dmg))
				{
					ClearMultiDamage();
					pEnt->TraceAttack(ppev, dmg, gpGlobals->v_forward, &tr, DMG_SLASH | DMG_NEVERGIB); // 0x1004
					ApplyMultiDamage(ppev, ppev);
					if (pEnt->IsPlayer())
					{
						it.bloody = 1; // item+0x8 — bloody knife (rides knife.sc as iparam2)
						pev->body = 1; // bloody w_/p_ submodel
						EMIT_SOUND_DYN(ENT(pEnt->pev), CHAN_BODY, "weapons/knife/knife_hitbody.wav",
							VOL_NORM, 0.8f, 0, 95 + RANDOM_LONG(0, 31)); // from the VICTIM's edict
					}
				}
			}
		}
		if (whiff)
		{
			// miss (or a non-living hit): arm the whiff-lunge — knife flavor.
			m_pPlayer->m_flWhiffLungeTime = 0.25f * wi->flFireRate;
			m_pPlayer->m_flWhiffLungeForce = dmg;
			m_pPlayer->m_iWhiffLungeType = 1;
		}
#endif
		// knife.sc EVERY slash — the whoosh/impact/viewmodel presentation is client-side.
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usMelee, 0.0, g_vecZero, g_vecZero,
			0.0, 0.0, m_weaponType, it.bloody, 0, 0);
		// the slash never touches the clip (consumed stays false).
	}
	else
	{
		// ---- THROW ----
		Vector eyes = ppev->origin + ppev->view_ofs;
		TraceResult tr;
		UTIL_TraceLine(eyes + gpGlobals->v_right * 1.0f,
			eyes + gpGlobals->v_forward * 32.0f + gpGlobals->v_right * 1.0f,
			dont_ignore_monsters, ENT(ppev), &tr);

		CBaseEntity *pKnife = TSThrowKnife(m_pPlayer, tr.vecEndPos, gpGlobals->v_forward * 1900.0f,
			wi->iKnifeType); // server-only (the client build's copy returns NULL)
		if (pKnife)
		{
			Vector dir = pKnife->pev->velocity;
			dir = (dir.Length() > 0.0f) ? dir.Normalize() : g_vecZero;
			pKnife->pev->angles = UTIL_VecToAngles(dir);
			pKnife->pev->angles.z = 90.0f;       // blade rolled edge-on
			pKnife->pev->avelocity.x = -1300.0f; // end-over-end spin (KatanaThrow uses -700)
		}
		consumed = true; // even if CreateKnife failed (the original's merge point @0x74865)

		if (!(ppev->iuser4 & 0x30c30) && (ppev->flags & FL_ONGROUND))
			m_pPlayer->m_TSAnim.Animate(m_pPlayer, TSA_CC_KNIFELAUNCH); // throw anim 32

		EMIT_SOUND_DYN(ENT(ppev), CHAN_BODY, "weapons/knife/knife_miss.wav",
			VOL_NORM, 0.8f, 0, 95 + RANDOM_LONG(0, 31)); // the throw whoosh — always
	}

	if (consumed)
	{
		if (!InfiniteClips(m_pPlayer))
		{
			// the thrown knife frees its carry slot and the move speed recomputes (the original adds
			// slotCost to player+0x548 by hand; reTS derives the slots, counting a stackable per unit of
			// its clip, so the m_iClip-- here frees exactly one knife's cost).
			m_iClip--;
			m_pPlayer->SetPlayerSpeed(GetSpeedBySlots(m_pPlayer));
		}
		if (m_iClip <= 0)
		{
			DropWeapon(m_weaponType, true); // threw the last one — leaves the arsenal, next-best deploys
		}
		else
		{
			// redraw the next knife: model-derived draw seq + draw.sc + a short idle.
			SendWeaponAnim(s_drawSeq[m_weaponType], pev->body);
			PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usDraw, 0.0, g_vecZero, g_vecZero,
				0.0, 0.0, m_weaponType, it.attach, 0, 0);
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
		}
	}
}

// @ CCombatAttack__6CTSGun (@0x86078) — kung-fu (bare hands) contextual close-combat. The original
// reads the player's movement BUTTONS + flags + speed (it has no active weapon item, gun+0xcc==0) and
// dispatches one of ~13 attack ids, each with its own anim / damage / refire / forward-lunge. This is the
// RE-faithful port (entvars decoded: button@0x180 → IN_ATTACK 1 / IN_DUCK 4 / IN_BACK 0x10 / IN_ATTACK2
// 0x800; flags byte@0x1a5 → FL_ONGROUND 0x200 / FL_DUCKING 0x4000; iuser4@0x250 = TS lock-state mask
// 0x50c30). Two PERSISTENT combo counters drive the chains: PUNCH (m_comboStep / orig this+0x9fc) = 3 jabs
// → uppercut; KICK (m_kickCombo / orig this+0xa00) = 3 kicks → spin finisher. Both reset only on a finisher/
// dive (NOT on a time gap — the original has no chaining window). The last-attack id (m_lastMeleeAnim /
// this+0xa04) makes each chain cycle without repeating. Damage (local_114): punches 35, kicks 45, back-kick
// 55. Kung-fu is FULL-AUTO (the standard ItemPostFrame holds fire → repeats at the refire cap). Refire =
// local_108 (NOT local_10c — Ghidra mis-attributed the load; the boundary-confused frame analysis shifted
// it 4 bytes): normal punch 0.25 (= DAT_00107e98, live-confirmed ~4/sec full-auto cap), uppercut 0.65,
// air-punch 0.5; basic kick 0.3, spin finisher 0.5, duckkick/sidekick-dive 1.0, back-kick 1.35 (1.0 on
// ground), flying kick 0.75. local_10c (0.1-0.35) is the VICTIM block-stun (BlockPlayer(player, *0.75)).
//
// The original kung-fu viewmodel carries ~26 sequences (ids 0xa..0x19); our v_melee.mdl has 11, so each
// original id is remapped to the nearest v_melee seq + the player-model 3rd-person kung-fu seq. The
// DISPATCH / DAMAGE / TIMING are byte-faithful; the anim is a remap (see the ledger note on the 11-seq model).
//
// On a miss the swing arms the run-through window (player+0x79c/+0x7a0, below), which PreThink counts
// down and CTSStunt::CheckForBreakables consumes; there is no attacker velocity push. The iuser4 lock-state
// (0x50c30 → forced light attack) and the player+0x814/0x815 state bits (special-punch 0x17 / forced kick
// finisher) default to "clear" — the common path. v_melee anims: 2 punch_left, 3 punch_right, 4 block_low,
// 5 kick, 6 sidekick, 7 uppercut, 8 spinkick, 9 backkick, 10 punch_right2.
void CTSGun::CCombatAttack()
{
	// RECONSTRUCTED decompile-exact from CCombatAttack (ts_i386.so @0x76078, TS_gun.cpp) — see
	// reference/dossiers/cont85/kick_decode.md. The original reads IN_ATTACK / IN_ATTACK2 itself
	// (single function, not a fSecondary split). Key facts: flDamage passed to CloseCombatAttack is
	// the SPEED-SCALED lunge (also the whiff-lunge force on a miss), NOT a flat 35/45/55; the 35/45/55
	// is the trace REACH; iAnim = the MOVE-ID (0x0a..0x19, == m_lastMeleeAnim, HandleCloseCombat's case).
	entvars_t *ppev = m_pPlayer->pev;

	// three early-return gates (@preamble): player m_flNextAttack, then the gun's two melee timers.
	if (m_pPlayer->m_flNextAttack   > UTIL_WeaponTimeBase()) return;
	if (m_flNextPrimaryAttack       > UTIL_WeaponTimeBase()) return;
	if (m_flNextSecondaryAttack     > UTIL_WeaponTimeBase()) return;

	const int   btn      = ppev->button;
	const bool  onGround = (ppev->flags & FL_ONGROUND) != 0;
	const bool  gunDuck  = (pev->flags & FL_DUCKING) != 0; // @orig this->pev->flags&0x4000 — the GUN's pev (normally clear)
	const float speed    = ppev->velocity.Length();

	// movingBack + the attack class bVar9 (2 = fwd/neutral, 3 = backward).
	UTIL_MakeVectors(Vector(0.0f, ppev->v_angle.y, ppev->v_angle.z)); // orig builds from (0, yaw, roll)
	bool movingBack;
	float fwdDot = 0.0f;
	if (speed <= 100.0f)
		movingBack = (btn & IN_BACK) != 0;
	else
	{
		Vector vn = (speed != 0.0f) ? ppev->velocity * (1.0f / speed) : Vector(0, 0, 1);
		fwdDot = DotProduct(gpGlobals->v_forward, vn);
		movingBack = fwdDot <= -0.5f;
	}
	int bVar9 = movingBack ? 3 : 2;

	const bool stunting = (ppev->iuser4 & 0x50c30) != 0; // mid dive/roll/slowmo/duck-active

	// per-branch outputs (defaults @preamble).
	int   moveId = -1;      // gun+0xa04 m_lastMeleeAnim / iAnim; -1 (0xffffffff) = no 3rd-person body anim
	int   evAnim = 0;       // a3 (0x3..0xb) -> closecombat.sc iparam2 (1st-person view-anim index)
	int   evCat  = 0;       // local_104 (1/2/3 category) -> iparam1; 0 = idle -> the whole hit tail is skipped
	float reach  = 0.0f;    // trace reach (35 punch / 45 kick / 55 backkick)
	float lunge  = 0.0f;    // speed-scaled: damage on a hit, forward push on a miss
	float refire = 0.6f;    // primary + secondary re-arm
	float whiffT = 0.0f;    // whiff-lunge window / self-block-stun scalar
	bool  blockEnable = false; // on-ground kicks self-root the attacker after the swing
	bool  unstunt = true;      // the side-kick clears this (skips the un-stunt view reset)
	int   iuserSet = 0;        // iuser4 bits to OR in (duck/side kick)
	bool  setDuckView = false; // view_ofs=(0,0,16) + m_TSStunt.m_flRollTime=2.0 (duck/side kick "land low, get up")

	if (stunting)
	{
		// A — STUNT KICK: a fixed light kick during a stunt, no body anim.
		moveId = -1; evAnim = 4; evCat = 1; reach = 35; lunge = 15; refire = 0.55f; whiffT = 0.15f;
		bVar9 = 2; m_comboStep = 0;
	}
	else if (!(btn & IN_ATTACK))
	{
		// ===== KICKS (need IN_ATTACK2; else idle) — first match wins. =====
		if (!(btn & IN_ATTACK2))
		{
			evCat = 0; // idle: no attack this frame (only the re-arm timers below run)
		}
		else if (onGround && ((btn & IN_DUCK) || gunDuck))
		{
			// B1 — DUCK KICK (0x14 "duckkick")
			moveId = 0x14; evAnim = 6; evCat = 2; reach = 45; lunge = 35; refire = 1.0f; whiffT = 0.35f;
			bVar9 = 2; blockEnable = true; iuserSet = 4 | 1; setDuckView = true;
		}
		else if (bVar9 == 3)
		{
			// B2 — BACK / JUMP KICK (0x11 "backkick") — speed-scaled, reversed sweep
			moveId = 0x11; evAnim = 7; evCat = 3; reach = 55;
			lunge = speed * 0.303030303f; if (lunge > 80.0f) lunge = 80.0f; if (lunge < 15.0f) lunge = 15.0f;
			refire = 1.35f; whiffT = 0.3f;
			if (onGround) { refire = 1.0f; blockEnable = true; }
		}
		else if (!onGround && speed > 270.0f && (btn & IN_DUCK) && fwdDot > 0.5f)
		{
			// B3 — SIDE KICK (0x10 "sidekick") — fast forward air dive + duck
			moveId = 0x10; evAnim = 8; evCat = 2; reach = 45;
			lunge = speed * 0.153846154f; if (lunge > 45.0f) lunge = 45.0f; if (lunge < 15.0f) lunge = 15.0f;
			refire = 1.0f; whiffT = 0.25f;
			m_kickCombo = 0; unstunt = false; iuserSet = 4; setDuckView = true;
		}
		else if (onGround || speed <= 200.0f)
		{
			// B4 — GROUND KICK: basic-kick combo -> a spin finisher
			evCat = 2; reach = 45;
			// player+0x815&8 DECODED (cont.86): +0x814 is the stock m_afButtonPressed int (PreThink
			// computes the pressed/released edge trio at +0x810/814/818), so byte+0x815 bit 3 =
			// IN_ATTACK2 (0x800) freshly PRESSED — a spaced CLICK forces the finisher; a held/mashed
			// re-fire (no fresh edge at the refire frame) walks the B4b combo chain instead.
			if ((m_pPlayer->m_afButtonPressed & IN_ATTACK2) != 0 || m_kickCombo > 2)
			{
				// B4a — KICK FINISHER (spin) — cycle 0xa/0xe/0xd/0xf, no repeat
				switch (m_lastMeleeAnim)
				{
				case 0x0f: moveId = 0x0a; break;
				case 0x0a: moveId = 0x0e; break;
				case 0x0e: moveId = 0x0d; break;
				case 0x0d: moveId = 0x0f; break;
				default: { int r = RANDOM_LONG(0, 3); moveId = (r==0)?0x0e : (r==1)?0x0d : (r==2)?0x0a : 0x0f; }
				}
				evAnim = 0x0a; lunge = 25; refire = 0.5f; whiffT = 0.3f;
				m_kickCombo = 0; blockEnable = true;
			}
			else
			{
				// B4b — KICK COMBO STEP (circle kick) — alternate 0xb/0xc
				switch (m_lastMeleeAnim)
				{
				case 0x0b: moveId = 0x0c; break;
				case 0x0c: moveId = 0x0b; break;
				default: moveId = (RANDOM_LONG(0, 1) == 1) ? 0x0c : 0x0b; break;
				}
				evAnim = 9; lunge = 15; refire = 0.3f; whiffT = 0.2f;
				m_kickCombo++; blockEnable = true;
			}
		}
		else
		{
			// B5 — AIR KICK (fast + airborne) — cycle 0xf/0x13/0x12, no repeat
			switch (m_lastMeleeAnim)
			{
			case 0x0f: moveId = 0x13; break;
			case 0x13: moveId = 0x12; break;
			case 0x12: moveId = 0x0f; break;
			default: { int r = RANDOM_LONG(0, 2); moveId = (r==0)?0x13 : (r==1)?0x12 : 0x0f; }
			}
			evAnim = 0x0a; evCat = 2; reach = 45;
			lunge = speed * 0.1f; if (lunge > 30.0f) lunge = 30.0f; if (lunge < 15.0f) lunge = 15.0f;
			refire = 0.75f; whiffT = 0.25f;
			m_kickCombo = 0;
		}
	}
	else
	{
		// ===== PUNCHES (IN_ATTACK held). reach 35, class 1. =====
		evCat = 1; reach = 35; bVar9 = 2;
		if (!onGround)
		{
			// C — AIR PUNCH (0x18 "punchair")
			moveId = 0x18; evAnim = 5; lunge = 25; refire = 0.5f; whiffT = 0.15f;
			m_comboStep = 0;
		}
		else if (m_comboStep > 2 || ((btn & IN_DUCK) && !gunDuck))
		{
			// D1 — UPPERCUT (0x19) — 3-punch finisher OR a duck-tap
			moveId = 0x19; evAnim = 0x0b; lunge = 30; refire = 0.65f; whiffT = 0.15f;
			m_comboStep = 0;
		}
		else if ((m_pPlayer->m_afButtonPressed & IN_ATTACK) != 0 || m_comboStep > 2)
		{
			// D2 — PUNCH (reset, 0x17 "punchright"). (player+0x814^1)&1 DECODED (cont.86): +0x814 is
			// the stock m_afButtonPressed, bit 0 = IN_ATTACK freshly PRESSED. Spaced clicks land their
			// edge on the attack frame -> punchright every time; held/mashed re-fires have no fresh
			// edge at the refire frame -> the D3 jab chain (left, left, then the D1 uppercut).
			moveId = 0x17; evAnim = 4; lunge = 20; refire = 0.35f; whiffT = 0.15f;
			m_comboStep = 0;
		}
		else
		{
			// D3 — PUNCH COMBO STEP (jab, 0x16 "punchleft")
			moveId = 0x16; evAnim = 3; lunge = 10; refire = 0.25f; whiffT = 0.10f;
			m_comboStep++;
		}
	}

	if (evCat != 0)
	{
		m_lastMeleeAnim = moveId;

#ifndef CLIENT_DLL
		// --- shared hit tail (@LAB_000869a3) — game-state + the hull sweep are server-authoritative ---

		// damage/lunge scaling: melee-bar multiplier, doubled by the KungFu powerup.
		lunge *= m_pPlayer->GetMeleeMultiplier();
		if (KungFu(m_pPlayer))
			lunge += lunge;

		// build the swing: backkick reverses (v_angle z zeroed, reach negated) + uses the human hull.
		Vector vecDir = ppev->v_angle;
		if (bVar9 == 3)
			vecDir.z = 0.0f;
		UTIL_MakeVectors(vecDir);
		Vector vecSrc = m_pPlayer->GetGunPosition();
		Vector vecEnd = vecSrc + gpGlobals->v_forward * ((bVar9 == 3) ? -reach : reach);
		const int iHull = (bVar9 != 3) ? 3 : 1;

		// @0x76cf6: the shared resolver — TSMBlur roll, CheckHull trace, validity/teammate gates,
		// VictimKungfuHold deflect (counter role-swap), disarm-pressure, >30-dmg knockback, TraceAttack.
		// flDamage is the FLAT local_114 (35/45/55, == `reach`), NOT the speed/melee/kungfu-scaled
		// `lunge` (that is local_110, and it feeds ONLY the whiff-lunge store below). Confirmed by the
		// 0x76078 disasm: the scaled slot ([esp+0x20]) is used at the post-call miss branch, while the
		// param_6 push (0x76cd0) reads the flat local_114. Passing `lunge` here made melee damage
		// scale with speed+kungfu (symptom c: duck-kick near-instant-kill) and mis-gated the >30
		// knockback (symptom b). Melee DAMAGE is flat; only the whiff push is scaled.
		CBaseEntity *pHit = m_pPlayer->CloseCombatAttack(vecSrc, vecEnd, gpGlobals->v_forward,
			iHull, reach, moveId, true, false, true, false);

		// on a MISS, arm the RUN-THROUGH window (@0x76d02-0x76d50; consumed by CTSStunt::
		// CheckForBreakables each PreThink tick — the attacker's moving hull becomes the weapon).
		// 0x79c ← the SCALED per-move value (lunge, [esp+0x20] — also the >0.001 gate);
		// 0x7a0 ← ±the FLAT damage (reach 35/45/55, [esp+0x1c]; sign flags the back-kick's
		// behind-sweep). cont.96: we had whiffT (the BlockPlayer scalar) in the window and the
		// scaled value in the force — so the run-through >30 knockback could never fire.
		if (pHit == nullptr && whiffT > 0.001f)
		{
			// cont.96 CORRECTION (user: a whiffed punch stayed armed ~10-30s and auto-hit anyone
			// approached later): the WINDOW is the SHORT whiffT (0.1-0.35s — same magnitude as the
			// katana 0.25 / wheel 0.3 windows), NOT the scaled 10-30 value (that reading made the
			// run-through a latent landmine). Force stays the FLAT ±reach (the >30 knockback gate).
			m_pPlayer->m_flWhiffLungeTime = whiffT;
			m_pPlayer->m_flWhiffLungeForce = (bVar9 == 3) ? -reach : reach; // @0x76d2c: the same local CloseCombatAttack got as its damage (@0x76cd0), negated for move 3
			m_pPlayer->m_iWhiffLungeType = 0;
		}

		// The coupled STUNT overlay (@LAB_000869a3 head). CONSUMER WIRED: the "land low, get up" recovery
		// is CTSStunt::Play's getup (fires when m_flRollTime < 0.01 → clears PRONE|STOP|ROLL, restores
		// view_ofs). The timer is m_TSStunt.m_flRollTime (stunt+0x4 == the original's player+0x4f4) — the
		// earlier bug armed a SEPARATE standalone member (m_flDuckSlashAux), so the getup never saw it and
		// the player stayed prone/low ("stuck in the floor"). [[embedded-object-offset-blindness]]
		if (unstunt && (ppev->iuser4 & TSSTUNT_WALLCLIMB_A))
		{
			// a queued wall-climb ends here: drop it + restore the standing/ducking view.
			ppev->iuser4 &= ~(TSSTUNT_WALLCLIMB_A | TSSTUNT_PRONE);
			ppev->view_ofs = (ppev->flags & FL_DUCKING) ? Vector(0, 0, 16) : Vector(0, 0, 28);
			ppev->friction = 1.0f;
			ppev->gravity = 1.0f;
		}
		// The low-view overlay — ✅ VERIFIED cont.86 (user: "exactly matches the original"); DO NOT TOUCH.
		// Recovery is CTSStunt::Play's getup: roll<0.01 OR m_fSequenceFinished (the kick anim ending,
		// ~1s), so the 2.0 roll is only the fallback and fall height shortens the post-landing tail.
		if (iuserSet)
			ppev->iuser4 |= iuserSet; // duck-kick: PRONE|STOP; side-kick: PRONE (holds the low view vs PM_Duck)
		if (setDuckView)
		{
			ppev->view_ofs = Vector(0, 0, 16);
			m_pPlayer->m_TSStunt.m_flRollTime = 2.0f; // stunt+0x4 (orig player+0x4f4) — runs down during the fall
		}

		// on-ground kicks self-root the attacker briefly after the swing (gated on the STOP bit clear).
		if (blockEnable && ((ppev->iuser4 ^ 1) & 1) != 0)
			m_pPlayer->BlockPlayer(whiffT * 0.75f);
#endif

		// events/closecombat.sc — the client owns the 1st-person anim + whoosh + surface presentation.
		// iparam1 = the 1/2/3 category, iparam2 = the view-anim index (client reads both). fparam1 = 0
		// (== (float)m_weaponType, which is TSW_MELEE=0 here).
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usCloseCombat, 0.0, g_vecZero,
			g_vecZero, 0.0f, 0.0f, evCat, evAnim, 0, 0);
#ifdef CLIENT_DLL
		// Kung-fu "bounce-back" screen-shake (client-only presentation; RE client FUN_1000af10 @0xb5e5:
		// Shake_Setup(amp = min(power,45)*0.35, 0.2, 10), only on the prediction's first run of the
		// command, @0x1000b5be). Live-measured power: punch~20, kick~25.
		if (g_runfuncs)
		{
			float kfPower = (evCat >= 2) ? 25.0f : 20.0f;
			TS_TriggerViewShake(kfPower * 0.35f); // 0.35 -> punch 7.0, kick 8.75
		}
#endif
	}

	// re-arm timers ALWAYS (@orig gun+0x9c/0xa0 = t+refire, gun+0xa8 = t+2.0), even on an idle call.
	m_flNextPrimaryAttack   = UTIL_WeaponTimeBase() + refire;
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + refire;
	m_flTimeWeaponIdle      = UTIL_WeaponTimeBase() + 2.0f;
	m_pPlayer->m_flKungFuBlock   = 0.0f;   // player+0x7a8
	m_pPlayer->m_flLastMeleeRate = refire; // player+0x4f8
}

// @ CCombatBlock__6CTSGun (@0x85ebc) — kung-fu "reload" = the block taunt. Plays the "hooah"
// @ CCombatBlock__6CTSGun — RELOAD with ANY melee weapon (kung-fu/knife/katana/seal-knife) =
// the block taunt: opens the per-weapon 0.5s deflect window, drains 33 stamina via
// CBasePlayer::UpdateMeleeBar (which handles the depleted 1.5s lockout + the KFuPower send),
// and gates attacks. RE-faithful gates: not mid-attack, not stunting (iuser4 & 0x70c30), bar
// must be non-empty (>= 0.01 — a LOW bar still blocks, then empties + locks out).
// The window is a VALUE 0.5 counted down in PreThink (addendum L2), floor 0.
// @ CCombatBlock__6CTSGun (@0x75ebc, decompile-exact) — the melee BLOCK. Gates: the player's
// m_flNextAttack AND all three weapon timers (primary/secondary/ALTERNATE) clear, not stunting
// (iuser4 & 0x70c30), melee bar >= 0.01. Opens the fire-type-matched block window (+0x7a8 bare /
// +0x7ac knife / +0x7b0 katana = 0.5s), then the CLIENT owns the presentation via
// events/closecombat.sc — iparam1 = 0 (BLOCK: the handler plays player/block.wav and returns),
// iparam2 = the anim-block index (1 bare hands / 0x12 armed), fparam1 = (float)weaponType.
// 3rd-person Animate(TSA_CC_BLOCK) ALWAYS (armed too); UpdateMeleeBar(33); primary+secondary
// +0.5, idle +3.0.
void CTSGun::CCombatBlock()
{
	if (m_pPlayer->m_flNextAttack > UTIL_WeaponTimeBase())
		return; // global attack gate (incl. the depleted-bar 1.5s lockout)
	if (m_flNextPrimaryAttack > UTIL_WeaponTimeBase() ||
		m_flNextSecondaryAttack > UTIL_WeaponTimeBase() ||
		m_flNextAlternateAttack > UTIL_WeaponTimeBase())
		return; // mid-attack / already blocking (all THREE gates, @0x75ee4)
	if ((m_pPlayer->pev->iuser4 & 0x70c30) != 0)
		return; // stunting (dive/roll/wall-jump family) — no block
	if (m_pPlayer->m_flMeleeBar < 0.01f)
		return; // empty bar — nothing to block with

	// Open the fire-type-matched BLOCK window + pick the event's anim index.
	int blockIdx;
	if (!m_pActiveItem)
	{
		m_pPlayer->m_flKungFuBlock = 0.5f;
		blockIdx = 1; // the bare-hands block anim slot
	}
	else
	{
		if (m_pActiveInfo && m_pActiveInfo->iFireType == TSFT_KATANA)
			m_pPlayer->m_flKatanaBlock = 0.5f;
		else if (m_pActiveInfo && m_pActiveInfo->iFireType == TSFT_KNIFE)
			m_pPlayer->m_flKnifeBlock = 0.5f;
		blockIdx = 0x12; // the armed block anim slot
	}

	// events/closecombat.sc — the block presentation (the client plays block.wav + block[idx]).
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usCloseCombat, 0.0, g_vecZero,
		g_vecZero, (float)m_weaponType, 0.0, 0, blockIdx, 0, 0);

	m_pPlayer->m_TSAnim.Animate(m_pPlayer, TSA_CC_BLOCK); // 3rd-person block pose (armed too)
#ifndef CLIENT_DLL
	m_pPlayer->UpdateMeleeBar(33.0f); // drain (handles depletion lockout + the KFuPower send; server-only)
#endif
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.5f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.5f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
}

// @ AlternateAttack__6CTSGun (@0x71ba8, decompile-exact) — the +alt2 (IN_CANCEL) input:
// `if (m_pActiveItem && iColdCockDamage > 0) ColdCock();`. NO timer gate here — the base
// ItemPostFrame dispatches it through CanAttack(m_flNextAlternateAttack) (gun+0xa4), which
// ColdCock re-arms (+0.725).
void CTSGun::AlternateAttack()
{
	if (!m_pActiveItem)
		return;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	if (!wi || wi->iColdCockDamage <= 0)
		return;
	ColdCock();
}

// @ ColdCock__6CTSGun @0x71dd0 — a quick melee bash with the held weapon (CBasePlayer::
// ColdCockAttack@0x86610 in the original). Short hull-trace in front; on hit deals club damage.
// The pistol-whip/rifle-butt: per-weapon damage from the table (iColdCockDamage, x2/3 while
// stunting), range 35, hull 3, routed through the ColdCockAttack twin with all four behavior bools
// FALSE. Cadence 0.725 (x1.5 stunting). Interrupts zoom/reload/grenade-idle. Attacker confirm on a
// player hit = kevlarhit.wav pitch 60-91. events/coldcock.sc carries the rest, hit or miss.
void CTSGun::ColdCock()
{
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	if (!wi || wi->iColdCockDamage <= 0)
		return; // this weapon can't coldcock

	const bool stunting = (m_pPlayer->pev->iuser4 & 0x70c30) != 0;

	UTIL_MakeVectors(m_pPlayer->pev->v_angle);

	// @0x81e90: the bash INTERRUPTS what the weapon was doing — a cooked grenade is force-thrown
	// (the put-away path), any in-progress reload aborts.
	if (m_pActiveInfo)
	{
		if (m_pActiveInfo->iFireType == TSFT_GRENADE)
			GrenadeIdle(1);
		StopReload();
	}

	const float cadence = stunting ? 0.725f * 1.5f : 0.725f;
#ifndef CLIENT_DLL
	Unzoom();
#endif
	// ALL FOUR gates re-arm to the cadence (alternate/secondary/primary + the player's
	// m_flNextAttack), idle +2.0 (@0x81f0c).
	m_flNextAlternateAttack = UTIL_WeaponTimeBase() + cadence;
	m_flNextSecondaryAttack = m_flNextAlternateAttack;
	m_flNextPrimaryAttack = m_flNextAlternateAttack;
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + cadence;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;

	float dmg = (float)wi->iColdCockDamage;
	if (stunting)
		dmg *= 2.0f / 3.0f;

	// events/coldcock.sc (@0x81f80, gated on the item being owned): the CLIENT owns the whole
	// presentation — the block[28] viewmodel anim, the ALWAYS-whoosh, the texture note + smoke
	// puff on a hit (EV_TSColdCock). iparam1=type, iparam2=attach; fparam1/2 carry the aim dir
	// x/y (unconsumed by the handler — kept for byte-faithfulness). client.dll's predicted ColdCock
	// plays it with any active item (@0x10008dc8): the client marks no item owned.
#ifdef CLIENT_DLL
	if (m_pActiveItem)
#else
	if (m_pActiveItem && m_pActiveItem->owned)
#endif
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usColdCock, 0.0, g_vecZero,
			g_vecZero, gpGlobals->v_forward.x, gpGlobals->v_forward.y,
			m_weaponType, m_pActiveItem->attach, 0, 0);

	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecEnd = vecSrc + gpGlobals->v_forward * 35.0f;

#ifndef CLIENT_DLL
	// No damagemult: the original reads it only for bullets (ShootBulletsPlayer @0x5fde0,
	// UpdateBullet @0x5f256), cheat-gated.
	CBaseEntity *pHit = m_pPlayer->ColdCockAttack(vecSrc, vecEnd, gpGlobals->v_forward,
		3, dmg, stunting ? -1 : 39 /*anim 39, none while stunting*/,
		false, false, false, false);
	if (pHit && pHit->IsPlayer())
	{
		// attacker confirm — the kevlar "clonk" on CHAN_BODY, attn 0.8 (RE: pitch 60 + rand(0,31))
		EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_BODY, "player/kevlarhit.wav", VOL_NORM,
			0.8f, 0, 60 + RANDOM_LONG(0, 31));
	}
	// (the miss whoosh now rides events/coldcock.sc — the client plays closecombat.wav on
	// EVERY bash, hit or miss)
#endif
}

// (TSCheckRunThrough DELETED cont.96 — it was a second reconstruction of the SAME original
// function as CTSStunt::CheckForBreakables @0x8bd9c (the "0x9bd9c" in its old comment was a
// typo; nm has ONE symbol). The faithful, window-aware version lives in ts_stunt.cpp; keeping
// both broke the one-hit-per-victim dedupe: this one NULLed m_hRunThroughLast when grounded,
// letting the sister re-hit the same victim every other tick.)

// Reload the active weapon from its own per-slot reserve (the original draws from
// the per-caliber armament; here each arsenal slot carries its own spare rounds).
// @ GrenadeFire__6CTSGun @0x732f0 — PULL THE PIN (decompile-exact): gated on an idle phase
// (item+0x8 == 0) + a loaded grenade; plays the pin-pull viewmodel anim (anim block [19]) +
// grenade.sc iparam1=19, arms the COOK phase (item+0x8 = 1), re-arms both attacks + idle at
// +0.7, and stamps the cook member (gun+0xa0c = time+5; nothing reads it). NO fuse starts here
// — the grenade is thrown (fuse 3.0 FROM THE THROW) by GrenadeIdle when the button releases.
void CTSGun::GrenadeFire()
{
	if (m_weaponType == TSW_MELEE) // original: m_pActiveItem == NULL
		return;
	if (m_burstOrPhase != 0 || m_iClip <= 0) // item+0x8 phase busy / no grenade
		return;

	SendWeaponAnim(2, pev->body); // m61 anim block [19] = seq 2 (pin pull; record+0x26)
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usGrenade, 0.0, g_vecZero, g_vecZero,
		0.0, 0.0, 19, 0, 0, 0);
	m_burstOrPhase = 1; // item+0x8 = COOKED (the pin is out)
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.7f;
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.7f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.7f;
	m_flGrenadeCook = gpGlobals->time + 5.0f; // gun+0xa0c, written here only and read nowhere
}

// @ GrenadeIdle__6CTSGuni @0x74f18 — the cook machine (decompile-exact). Phase 1 (cooked):
// while ATTACK or ATTACK2 is held (and not forced) keep cooking; on release THROW — pitch-shaped
// force (90 - shapedPitch)*7 capped 750; overhand (altMode 0) from the eyes at full force with
// avelocity.x=200; underhand (altMode 1) from the FEET at 2/3 force, friction 0.4,
// avelocity=(-100,600); fuse 3.0 FROM THE THROW; throw anim (block [20]/[21]) + grenade.sc
// iparam1 20/21 + body anim 0x35; timers +0.5; the last grenade strips the weaponmodel;
// phase -> 2 (or a forced throw consumes immediately). Phase 2: the post-throw tick consumes —
// slot refund + speed recompute, DropWeapon at 0 left, else redraw (block [2]) + idle 2.0.
// Phase 0: idle +3.0.
void CTSGun::GrenadeIdle(int fForce)
{
	if (m_weaponType == TSW_MELEE) // original: m_pActiveItem == NULL
		return;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	TSItem &it = m_items[m_weaponType];
	bool consume = false;

	if (m_burstOrPhase == 1)
	{
		if ((m_pPlayer->pev->button & (IN_ATTACK | IN_ATTACK2)) && fForce == 0)
			return; // still cooking

		// pitch-shaped throw force: shaped = pitch*(up 1.111 / down 0.888) - 10;
		// force = (90 - shaped) * 7, capped 750 (aim up = harder throw).
		Vector ang = m_pPlayer->pev->v_angle;
		const float px = ang.x;
		ang.x = px * ((px < 0.0f) ? (float)(10.0 / 9.0) : (float)(8.0 / 9.0)) - 10.0f;
		float force = (90.0f - ang.x) * 7.0f;
		if (force > 750.0f)
			force = 750.0f;
		UTIL_MakeVectors(ang);

		Vector vecSrc, vecVel;
		if (it.altMode == 0)
		{
			// OVERHAND: from the eyes, full force + the thrower's velocity.
			vecSrc = m_pPlayer->pev->origin + m_pPlayer->pev->view_ofs + gpGlobals->v_forward * 16.0f;
			vecVel = gpGlobals->v_forward * force + m_pPlayer->pev->velocity;
		}
		else
		{
			// UNDERHAND roll: from the FEET, 2/3 force.
			vecSrc = m_pPlayer->pev->origin + gpGlobals->v_forward * 16.0f;
			vecVel = gpGlobals->v_forward * (force * (2.0f / 3.0f)) + m_pPlayer->pev->velocity;
		}

		if (wi->iKnifeType == 4) // info+0x20 == 4 — the m61 projectile-type code
		{
			CBaseEntity *pGren = TSThrowM61(m_pPlayer->pev, vecSrc, vecVel, 3.0f); // FUSE 3.0 from the throw
			if (pGren)
			{
				if (it.altMode == 0)
				{
					pGren->pev->avelocity.x = 200.0f; // overhand tumble
				}
				else
				{
					pGren->pev->friction = 0.4f;      // the roll
					pGren->pev->avelocity.y = 600.0f;
					pGren->pev->avelocity.x = -100.0f;
				}
			}
		}

		SendWeaponAnim(it.altMode == 0 ? 3 : 4, pev->body); // m61 block [20]/[21] (record+0x28/+0x2a)
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usGrenade, 0.0, g_vecZero, g_vecZero,
			0.0, 0.0, (it.altMode != 0) + 0x14, 0, 0, 0); // iparam1 = 20 overhand / 21 underhand
		m_pPlayer->m_TSAnim.Animate(m_pPlayer, 0x35); // 3rd-person grenade throw

		m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.5f;
		m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.5f;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;
		if (m_iClip == 1)
			m_pPlayer->pev->weaponmodel = 0; // the last grenade leaves the hand
		if (fForce == 0)
			m_burstOrPhase = 2; // the next idle tick consumes
		else
		{
			m_burstOrPhase = 0;
			consume = true; // forced (holster) throw consumes NOW
		}
		m_iClip--;
	}
	else if (m_burstOrPhase == 2)
	{
		m_burstOrPhase = 0;
		consume = true;
	}
	else
	{
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
	}

	if (consume)
	{
		// the thrown grenade frees its slots + the speed recomputes (our slots derive from the
		// arsenal, so the recompute shifts on the last one's DropWeapon).
		m_pPlayer->SetPlayerSpeed(GetSpeedBySlots(m_pPlayer));
		if (m_iClip < 1)
		{
			DropWeapon(m_weaponType, true);
		}
		else
		{
			SendWeaponAnim(s_drawSeq[m_weaponType], pev->body); // block [2] = 1 — redraw the next grenade
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
		}
	}
}

// @ KatanaFire__6CTSGun @0x733f0 — the katana slash, a 5-STANCE system (decompile + raw-disasm exact;
// reference/dossiers/cont81/analysis_katanafire.md). Stance -> {reach, SIGNED dmg, delay, viewmodel
// anim (the event's iparam1), 3rd-person anim, post-swing lock}:
//   STUNT (iuser4 & 0x70c30)      35 /  35 / 1.0 / 17 / -1 (keep the stunt anim)
//   BACK  (moving backward)       55 / -75 / 1.0 / 15 / 35  + lock — swings BEHIND the player
//   DUCK  (ground + duck)         50 /  65 / 0.9 / 17 / 37  + lock + PRONE|STOP + view_ofs 16 + aux 2.0
//   NORMAL (ground)               50 /  45 / 0.7 / 17|16 / 33|34 alternating (m_lastMeleeAnim 1<->2)
//   AIR                           50 / clamp(speed*2/13, 35, 55) / 1.1 / 17 / 36
// dmg x GetMeleeMultiplier; negative dmg traces BACKWARD (CheckHull hull 1, else 3); VictimKatanaHold
// deflects into the block presentation; a player hit bloodies the blade (item+0x8 + pev->body=1) and
// plays hit-body.wav on the VICTIM; a miss arms the whiff-lunge (0.25s window, SIGNED force, type 2).
// katana.sc plays EVERY swing (FEV_NOTHOST, iparam1=vmAnim, iparam2=bloody) — the whoosh/viewmodel
// presentation is CLIENT-side; the server emits no swing sound and sends no SendWeaponAnim.
void CTSGun::KatanaFire()
{
	if (m_weaponType == TSW_MELEE) // original gate: m_pActiveItem == NULL
		return;
	entvars_t *ppev = m_pPlayer->pev;
	TSItem &it = m_items[m_weaponType];

	UTIL_MakeVectors(ppev->v_angle);
	const bool stunting = (ppev->iuser4 & 0x70c30) != 0;
	const bool onGround = (ppev->flags & FL_ONGROUND) != 0;
	const float speed = ppev->velocity.Length();

	// moving backward: slow -> the +back key; fast -> velocity vs facing (dot <= -0.5).
	bool movingBack;
	if (speed <= 100.0f)
		movingBack = (ppev->button & IN_BACK) != 0;
	else
		movingBack = DotProduct(gpGlobals->v_forward, ppev->velocity * (1.0f / speed)) <= -0.5f;

	int reach, vmAnim, bodyAnim;
	float dmg, delay;
	bool lock = false;
	if (stunting)
	{
		reach = 35; dmg = 35.0f; delay = 1.0f; vmAnim = 17; bodyAnim = -1;
	}
	else if (movingBack)
	{
		reach = 55; dmg = -75.0f; delay = 1.0f; vmAnim = 15; bodyAnim = TSA_CC_KATANA3; // 35
		lock = true;
	}
	else if (onGround && ((ppev->button & IN_DUCK) || (pev->flags & FL_DUCKING)))
	{
		// NB: the FL_DUCKING test reads the GUN's own pev (original-source slip @0x73624 — a carried
		// item never has it set, so IN_DUCK is the live trigger). Replicated as-is for fidelity (L4).
		reach = 50; dmg = 65.0f; delay = 0.9f; vmAnim = 17; bodyAnim = TSA_CC_KATANADUCK; // 37
		lock = true;
		ppev->iuser4 |= TSSTUNT_PRONE | TSSTUNT_STOP; // drop into the ducking hull + damp velocity
		ppev->view_ofs = Vector(0, 0, 16);
		m_pPlayer->m_TSStunt.m_flRollTime = 2.0f; // stunt+0x4 (orig player+0x4f4) — arms Play's getup ("land low, get up")
	}
	else if (onGround)
	{
		reach = 50; dmg = 45.0f; delay = 0.7f;
		// alternate the two normal slashes without repeating (this+0xa04 cycle 1<->2).
		if (m_lastMeleeAnim != 1 && m_lastMeleeAnim != 2)
			m_lastMeleeAnim = RANDOM_LONG(1, 2);
		if (m_lastMeleeAnim == 1)
		{
			vmAnim = 17; bodyAnim = TSA_CC_KATANA1; m_lastMeleeAnim = 2; // 33
		}
		else
		{
			vmAnim = 16; bodyAnim = TSA_CC_KATANA2; m_lastMeleeAnim = 1; // 34
		}
	}
	else
	{
		reach = 50; delay = 1.1f; vmAnim = 17; bodyAnim = TSA_CC_KATANAAIR; // 36
		dmg = speed * (2.0f / 13.0f); // air slash scales with speed
		if (dmg < 35.0f) dmg = 35.0f;
		if (dmg > 55.0f) dmg = 55.0f;
	}

	m_pPlayer->m_TSAnim.Animate(m_pPlayer, bodyAnim); // -1 (stunt) is a no-op

#ifndef CLIENT_DLL // hit resolution is server-authoritative (house pattern, see ColdCock/CCombatAttack)
	dmg *= m_pPlayer->GetMeleeMultiplier();

	// signed damage sets the trace direction: the back-slash sweeps BEHIND the player.
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecEnd = vecSrc + gpGlobals->v_forward * ((dmg < 0) ? -(float)reach : (float)reach);
	TraceResult tr;
	m_pPlayer->CheckHull(vecSrc, vecEnd, tr, (dmg < 0) ? 1 : 3);

	bool whiff = true;
	if (tr.flFraction < 1.0f)
	{
		CBaseEntity *pEnt = tr.pHit ? CBaseEntity::Instance(tr.pHit) : nullptr;
		if (pEnt && pEnt->IsAlive() && !(pEnt->pev->effects & EF_NODRAW))
		{
			whiff = false;
			if (!VictimKatanaHold(m_pPlayer, pEnt, dmg))
			{
				ClearMultiDamage();
				pEnt->TraceAttack(ppev, fabsf(dmg), gpGlobals->v_forward, &tr, DMG_CLUB | DMG_NEVERGIB);
				ApplyMultiDamage(ppev, ppev);
				if (pEnt->IsPlayer())
				{
					it.bloody = 1;   // item+0x8 — bloody blade (rides the event as iparam2)
					pev->body = 1;   // bloody katana w_/p_ submodel
					EMIT_SOUND_DYN(ENT(pEnt->pev), CHAN_BODY, "weapons/katana/hit-body.wav",
						VOL_NORM, 0.8f, 0, 95 + RANDOM_LONG(0, 31)); // from the VICTIM's edict
				}
			}
		}
	}
	if (whiff)
	{
		// miss (or a dead/nodraw target): arm the whiff-lunge — SIGNED force, katana flavor.
		m_pPlayer->m_flWhiffLungeTime = 0.25f;
		m_pPlayer->m_flWhiffLungeForce = dmg;
		m_pPlayer->m_iWhiffLungeType = 2;
	}

	// post-swing root (back/duck slash): skipped when STOP is already up (the duck branch set it).
	if (lock && !(ppev->iuser4 & TSSTUNT_STOP))
	{
		m_pPlayer->m_flBlockTime = delay * 0.9f;
		ppev->iuser4 |= TSSTUNT_STOP;
	}
#endif

	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usKatana, 0.0, g_vecZero, g_vecZero,
		0.0, 0.0, vmAnim, it.bloody, 0, 0);

	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + delay;
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + delay;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
	m_pPlayer->m_flLastMeleeRate = delay; // player+0x4f8
}

// @ KatanaThrow__6CTSGun @0x73b68 — right-click with the katana HURLS it: a 1000 u/s CTSKnife
// (type code 8, s_weapon_info+0x20) spawned at a wall-clipped muzzle point, rolled edge-on with a
// -700 end-over-end spin. The weapon then LEAVES the arsenal (drop + next-best select + speed
// recompute) unless the infinite-clips powerup keeps it. No viewmodel anim, no client event —
// a server entity + one server sound; the deploy of the next weapon is the visible presentation.
void CTSGun::KatanaThrow()
{
	if (m_weaponType == TSW_MELEE) // original gate: m_pActiveItem == NULL
		return;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);

	// both attack timers re-arm at info+0x28 (katana 0.7 — moot: the weapon is dropped below).
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + wi->flSpread;
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + wi->flSpread;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;

	// spawn point = the first hit within eye + 32*forward (+1u right) — the blade can't start in a wall.
	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	Vector eyes = m_pPlayer->pev->origin + m_pPlayer->pev->view_ofs;
	TraceResult tr;
	UTIL_TraceLine(eyes + gpGlobals->v_right * 1.0f,
		eyes + gpGlobals->v_forward * 32.0f + gpGlobals->v_right * 1.0f,
		dont_ignore_monsters, ENT(m_pPlayer->pev), &tr);

	CBaseEntity *pKnife = TSThrowKnife(m_pPlayer, tr.vecEndPos, gpGlobals->v_forward * 1000.0f,
		wi->iKnifeType); // server-only (the client build's copy returns NULL)
	if (pKnife)
	{
		// the thrower orients the projectile (the original does this here, not in CTSKnife::Spawn).
		Vector dir = pKnife->pev->velocity;
		dir = (dir.Length() > 0.0f) ? dir.Normalize() : g_vecZero;
		pKnife->pev->angles = UTIL_VecToAngles(dir);
		pKnife->pev->angles.z = 90.0f;      // blade rolled edge-on
		pKnife->pev->avelocity.x = -700.0f; // end-over-end spin (KnifeFire's knife uses -1300)
	}

	if (!(m_pPlayer->pev->iuser4 & 0x30c30) && (m_pPlayer->pev->flags & FL_ONGROUND))
		m_pPlayer->m_TSAnim.Animate(m_pPlayer, TSA_CC_KNIFELAUNCH); // 3rd-person launch gesture, not mid-stunt

	EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_BODY, "weapons/katana/miss.wav",
		VOL_NORM, 0.8f, 0, 95 + RANDOM_LONG(0, 31)); // orig 8-arg EMIT_SOUND_DYN, w 0.5 = the default (verified, census @0x73fb3)

	if (!InfiniteClips(m_pPlayer))
	{
		// thrown away for good: remove from the arsenal + auto-select next-best, then recompute the
		// move speed from the freed slots (the original refunds slotCost into player+0x548 by hand;
		// our slots derive from the owned items, so drop first, then recompute).
		DropWeapon(m_weaponType, true);
		m_pPlayer->SetPlayerSpeed(GetSpeedBySlots(m_pPlayer));
	}
}

// @ AkimboFire__6CTSGun — the dual-wield fire-types (4 combinable, 5 gold-only). The
// faithful engine splits the clip into a left/right barrel (m_pActiveItem+0x1c/+0x20)
// and, in alternating fire-mode, fires one barrel per trigger (IN_ATTACK vs IN_ATTACK2)
// from a ±5u muzzle offset. FUNCTIONAL reconstruction on our single-clip arsenal: two
// guns fire continuously while held, alternating the muzzle side each shot.
bool CTSGun::AkimboFire(const s_weapon_info *wi)
{
	TSItem &it = m_items[m_weaponType];

	// @ AkimboFire barrel state machine (ts_i386.so @0x729c0, decompile-exact). ONE per-item byte
	// holds the barrel side — item+0x24 (our it.altMode), NOT m_fInSpecialReload — and it rides
	// clientdata.vuser4.x so it PREDICTS. Primary/SecondaryAttack LEAVE IT ALONE (the original's
	// @0x71a20/@0x717a8 only touch item+0x24 for grenade/knife), so the per-shot flip below persists
	// across pulls -> true alternation. Convention: altMode 0 = RIGHT barrel, 1 = LEFT (the
	// original's decrement: pumpToggle==0 -> rightClip--).
	//  - plain AUTO (the uzis' default): BOTH barrels fire AT ONCE while held;
	//  - plain SEMI: ALTERNATE barrels per pull (flip item+0x24, skipping a dry one);
	//  - the FREE modes (4 "Free Semi" / 5 "Free Full"): the CLICKED barrel only (IN_ATTACK=RIGHT,
	//    IN_ATTACK2=LEFT), each draining ITS OWN clip and dry-clicking when THAT barrel is empty.
	// @0x829e5: in the FREE modes, BOTH buttons together = a LINKED both-barrels shot — "Free Full"
	// (5) checks the HELD buttons, "Free Semi" (4) the PRESSED cache (player+0x814). The both-empty
	// dry for the non-free / linked paths already happened in Fire() (m_iClip < 1 gate).
	const bool freeMode = (m_fireMode == TSFM_AKIMBO_ALT || m_fireMode == TSFM_AKIMBO_BOTH);
	const unsigned int ccBtns = (m_fireMode == TSFM_AKIMBO_BOTH)
		? (unsigned int)m_pPlayer->pev->button : (unsigned int)m_pPlayer->m_afButtonPressed;
	const bool fireBoth = (m_fireMode == TSFM_AUTO) ||
		(freeMode && (ccBtns & IN_ATTACK) != 0 && (ccBtns & IN_ATTACK2) != 0);

	int sides[2], nSides = 0; // 0 = right, 1 = left
	if (fireBoth)
	{
		if (it.rightClip > 0) sides[nSides++] = 0;
		if (it.leftClip  > 0) sides[nSides++] = 1;
		if (nSides == 0)
		{
			m_iClip = 0;
			EmptySound(); // @0x72adc — the in-akimbo dry click (0.1s re-arm inside)
			return false;
		}
		// advance the toggle too (the original flips item+0x24 on this path as well).
		it.altMode = it.altMode ? 0 : 1;
		if (it.rightClip < 1) it.altMode = 1;
		if (it.leftClip  < 1) it.altMode = 0;
	}
	else if (freeMode)
	{
		// free single-click: the CLICKED barrel only (item+0x24 set from the held button).
		if ((m_pPlayer->pev->button & IN_ATTACK) != 0)
		{
			if (it.rightClip < 1) { EmptySound(); return false; } // right dry — the other may be loaded
			it.altMode = 0; // RIGHT
		}
		if ((m_pPlayer->pev->button & IN_ATTACK2) != 0)
		{
			if (it.leftClip < 1) { EmptySound(); return false; } // left dry
			it.altMode = 1; // LEFT
		}
		sides[nSides++] = it.altMode;
	}
	else // plain SEMI (and burst): ALTERNATE barrels per pull — flip item+0x24, then clamp to a
	{    // loaded barrel. The persisted item+0x24 makes shot N fire the OTHER barrel from shot N-1.
		it.altMode = it.altMode ? 0 : 1;
		if (it.rightClip < 1) it.altMode = 1; // right dry -> LEFT
		if (it.leftClip  < 1) it.altMode = 0; // left dry  -> RIGHT
		sides[nSides++] = it.altMode;
	}

	m_pPlayer->pev->effects |= EF_MUZZLEFLASH;
	// (3rd-person shoot body set by CTSGun::Fire's direct Animate call, not via the empty TS SetAnimation.)

	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	Vector vecAiming = gpGlobals->v_forward;
	float flSpread = wi->flSpread > 0 ? wi->flSpread : 0.02f;
	// @0x72c69: the akimbos run the dynamic-accuracy growth too — recoil DOUBLED on a linked
	// both-barrels shot; the (non-integral) silencer flag tightens.
	const bool akSilenced = (it.attach & 1) != 0 && (wi->attachBuiltin & 1) == 0;
	if (wi->bUsesSpread)
		ScaleSpread(flSpread, (nSides == 2) ? wi->flRecoil * 2.0f : wi->flRecoil, akSilenced);
	// @0x72c9c: the silencer damage penalty (x0.775)
	const float akDamage = akSilenced ? wi->flDamage * 0.775f : wi->flDamage;

	for (int s = 0; s < nSides; s++)
	{
		const int leftSide = sides[s];
		if (leftSide) it.leftClip--; else it.rightClip--;
		// Muzzle offset to the firing gun's side (+5u right / -5u left), like the original.
		Vector vecSrc = m_pPlayer->GetGunPosition() + gpGlobals->v_right * (leftSide ? -5.0f : 5.0f);
#ifndef CLIENT_DLL
		// @0x72e62/0x72fff/0x73133: one pellet per fired barrel through the TS bullet path.
		m_pPlayer->ShootBulletsPlayer(1, vecSrc, vecAiming,
			Vector(flSpread, flSpread, flSpread), akDamage, wi->iKnifeType,
			wi->iCaliber, m_pPlayer->random_seed);
#else
		m_pPlayer->FireBulletsPlayer(1, vecSrc, vecAiming,
			Vector(flSpread, flSpread, flSpread), 8192, BULLET_PLAYER_9MM,
			0, (int)(wi->flDamage * TSDamageMult()), m_pPlayer->pev, m_pPlayer->random_seed);
#endif
	}
	m_iClip = it.leftClip + it.rightClip; // total clip mirrors the two barrels
	// @0x82c3a: a fire-type-4 akimbo mirrors one barrel into its SINGLE's parked clip after
	// every shot (the same mirror the reload completion keeps).
	if (wi->iFireType == TSFT_AKIMBO && wi->iSingleType >= 0 && wi->iSingleType < TSW_COUNT &&
		wi->iSingleType != m_weaponType)
		m_items[wi->iSingleType].clip = it.leftClip;

	// (the viewmodel anim rides the EVENT now — EV_TSFireLinked plays block[3+RL(0,2)] for the
	// both-barrels shot; EV_TSFire plays block[22/23] left / [24/25] right off bparam1)

	// The ORIGINAL wire (guntail dossier §5): the BOTH-barrel shot is its OWN EVENT —
	// events/firelinked.sc (one bang = wi+0x306, two flares/shells, two volleys) — while a
	// SINGLE akimbo shot rides fire.sc with bparam1 = the barrel (0 right / 1 left) and
	// bparam2 = the FireBullets SEED (our old bparam2=side extension collided with the seed;
	// fixed). bparam1 is item+0x24 on both events (@0x731e2): 1 when the left barrel fired, 0 the
	// right; the linked shot sends the toggle as it was flipped. fparam1 = fparam2 = the spread
	// (@0x7321e pushes it twice).
	if (nSides == 2)
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usFireLinked, 0.0, g_vecZero,
			g_vecZero, flSpread, flSpread, m_weaponType, it.attach, it.altMode,
			m_pPlayer->random_seed);
	else
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usFire, 0.0, g_vecZero, g_vecZero,
			flSpread, flSpread, m_weaponType, it.attach, sides[0],
			m_pPlayer->random_seed);
#ifdef CLIENT_DLL
	// The aim kick, from the prediction as client.dll's AkimboFire 0x100096f0 does: the weapon's kick,
	// doubled on the linked shot, with the silencer flag.
	TS_AddViewRecoil((nSides == 2) ? 2.0f * wi->flViewKick : wi->flViewKick, wi->iFireType, akSilenced,
		DoubleFireRate(m_pPlayer), gpGlobals->time);
#endif

	// @0x73259 (client.dll 0x100096f0 the same): 0.03 between a burst's rounds and 0.2 after its last,
	// 0.1 in semi and free semi, else the weapon's rate, for both triggers; the idle 2 s on.
	// DoubleFireRate's 1.5x comes from the PostThink timer decrement only (see StandardFire note).
	float flRate;
	if (m_fireMode == TSFM_BURST)
		flRate = (m_burstOrPhase == 1) ? 0.2f : 0.03f;
	else if (m_fireMode == TSFM_SEMI || m_fireMode == TSFM_AKIMBO_ALT)
		flRate = 0.1f;
	else
		flRate = wi->flFireRate > 0 ? wi->flFireRate : 0.1f;
	m_flNextPrimaryAttack = (UTIL_WeaponTimeBase() + flRate);
	m_flNextSecondaryAttack = m_flNextPrimaryAttack; // the right-barrel trigger shares the cadence
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 2.0f;
	return true;
}

// @ AddItem__6CTSGuni @0x80794 — attach a gun item (silencer/laser/flashlight/scope: bits 1/2/4/8) to
// the ACTIVE weapon. The original validates against the weapon's supported-attachment mask
// (s_weapon_info+0xe0) and a conflicting/silenced-base mask (+0xe4), refuses a duplicate, OR's the bits
// into the active item's attach (item+0x10) — and into the per-slot save array — then re-selects/deploys
// to refresh the attachment'd model. In our arsenal the active item IS m_items[m_weaponType], so one
// store covers both; the two masks are attachSupported and attachBuiltin below.
// The attach mask travels to the client in the fire event (iparam2 — see StandardFire/AkimboFire).
// (CTSGun::SendWeaponAnim(int) shadow DELETED, fold-plan Y4: the original has no such method —
// slot 0x174 = CBasePlayerWeapon::SendWeaponAnim(int,int,int) @0x900d0. Callers pass the body
// they already hold: Deploy @0x71030 computes GetBodyByItems ONCE into pev->body, later sites
// pass pev->body; HolsterWeapon @0x75698 passes 0 explicitly. pev->body is refreshed at every
// attach mutation point (Deploy; AddItem redeploys) — verified before the fold.)

int CTSGun::AddItem(int bits)
{
	if (m_weaponType < 0 || m_weaponType >= TSW_COUNT)
		return 0;
	TSItem &it = m_items[m_weaponType];
	if (!it.owned)
		return 0;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	// RE @0x80794: refuse if the weapon doesn't support the attachment (not in +0xe0), if it's a
	// built-in/conflicting attachment (+0xe4, e.g. a silencer on the silenced-base MP5SD), or if it's
	// already attached. Then OR the bits onto the active item's attach.
	if ((bits & wi->attachSupported) == 0)   return 0; // unsupported
	if ((bits & wi->attachBuiltin) != 0)     return 0; // built-in / can't re-add
	if ((bits & it.attach) != 0)             return 0; // already attached
	it.attach |= bits;
	// Laser/flashlight start ON (maximal active — the player toggles them OFF as they play;
	// user-verified vs the live original). In the original the spawn loadout carries the
	// attach mask INSIDE tkw -> TakeWeapon's fresh-attach path grants active there; until our
	// buy grammar moves onto that path, grant it here too so a tki-delivered attachment
	// behaves identically.
	it.active |= (bits & 6);

	// @0x80794: MIRROR the bits onto the akimbo pair — an akimbo shares its single's
	// attachments (buy a silencer for the mk23 and the akimbo pair has it too, both ways).
	if (wi->iFireType == TSFT_AKIMBO || wi->iFireType == TSFT_AKIMBO_ONLY)
	{
		if (wi->iSingleType >= 0 && wi->iSingleType < TSW_COUNT)
		{
			m_items[wi->iSingleType].attach |= bits;
			m_items[wi->iSingleType].active |= (bits & 6);
		}
	}
	else if (wi->iAkimboType != -1 && IsOwned(wi->iAkimboType))
	{
		m_items[wi->iAkimboType].attach |= bits;
		m_items[wi->iAkimboType].active |= (bits & 6);
	}

	// @0x80794 tail: the original zeroes m_weaponType and re-runs SelectWeapon + Deploy so the
	// viewmodel body/draw refresh with the new attachment. Our SelectWeapon early-outs on the
	// same type by design, so redeploy the active weapon directly — same effect (draw anim +
	// pev->body recomputed via GetBodyByItems with the new attach mask).
	if (m_pPlayer && m_pPlayer->m_pActiveItem == (CBasePlayerItem *)this)
		Deploy();
	return 1;
}

#ifndef CLIENT_DLL
// @ Zoom__6CTSGuni @0x71710 — enter a scope zoom level. Sets the player's networked view FOV; for a
// high-magnification scope (zoomFov1 < 40) it ALSO hides the viewmodel (pev->viewmodel = 0), matching
// the original (you look through the scope, not at the gun).
void CTSGun::Zoom(int fov)
{
	// BOTH carriers: pev->fov (the server-side cycle state) AND m_iFOV, which the client data
	// carries (cd->fov); the client's HUD_PostRunCmd copies it to g_lastFOV and CHud::Think to
	// gHUD.m_iFOV, which the refdef honors. No SetFOV message: the original sends none for a zoom.
	m_pPlayer->pev->fov = (float)fov;
#ifndef CLIENT_DLL
	m_pPlayer->m_iFOV = fov;
#endif
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	if (wi->zoomFov1 < 40)
		m_pPlayer->pev->viewmodel = 0;
}

// @ Unzoom__6CTSGun @0x71748 — leave the scope: clear the view FOV and, if the viewmodel was hidden,
// restore it to the weapon's v_model.
void CTSGun::Unzoom()
{
	m_pPlayer->pev->fov = 0;
#ifndef CLIENT_DLL
	m_pPlayer->m_iFOV = 0;
#endif
	if (m_pPlayer->pev->viewmodel == 0)
	{
		const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
		if (wi->pszViewModel)
			m_pPlayer->pev->viewmodel = MAKE_STRING(wi->pszViewModel);
	}
}
#endif

#ifdef CLIENT_DLL
void TSHud_GunUpdate(int type, int clip, int fireMode, int equip, int akimboType, int akimboClip,
	int singleType, int singleClip); // cl_dll/ts_ammo.cpp

// client.dll @0x1000acd0, the client's CTSGun::UpdateClientData, which HUD_WeaponsPostThink calls
// with the arsenal out on a first prediction (@0x10003f6c): with a weapon out, the predicted clip,
// fire mode and equipped attachments go to the ammo HUD, and the clip, with the parked clips of its
// akimbo and single variants, to the weapon bar (@0x100581c0). This is how the original client
// keeps them between the server's WeaponInfo messages, which come only on a type or reserve change.
bool CTSGun::UpdateClientData(CBasePlayer *pPlayer)
{
	if (m_pActiveItem)
	{
		const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
		const int ak = wi->iAkimboType;
		const int single = wi->iSingleType;
		TSHud_GunUpdate(m_weaponType, m_iClip, m_fireMode, m_items[m_weaponType].attach,
			ak, (ak >= 0 && ak < TSW_COUNT) ? m_items[ak].clip : 0,
			single, (single >= 0 && single < TSW_COUNT) ? m_items[single].clip : 0);
	}
	return true;
}
#else
// @ UpdateClientData__6CTSGun @0x758c8 — send the TS WeaponInfo HUD message (the ammo
// readout) on change. The original packs the active s_weapon_status slot:
//   BYTE weaponType, BYTE clip (slot+0x4), SHORT reserve (per-caliber), BYTE fireMode
//   (slot+0x2), BYTE flags (slot+0x10).  Our modern arsenal stores the same values
//   directly (m_weaponType / m_iClip / m_ammoReserve[caliber] / m_fireMode). See
//   reference/HUD_MESSAGES.md.
bool CTSGun::UpdateClientData(CBasePlayer *pPlayer)
{

	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	const int caliber = wi->iCaliber;
	const int reserve = (caliber > 0 && caliber < TS_NUM_CALIBERS) ? m_ammoReserve[caliber] : 0;

	// (No ActItems here: the original sends it from CBasePlayer::UpdateClientData, @0x823c7.)

	// @0x758e7: it runs at most every 0.1 s (gun+0x9e0).
	if (m_flNextClientUpdate > gpGlobals->time)
		return false;
	m_flNextClientUpdate = gpGlobals->time + 0.1f;

	// @ the WStatus per-slot weapon-bar loop of CTSGun::UpdateClientData @0x758c8 (mechanism A) —
	// every call, change-detected per slot on the OWNED byte alone (the client keeps the clips
	// itself). It must run BEFORE the WeaponInfo early-return below: gating it behind that return
	// left a PARKED akimbo/single variant's bar cell STALE (the -1/red phantom — cont.89 akimbo
	// dossier §2). One WStatus per slot on change: BYTE type, BYTE owned(0/1), BYTE clip.
	if (!m_bWStatusInit)
	{
		// Reset zeroes the caches (@0x70644): a slot the player does not own sends nothing, so a
		// newcomer gets a WStatus only for what he holds.
		for (int i = 0; i < TSW_COUNT; i++)
			m_iClientWStatus[i] = 0;
		m_bWStatusInit = true;
	}
	for (int t = 0; t < TSW_COUNT; t++)
	{
		const int owned = m_items[t].owned ? 1 : 0;
		// the held weapon's live clip is m_iClip (its item's clip is parked only on a switch).
		// TSW_MELEE (kung-fu / bare hands) has NO clip: send -1 (WEAPON_NOCLIP) always — neither
		// m_iClip (0 while wielded) nor it.clip (0) is -1, so without this the kung-fu bar cell
		// went RED against the original client (it reds any cell with clip < 1). Interop fix.
		int clip = !owned ? 0 : (t == TSW_MELEE) ? -1 : (t == m_weaponType) ? m_iClip : m_items[t].clip;
		// Send the RAW clip byte. MELEE (kung-fu) has clip -1 (WEAPON_NOCLIP); the original writes it raw
		// (-1 -> byte 0xFF = 255), and the client reds a cell only when clip < 1, so 255 renders NORMAL (not
		// red). Clamping <0 to 0 (the old "a bar clip is never negative" assumption) made kung-fu red. Guns
		// are always >=0 so they're unaffected. cont.89 kung-fu-red fix.
		clip &= 0xFF; // -1 -> 255; guns unchanged (0..~64)
		if (owned == m_iClientWStatus[t])
			continue;
		m_iClientWStatus[t] = owned;
		MESSAGE_BEGIN(MSG_ONE, gmsgTSWeaponStatus, NULL, pPlayer->pev);
		WRITE_BYTE(t);     // tsweapon_t
		WRITE_BYTE(owned); // 1 = in the arsenal
		WRITE_BYTE(clip);  // loaded rounds
		MESSAGE_END();
	}

	// Kung fu (no weapon out, @0x75a5c-0x75b19): WeaponInfo goes on a type change alone, carrying
	// the melee bar truncated in the clip byte and zeros.
	if (!m_pActiveItem)
	{
		if (m_weaponType == m_iClientWeaponType)
			return false;
		m_iClientWeaponType = m_weaponType;
		MESSAGE_BEGIN(MSG_ONE, gmsgWeaponInfo, NULL, pPlayer->pev);
		WRITE_BYTE(m_weaponType);
		WRITE_BYTE((int)pPlayer->m_flMeleeBar);
		WRITE_SHORT(0);
		WRITE_BYTE(0);
		WRITE_BYTE(0);
		MESSAGE_END();
		return false;
	}

	// With a weapon out (@0x75b34-0x75c90), WeaponInfo goes when the type (gun+0x97a) or the
	// caliber's reserve (gun+0x9b4) changed since the last one, with the weapon's clip, fire mode
	// and equipped attachments as they are then. The client keeps those itself in between (client.dll
	// @0x1000acd0, CTSGun::UpdateClientData under CLIENT_DLL above).
	if (m_weaponType == m_iClientWeaponType && reserve == m_iClientReserve)
		return true;

	m_iClientWeaponType = m_weaponType;
	m_iClientReserve = reserve;

	MESSAGE_BEGIN(MSG_ONE, gmsgWeaponInfo, NULL, pPlayer->pev);
	WRITE_BYTE(m_weaponType);     // active tsweapon_t
	WRITE_BYTE(m_iClip);          // rounds loaded (a blade: how many he carries)
	WRITE_SHORT(reserve);         // spare rounds for this caliber
	WRITE_BYTE(m_fireMode);       // 0=auto 1=semi 2=burst 3=pump
	WRITE_BYTE(m_items[m_weaponType].attach); // EQUIPPED attachment mask (silencer 1 / laser 2 / flash 4 / scope 8)
	MESSAGE_END();

	// @ mechanism B of @0x758c8 — the PAIRED-VARIANT resend. On the active weapon's type/reserve
	// change (we reach here only then), refresh the bar cell of its akimbo variant
	// (weapon_info+0xc8 = iAkimboType) and single variant (weapon_info+0xcc = iSingleType, when owned
	// && != active) with their LIVE parked clip — so a held akimbo keeps its base single's cell
	// fresh, and a held single keeps the akimbo's. This is the original's drop-in behaviour and the
	// definitive fix for the parked variant's stale -1/red cell. cont.89 akimbo dossier §2.
	{
		const s_weapon_info *awi = get_weapon_info((tsweapon_t)m_weaponType);
		const int pair[2] = { awi->iAkimboType,
			(awi->iSingleType != m_weaponType) ? awi->iSingleType : -1 };
		for (int k = 0; k < 2; k++)
		{
			const int t = pair[k];
			if (t < 0 || t >= TSW_COUNT || !m_items[t].owned)
				continue;
			int c = (t == m_weaponType) ? m_iClip : m_items[t].clip;
			if (c < 0) c = 0;
			MESSAGE_BEGIN(MSG_ONE, gmsgTSWeaponStatus, NULL, pPlayer->pev);
			WRITE_BYTE(t);
			WRITE_BYTE(1);
			WRITE_BYTE(c);
			MESSAGE_END();
		}
	}

	return true;
}
#endif // CLIENT_DLL

// @ ReloadWeapon__6CTSGuniif (@0x74a14, decompile-exact) — ARM a whole-clip reload: gate on a
// wielded item with caliber reserve; take = min(iClipSize - clip, reserve); if any, set the
// item's reloading flag (item+0xc, our m_fInReload) and lock the player's m_flNextAttack for
// flDuration (the weapon's flReloadTime). The REFILL happens in the base ItemPostFrame when that
// lock elapses. iAnim is computed by the caller but DEAD in the original body — the reload
// viewmodel anim rides the events/reload.sc playback client-side.
int CTSGun::ReloadWeapon(int iClipSize, int iAnim, float flDuration)
{
	(void)iAnim;
	if (!m_pActiveItem || !m_pActiveInfo)
		return 0;
	const int caliber = m_pActiveInfo->iCaliber;
	if (caliber <= 0 || caliber >= TS_NUM_CALIBERS || m_ammoReserve[caliber] <= 0)
		return 0;
	const int need = iClipSize - m_iClip;
	const int take = (need < m_ammoReserve[caliber]) ? need : m_ammoReserve[caliber];
	if (take <= 0)
		return 0;
	m_fInReload = true; // item+0xc — the base ItemPostFrame completes the refill
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + flDuration; // player+0x12c
	return 1;
}

// @ StopReload__6CTSGun (@0x71288, decompile-exact) — abort an ARMED whole-clip reload (the
// item+0xc flag): clear it, and for a pump gun also reset the phase machine + the rack toggle;
// idle + the player's attack lock snap to NOW. Callers: ColdCock + MultiplyClips (the pump's
// shell-by-shell machine is phase-driven, never arms item+0xc, and is interrupted by
// ShotgunFire's phase->close instead).
void CTSGun::StopReload()
{
	if (m_pActiveItem && m_pActiveInfo && m_fInReload)
	{
		m_fInReload = false; // item+0xc
		if (m_pActiveInfo->iFireType == TSFT_PUMP)
		{
			m_burstOrPhase = 0;     // item+0x8 — the pump phase machine
			m_fInSpecialReload = 0; // item pumpToggle — back to a FIRE step
		}
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase();
		m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase();
	}
}

// @ EmptySound__6CTSGun (@0x71be4, decompile-exact) — the dry-fire click, via events/empty.sc
// (all params zero; the CLIENT handler owns the click sound — ev swap pending, see the gun-event
// tail task). The contender (weapon 0x24) is silent. Re-arms primary+secondary +0.1, idle +0.5.
void CTSGun::EmptySound()
{
	if (m_weaponType == TSW_CONTENDER)
		return;
	PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usEmpty, 0.0, g_vecZero, g_vecZero,
		0.0, 0.0, 0, 0, 0, 0);
	// (EV_TSEmpty owns the click now — weapons/pistol-empty.wav, universal; the old
	// 357_cock1 server stand-in was the wrong sample anyway.)
	m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.1f;
	m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.1f;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.5f;
}

// @ Reload__6CTSGun (@0x74d5c, decompile-exact) — the +reload dispatch. Bare hands AND the
// blades (fire-types 3/10) route to the kung-fu BLOCK; the grenade (fire-type 2) refuses; a pump
// (fire-type 1) runs the shell-by-shell ShotgunReload machine. Otherwise gate on reserve + not
// mid-burst, pick the reload anim from the ACTIVE STANCE's anim block [1] (the SAWED-OFF with a
// shell still chambered computes block [8] and discards it), arm it via ReloadWeapon
// (duration = the weapon's flReloadTime), and on success: idle +5.0, Unzoom, play
// events/reload.sc (iparam1=type, iparam2=equipped attach, fparam1=0), 3rd-person TSA_RELOAD.
void CTSGun::Reload()
{
	if (!m_pActiveItem)
	{
		CCombatBlock(); // bare hands: RELOAD = the kung-fu block
		return;
	}
	if (m_pActiveInfo->iFireType == TSFT_GRENADE)
		return;
	if (m_pActiveInfo->iFireType == TSFT_KNIFE || m_pActiveInfo->iFireType == TSFT_KATANA)
	{
		CCombatBlock(); // the blades block on RELOAD too
		return;
	}
	if (m_pActiveInfo->iFireType == TSFT_PUMP)
	{
		ShotgunReload();
		return;
	}
	const int caliber = m_pActiveInfo->iCaliber;
	if (caliber <= 0 || caliber >= TS_NUM_CALIBERS || m_ammoReserve[caliber] < 1)
		return;
	if (m_burstOrPhase != 0)
		return; // mid-burst: the queued rounds finish first (@0x84e18)

	// The vestigial anim arg: the original computes the stance block's [1] — or the sawed-off
	// shell-retained [8] when a barrel is still loaded — and passes it to ReloadWeapon, where it is
	// DEAD (ReloadWeapon ignores the anim; @0x74e1c..0x74e7d).
	const int anim = (m_bHoldPosition && m_pActiveInfo->iHoldReloadAnim >= 0)
		? m_pActiveInfo->iHoldReloadAnim : m_pActiveInfo->iReloadAnim;

	if (ReloadWeapon(m_pActiveInfo->iClip, anim, 1.0f * m_pActiveInfo->flReloadTime))
	{
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 5.0f; // dc_5
#ifndef CLIENT_DLL
		Unzoom();
#endif
		// events/reload.sc (@0x74eeb) — EV_TSReload owns the presentation (view-model block[phase],
		// the non-local generic_reload + brass/mag toss on phase 0). The original always sends phase 0,
		// so the sawed-off plays its two-shell reload (block[1]) even with one barrel still loaded; its
		// one-shell reload (block[8], v_sawedoff seq 9) is never played. The original client.dll
		// (FUN_10042690) reads block[8] only for an event phase of 8, and no other client path picks it.
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usReload, 0.0, g_vecZero, g_vecZero,
			0.0, 0.0, m_weaponType, m_pActiveItem->attach, 0, 0);
		m_pPlayer->m_TSAnim.Animate(m_pPlayer, TSA_RELOAD); // 0x33 — 3rd-person relo_<grip>
	}
}

// @ ShotgunReload__6CTSGun (@0x74aac, decompile-exact) — the pump shell-by-shell reload, a
// 3-phase machine in the item's burstOrPhase slot (item+0x8). NEVER arms item+0xc — the driver
// is WeaponIdle (fire-type 1 + phase != 0 -> Reload -> here, every frame), and a fire press
// interrupts via ShotgunFire's phase->2 (close). Each phase plays events/reload.sc with the
// phase discriminator in fparam1: 8.0 open / 9.0 insert / 10.0 close (iparam1=type,
// iparam2=attach).
//   phase 0 (start, from +reload): if a shell fits — Unzoom, restore a scope-hidden viewmodel,
//     phase=1, ALL FOUR locks +0.3 (primary/secondary/idle + the player's m_flNextAttack);
//   phase 1 (insert): on the idle tick — dry/full -> phase 2; else reserve--, clip++, next
//     shell after flReloadTime (0.3 for every pump);
//   phase 2 (close): phase=0, rack toggle reset (next trigger = FIRE step), player lock +0.3,
//     idle +3.0.
// reload1/2/3 are consecutive in every pump v_*.mdl (iReloadAnim = reload1), so the stand-in
// anims below are base/+1/+2 until the reload.sc client handler lands.
void CTSGun::ShotgunReload()
{
	const s_weapon_info *wi = m_pActiveInfo;
	const int caliber = wi->iCaliber;
	const bool haveShell = (caliber > 0 && caliber < TS_NUM_CALIBERS && m_ammoReserve[caliber] > 0);

	if (m_burstOrPhase == 0)
	{
		if (haveShell && m_iClip < wi->iClip)
		{
#ifndef CLIENT_DLL
			Unzoom();
			if (!m_pPlayer->pev->viewmodel && wi->pszViewModel)
				m_pPlayer->pev->viewmodel = MAKE_STRING(wi->pszViewModel); // scope-hidden restore
#endif
			m_burstOrPhase = 1;
			// EV_TSReload plays block[8] (the open) off fparam1=8
			PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usReload, 0.0, g_vecZero,
				g_vecZero, 8.0, 0.0, m_weaponType, m_pActiveItem->attach, 0, 0);
			// NO 3rd-person reload anim for the pump shell-by-shell reload — RE-confirmed the original
			// ShotgunReload @0x74aac plays only the view-model event, never Animate. Shell-for-shell
			// shotguns (Mossberg/Benelli/...) show no world reload; only CLIP shotguns (USAS/Sawed-off,
			// via the standard Reload above) animate the whole-mag swap.
			m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.3f; // dc_0_3
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 0.3f;
			m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.3f;
			m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.3f;
		}
	}
	else if (m_burstOrPhase == 1)
	{
		if (m_flTimeWeaponIdle <= UTIL_WeaponTimeBase())
		{
			if (!haveShell || m_iClip >= wi->iClip)
			{
				m_burstOrPhase = 2;
			}
			else
			{
				m_ammoReserve[caliber]--;
				m_iClip++;
				// EV_TSReload plays block[9] + the insert-shell shuck (everyone) off fparam1=9
				PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usReload, 0.0, g_vecZero,
					g_vecZero, 9.0, 0.0, m_weaponType, m_pActiveItem->attach, 0, 0);
				m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + wi->flReloadTime; // wi+0x14 (0.3)
			}
		}
	}
	else if (m_burstOrPhase == 2)
	{
		m_burstOrPhase = 0;
		m_fInSpecialReload = 0; // item pumpToggle — next trigger is a FIRE step
		// EV_TSReload plays block[10] + the weapon's close/rack sound (non-local) off fparam1=10
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usReload, 0.0, g_vecZero,
			g_vecZero, 10.0, 0.0, m_weaponType, m_pActiveItem->attach, 0, 0);
		m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.3f;
		m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f; // dc_3
	}
}

// @0x85e54 CCombatIdle — the bare-hands (kung-fu) viewmodel idle: on the idle timer, re-arm
// idle +3.0 and play the TSW_MELEE row's idle sequence from the table (+0x252 — 0 in the
// shipped data; kept as a table read like the original). Sole caller: WeaponIdle's leading
// no-active-item branch. @0x75e92-0x75eac pass skiplocal 0, as HolsterWeapon does, so the anim
// reaches a client predicting its weapons (cl_lw 1) too.
void CTSGun::CCombatIdle()
{
	if (m_flTimeWeaponIdle > UTIL_WeaponTimeBase())
		return;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
	const s_weapon_info *wi = get_weapon_info(TSW_MELEE);
	m_ForceSendAnimations = true;
	SendWeaponAnim(wi ? wi->iIdleAnim : 0, pev->body);
	m_ForceSendAnimations = false;
}

// @ WeaponIdle__6CTSGun (@0x754dc, decompile-exact) — runs EVERY frame (the base ItemPostFrame
// tail), not just on the idle timer. Bare hands -> CCombatIdle. Then, for a wielded item:
//  - the PUMP-RELOAD continuation: fire-type 1 with the phase machine armed -> Reload() (which
//    routes back into ShotgunReload) — this is the machine's per-frame driver;
//  - the BURST machine: fire-mode 2 with the primary timer clear — fire the next queued round
//    and decrement (this is how a tapped burst completes after release);
//  - on the idle timer: the grenade cook check (GrenadeIdle(0)), AUTO-RELOAD a dry clip (unless
//    InfiniteClips), or the idle anim — the stance block's [0] — with idle re-armed +3.0.
// (The original also mirrors item->clip into the client-clip cache and re-points item+0x28 at
// the stance's anim block here; our stance selection resolves at each use site instead.)
void CTSGun::WeaponIdle()
{
	if (!m_pActiveItem)
	{
		CCombatIdle();
		return;
	}
	if (m_pActiveInfo->iFireType == TSFT_PUMP && m_burstOrPhase != 0)
	{
		Reload(); // the shell-by-shell continuation (-> ShotgunReload)
		return;
	}
	if (m_fireMode == TSFM_BURST && m_flNextPrimaryAttack <= UTIL_WeaponTimeBase())
	{
		if (m_burstOrPhase < 1)
			m_burstOrPhase = 0;
		else
		{
			Fire();
			m_burstOrPhase--;
		}
	}
	if (m_flTimeWeaponIdle <= UTIL_WeaponTimeBase())
	{
		if (m_pActiveInfo->iFireType == TSFT_GRENADE)
		{
			GrenadeIdle(0); // the cook machine (the pin set idle +0.7, so this starts then)
		}
		else if (!InfiniteClips(m_pPlayer) && m_iClip < 1)
		{
			Reload(); // AUTO-RELOAD the dry clip
		}
		else if (m_iClip != 0)
		{
			m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 3.0f;
			const int idleAnim = (m_bHoldPosition && m_pActiveInfo->iIdleAnimHold >= 0)
				? m_pActiveInfo->iIdleAnimHold : m_pActiveInfo->iIdleAnim; // stance block [0]
			// @0x75646: skiplocal 0, so a predicting client gets the idle from the server too
			m_ForceSendAnimations = true;
			SendWeaponAnim(idleAnim, pev->body); // pev->body carries the attachment/bloody submodel (fold-plan Y4)
			m_ForceSendAnimations = false;
		}
	}
}

// @ SwitchFireMode__6CTSGun (@0x8169c) — cycle the held weapon's fire-mode to the next one
// offered in its iFireModes mask (modes 0..5, wrapping), starting just past the current.
void CTSGun::SwitchFireMode()
{
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	for (int i = 1; i <= 6; i++)
	{
		int mode = (m_fireMode + i) % 6;
		if (wi->iFireModes & (1 << mode))
		{
			m_fireMode = mode;
			m_items[m_weaponType].mode = mode;
			m_burstOrPhase = 0;
			return;
		}
	}
}

// Give a player a TS weapon of `type`. If they already hold the weapon_tsgun (the
// arsenal), add the type to it; otherwise create the weapon_tsgun and pick it up.
CBaseEntity *GiveTSWeapon(CBasePlayer *pPlayer, tsweapon_t type)
{
	CTSGun *pHeld = GetPlayerTSGun(pPlayer);
	if (!pHeld)
	{
		// A new gun is spawned empty (CTSGun::Spawn seeds nothing, as the original), picked up,
		// then given the weapon like a held one.
		edict_t *pent = CREATE_NAMED_ENTITY(MAKE_STRING("weapon_tsgun"));
		if (FNullEnt(pent))
			return NULL;
		pent->v.origin = pPlayer->pev->origin;
		pent->v.spawnflags |= SF_NORESPAWN;
		DispatchSpawn(pent);
		DispatchTouch(pent, pPlayer->edict());
		pHeld = GetPlayerTSGun(pPlayer);
		if (!pHeld)
			return NULL;
	}

	if (!pHeld->TakeWeapon(type))
		return NULL; // already owned (or invalid) — caller leaves it on the floor
	return pHeld;
}

// Force the player's TS arsenal to be the active item + deploy its current weapon. Called at the
// end of PlayerSpawn: the tsgun is the player's single weapon entity, but the engine's auto-deploy
// (GetNextBestWeapon) skips kung-fu (melee, no clip), so a bare/kung-fu-only loadout would leave
// m_pActiveItem NULL → no viewmodel + ItemPostFrame (firing) never runs. This guarantees it deploys.
void TSDeployArsenal(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (!pGun)
		return;
	pPlayer->m_pActiveItem = (CBasePlayerItem *)pGun;
	// Spawn you on your HIGHEST-PRIORITY owned weapon (g_iWeaponPriority), NOT whatever BuyArmament
	// deployed last (the last-queued buy). The original spawns on the best weapon — e.g. M4 (priority
	// index 3) over a sawed-off (10) or akimbo MK23 (19). GetBetterWeapon walks the priority table and
	// SelectWeapon-deploys; kung-fu is priority-last, so a bare/kung-fu-only loadout still deploys (and
	// forces the viewmodel). The original server has no such step: each bought weapon is selected and
	// deployed in turn (PickUpWeapon @0x8314c), so the last one bought is held; Steven verified the
	// original spawns on the best one, so the order the client sends its buys likely does that. Kept.
	pGun->GetBetterWeapon(-1);
}

// @ the ClientCommand `weapon_<N>` commit (@0xae433): wield the owned arsenal slot holding
// tsweapon_t `type` (holster -> set active -> deploy via SelectSlot). This is the faithful
// weapon-select transport: the client's invnext/invprev select cursor (FUN_1004f500) emits
// "weapon_%i" with the numeric type; the server selects that type if owned.
void TSSelectWeaponType(CBasePlayer *pPlayer, int type)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (!pGun || type < 0 || type >= TSW_COUNT)
		return;
	if (type == TSW_MELEE || pGun->IsOwned(type))
		pGun->SelectWeapon(type);
}

// @ the ClientCommand `lastinv` block (disasm-verified @0x9e3bc-0x9e454, fold-plan Y1): toggle
// back to the previously-wielded type (player+0xa1c). The ONLY gate is "holding a real weapon"
// (gun+0xc8 != 0 — no arsenal count, no owned pre-check: SelectWeapon @0x70fb8 gates owned
// itself). The original records the outgoing type in the COMMAND on the PLAYER (+0xa1c); ours
// records it in SelectWeapon on the gun (m_lastType) — same value, member re-home deferred to
// the members pass (Y1(c), ledger).
void TSLastInv(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (!pGun || pGun->m_weaponType == TSW_MELEE) // @0x9e407: must be ARMED (m_weaponType != 0)
		return;
	pGun->SelectWeapon(pGun->m_lastType);
}

// Infinite-clip powerup hook (PlayerAddPowerUp) — multiply the held weapon's reserve.
bool TSMultiplyClips(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	return pGun ? pGun->MultiplyClips() : false;
}

// Cycle the held weapon's fire-mode (the `firemode` command — default.cfg binds f).
void TSSwitchFireMode(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (pGun)
		pGun->SwitchFireMode();
}

// @ SwitchItems__6CTSGun = FUN_00081564 (the `useitems` command — default.cfg binds g): toggle the
// ACTIVE gun attachments (laser bit 2 / flashlight bit 4) of the held weapon. The active mask
// (TSItem::active, item+0x14) is a subset of the EQUIPPED mask (item+0x10); useitems cycles it.
// Faithful RE state machine (the laser/flashlight only RENDER while active, and AddItem starts them
// all-on). See [[useitems-is-attachment-toggle]].
//   both laser+flash equipped: active cycles  6(both) -> 0(off) -> 2(laser) -> 4(flash) -> 6 ...
//   only one equipped:         that bit toggles on/off
void CTSGun::SwitchItems()
{
	if (m_weaponType < 0 || m_weaponType >= TSW_COUNT)
		return;
	TSItem &it = m_items[m_weaponType];
	const int equipped = it.attach;

	if ((equipped & 6) == 6) // both laser + flashlight equipped
	{
		if ((it.active & 6) == 6)        // both on -> off
			it.active &= ~6;
		else if (it.active & 2)          // laser -> flashlight
			it.active = (it.active & ~2) | 4;
		else if (it.active & 4)          // flashlight -> both
			it.active |= 2;
		else                             // off -> laser
			it.active |= 2;
	}
	else if (equipped & 2)               // only laser equipped -> toggle laser
		it.active ^= 2;
	else if (equipped & 4)               // only flashlight equipped -> toggle flashlight
		it.active ^= 4;
	else
	{
		it.active &= ~6; // nothing toggleable equipped — clear, no sound
		return;
	}

	if (m_pPlayer)
		EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/switch.wav", VOL_NORM, ATTN_NORM);
}

// @ GetActiveItems__6CTSGun (@0x71228): the active attachment mask of the held weapon (item+0x14),
// with bit 0x40 OR'd in for an akimbo/akimbo-only weapon (the original tags dual-wield here).
int CTSGun::GetActiveItems() const
{
	if (m_weaponType < 0 || m_weaponType >= TSW_COUNT)
		return 0;
	int active = m_items[m_weaponType].active;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)m_weaponType);
	if (wi->iFireType == TSFT_AKIMBO || wi->iFireType == TSFT_AKIMBO_ONLY)
		active |= 0x40;
	return active;
}

// Toggle the held weapon's active attachments (the `useitems` command -> CTSGun::SwitchItems).
void TSSwitchItems(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (pGun)
		pGun->SwitchItems();
}

// @ DropWeapon__6CTSGun (@0x8094c) — remove an owned item from the arsenal. If it was the
// active weapon, auto-select the next-best owned weapon (fThrow → GetBetterWeapon) or just
// holster it. Does NOT spawn the world gun (CBasePlayer::DropWeapon does the throws); this is
// only the items[] bookkeeping side.
void CTSGun::DropWeapon(int type, bool fThrow)
{
	if (type < 1 || type >= TSW_COUNT || !m_items[type].owned)
		return;

	m_items[type] = TSItem(); // clear the item (owned=false, clips/mode/masks zeroed)

	if (type == m_weaponType)
	{
		if (fThrow)
			GetBetterWeapon(type);   // auto-select the next-best owned weapon (holster->deploy)
		else if (m_pPlayer)
			SyncActiveToItem();      // holster only (the active item is already cleared)
	}
}

#ifndef CLIENT_DLL // the drop-into-world helpers spawn server-only CTSWorldGun/CTSAmmoPack entities
// @0x8529c CBasePlayer::ThrowWeapon — the throw primitive (the sole caller is DropWeapon). Build the
// throw angles (alive = view + offset; dead = random), create the dropped gun via the 3-arg factory,
// then position it (eye + 16*forward + lateral), give it a random tumble, and toss it.
CTSWorldGun *CBasePlayer::ThrowWeapon(int type, const TSItem &status, const Vector *angleOffset,
	const Vector *velOffset)
{
	Vector ang;
	if (IsAlive())
	{
		ang = pev->v_angle;
		if (angleOffset)
			ang = ang + *angleOffset;
	}
	else
	{
		ang = Vector(RANDOM_FLOAT(-60, 60), RANDOM_FLOAT(-180, 180), 0);
	}
	UTIL_MakeVectors(ang);

	CTSWorldGun *pGun = CTSWorldGun::CreateWeapon(this, type, status);
	if (!pGun)
		return NULL;

	// origin = eye + 16*forward (+ velOffset: forward on x, lateral on right via y, vertical via z).
	Vector off(16, 0, 0);
	if (velOffset)
		off = Vector(16 + velOffset->x, velOffset->y, velOffset->z);
	Vector origin = (pev->origin + pev->view_ofs) + gpGlobals->v_forward * off.x + gpGlobals->v_right * off.y;
	origin.z += off.z;
	UTIL_SetOrigin(pGun->pev, origin);

	pGun->pev->angles = g_vecZero;
	pGun->pev->avelocity.y = RANDOM_FLOAT(-300, 300); // random tumble/spin (@0x8529c)
	pGun->pev->avelocity.z = RANDOM_FLOAT(-300, 300);

	// Throw speed S (RE'd from the @0x85359 / @0x853c1 x87 asm + the .rodata 0.9/0.33/90 constants):
	//   alive: S = (90 - look-pitch) * 2  (look level -> 180; look up -> harder; look down -> drops)
	//   dead : S = random 60..160
	// velocity = 0.9*playerVelocity (inherited momentum) + S*forward (toss) + 0.33*S upward (lob).
	float S = IsAlive() ? (90.0f - pev->v_angle.x) * 2.0f : RANDOM_FLOAT(60, 160);
	pGun->pev->velocity = pev->velocity * 0.9f + gpGlobals->v_forward * S;
	pGun->pev->velocity.z += S * 0.33f;
	return pGun;
}

// Thin glue for DropWeapon's branches: build the dropped item's status (clip override), then route
// through the real CBasePlayer::ThrowWeapon (velOffset.y carries the akimbo's ±lateral split).
static CTSWorldGun *TSThrowWorldGun(CBasePlayer *pPlayer, CTSGun *pGun, int type, int clip, float side)
{
	TSItem status = pGun->m_items[type]; // carry the dropped weapon's full state (mode/attach/barrels)
	status.owned = true;
	status.clip = clip;
	Vector lateral(0, side, 0);
	return pPlayer->ThrowWeapon(type, status, NULL, &lateral);
}

// A drop-key or disarm drop (DropWeapon type < 0) marks each gun it throws: the dropper, for whom
// AddToFullPack draws it with a glow shell, and its lifetime, twice weaponstay clamped 10..120 s
// (@0x84e76, @0x84f4a, @0x85200: time + 2 * weaponstay_time, >= 120 -> 120, <= 10 -> 10) in place of
// the weaponstay clamped 5..60 s CTSWorldGun::Spawn gave it. A drop_<N> or death drop keeps Spawn's.
static void TSMarkDropKeyGun(CTSWorldGun *pGun, CBasePlayer *pPlayer)
{
	float stay = 2.0f * CVAR_GET_FLOAT("weaponstay");
	if (stay >= 120.0f)
		stay = 120.0f;
	else if (stay <= 10.0f)
		stay = 10.0f;
	pGun->m_flExpire = gpGlobals->time + stay;
	pGun->m_pDropper = pPlayer->edict();
}

// @0x84900 CBasePlayer::DropWeapon(tsweapon_t type, float damage) — the ONE drop entry point (RE'd
// merge of our previously-split DropWeapon()/DropWeapon(int,float); dossier cont87/weapon_drop.md):
//   type <  0  -> the ACTIVE weapon   (-1 = the "drop" key; -2 = disarm, gated by melee pressure)
//   type >= 0  -> that SPECIFIC owned type (the weapon-select "drop_<N>" right-click)
// Akimbo branches (reference/WEAPON_SELECT.md): a combinable AKIMBO throws TWO base singles (clip
// split, ±4) and removes both items; a SINGLE whose akimbo we also own throws one + keeps the other
// barrel; else throw one and remove. The CTF flag drops ONLY in the out-of-range (bare-handed) branch
// — NOT unconditionally on every weapon drop (@0x84a87: a type outside 1..37, the flag carried).
bool CBasePlayer::DropWeapon(int type, float flDamage)
{
	CBasePlayer *pPlayer = this;
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (!pGun)
		return false;

	// disarm (-2) can't take the flag or bare hands
	if ((pGun->m_weaponType == TSW_FLAG || pGun->m_weaponType < 1) && type == -2)
		return false;

	bool fThrow = true; // true => reselect after the arsenal clear; false (disarm) => holster only

	// disarm (-2): accumulate melee pressure; only knock the weapon out past the threshold
	if (type == -2)
	{
		m_flDisarmPressure += flDamage;
		int thr = 30; // katana / default
		if (pGun->m_weaponType != TSW_KATANA)
		{
			thr = 25; // the melee category (GetWeaponType == 3)
			if (GetWeaponType((tsweapon_t)pGun->m_weaponType) != 3)
			{
				const s_weapon_info *awi = get_weapon_info((tsweapon_t)pGun->m_weaponType);
				int sc = awi ? awi->slotCost : 20;
				thr = (sc < 21) ? 20 : (sc < 50 ? sc : 50); // clamp(slotCost, 20, 50)
			}
		}
		if (m_flDisarmPressure < (float)thr)
			return false;
		fThrow = false;
		m_flDisarmPressure = 0.0f;
	}

	// resolve the target: negative -> the active weapon; non-negative -> that specific type
	const int a0 = (type < 0) ? pGun->m_weaponType : type;

	// out of weapon range (bare hands / the flag slot) -> a CTF flag drop (drop key only, not disarm)
	if (a0 < 1 || a0 >= TSW_COUNT)
	{
		if (type == -2)
			return false;
		if (!m_flagStatus.active) // HaveFlag()
			return false;
		DropFlag(false);
		return true;
	}

	if (!pGun->m_items[a0].owned)
		return false;
	const s_weapon_info *wi = get_weapon_info((tsweapon_t)a0);
	if (!wi->pszWorldModel) // kung-fu / katana &c. have no droppable world model
		return false;

	// park the active weapon's live clip so m_items[a0].clip is current for ANY a0 (active or not)
	pGun->SyncActiveToItem();
	const bool wasActive = (a0 == pGun->m_weaponType);
	CTSWorldGun *pThrown = NULL; // the last gun thrown (the original's edi)

	// The M61 (@0x84aaf). A cooking grenade (item phase 1, only the held one) is thrown as it is.
	if (a0 == TSW_M61)
	{
		const bool cooking = wasActive && pGun->m_burstOrPhase == 1;
		// A player who died in deathmatch or teamplay leaves each grenade as a grenade powerup,
		// 5 units apart along x, and keeps the item (@0x84ac1-0x84b82).
		if ((unsigned char)TSGameRules->m_iGameMode <= 1 && !IsAlive())
		{
			if (cooking)
				pGun->GrenadeIdle(1);
			for (int i = 0; i < pGun->m_items[a0].clip; i++)
			{
				CTSPowerUp *pPowerup = CTSPowerUp::CreatePowerUp(TS_PWUP_GRENADE, -1);
				if (pPowerup)
				{
					pPowerup->pev->origin = pev->origin;
					pPowerup->pev->origin.x += (float)(i * 5);
				}
			}
			return true;
		}
		// Otherwise one grenade at a time (@0x84b88-0x84c84).
		if (cooking)
		{
			pGun->GrenadeIdle(1);
		}
		else if (pGun->m_items[a0].clip > 0)
		{
			pThrown = TSThrowWorldGun(pPlayer, pGun, a0, 1, 0.0f);
			if (pGun->m_items[a0].clip <= 1)
				pGun->DropWeapon(a0, fThrow);
			else
			{
				pGun->m_items[a0].clip--;
				if (wasActive)
					pGun->m_iClip = pGun->m_items[a0].clip;
				if (!fThrow)
					pGun->HolsterWeapon();
			}
			EMIT_SOUND(ENT(pPlayer->pev), CHAN_ITEM, "weapons/weapondrop.wav", VOL_NORM, ATTN_NORM);
		}
		if (type < 0 && pThrown)
			TSMarkDropKeyGun(pThrown, this);
		SetPlayerSpeed(GetSpeedBySlots(pPlayer));
		return true;
	}

	// 1) combinable akimbo -> throw two base singles, remove both items.
	if (wi->iFireType == TSFT_AKIMBO && wi->iSingleType >= 1 && wi->iSingleType < TSW_COUNT)
	{
		const int single = wi->iSingleType;
		const int total = pGun->m_items[a0].clip;
		const int half = total / 2;
		// a drop-key/disarm drop marks each half as it is thrown (@0x84e76..0x84ed9, @0x84f4a..0x84fad)
		pThrown = TSThrowWorldGun(pPlayer, pGun, single, half, 4.0f);
		if (type < 0 && pThrown)
			TSMarkDropKeyGun(pThrown, this);
		pThrown = TSThrowWorldGun(pPlayer, pGun, single, total - half, -4.0f);
		if (type < 0 && pThrown)
			TSMarkDropKeyGun(pThrown, this);
		pGun->DropWeapon(a0, fThrow);     // remove the akimbo (auto-selects next if it was active)
		pGun->DropWeapon(single, fThrow); // and the base single, if it was also owned
	}
	// 2) single whose akimbo we ALSO own -> throw one, keep the other (clip = akimbo right barrel).
	else if (wi->iAkimboType >= 1 && wi->iAkimboType < TSW_COUNT && pGun->m_items[wi->iAkimboType].owned)
	{
		const int akimbo = wi->iAkimboType;
		const int keptClip = pGun->m_items[akimbo].rightClip;
		pThrown = TSThrowWorldGun(pPlayer, pGun, a0, pGun->m_items[a0].clip, 0.0f);
		pGun->m_items[a0].clip = keptClip;
		pGun->DropWeapon(akimbo, false); // remove the akimbo item (a0 stays owned)
		if (wasActive)
			pGun->m_iClip = keptClip; // refresh the live clip if a0 is the held weapon
	}
	// 3) stackable throwable (knife/sealknife/m61): .clip is a UNIT count.
	else if (wi->iStackable)
	{
		const int throwN = (type == -2) ? 1 : pGun->m_items[a0].clip; // disarm throws one unit
		pThrown = TSThrowWorldGun(pPlayer, pGun, a0, throwN, 0.0f);
		if (throwN < pGun->m_items[a0].clip)
		{
			pGun->m_items[a0].clip -= throwN; // units remain
			if (wasActive)
				pGun->m_iClip = pGun->m_items[a0].clip;
			if (!fThrow)
				pGun->HolsterWeapon(); // @0x84d74: a disarm puts the rest away
		}
		else
			pGun->DropWeapon(a0, fThrow);
	}
	// 4) plain weapon -> throw one, remove it.
	else
	{
		pThrown = TSThrowWorldGun(pPlayer, pGun, a0, pGun->m_items[a0].clip, 0.0f);
		pGun->DropWeapon(a0, fThrow);
	}

	// A drop-key or disarm drop (type < 0) marks the last gun thrown, the flag excepted
	// (@0x851ed..0x85265): its dropper alone sees it glow, and it lies twice weaponstay. A drop_<N>
	// (type >= 0) or a death drop glows for nobody and lies weaponstay.
	if (type < 0 && a0 != TSW_FLAG && pThrown)
		TSMarkDropKeyGun(pThrown, this);

	EMIT_SOUND(ENT(pPlayer->pev), CHAN_ITEM, "weapons/weapondrop.wav", VOL_NORM, ATTN_NORM);
	SetPlayerSpeed(GetSpeedBySlots(pPlayer));
	return true;
}

// On death, scatter the player's whole arsenal into the world as CTSWorldGuns (the dead
// player's arsenal is discarded on respawn, so no removal bookkeeping is needed).
void TSDropWeaponsOnDeath(CBasePlayer *pPlayer)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	if (!pGun)
		return;
	// RE CBasePlayer::Killed @0x7ed5c: the death scatter loops EVERY weapon type calling
	// CBasePlayer::DropWeapon(type, 0), plus a SECOND DropWeapon(type, 0) when that type has an akimbo
	// form (get_weapon_info(type).iAkimboType != -1). DropWeapon owns the akimbo -> TWO-base-singles
	// throw + the arsenal removal/single-pair bookkeeping internally, and no-ops on an unowned or
	// already-removed type — so a wielded AKIMBO correctly scatters TWO world guns. The old
	// single-TSThrowWorldGun-per-owned-type dropped only ONE gun for an akimbo and bypassed the
	// single/akimbo pair logic (the "something looks off" in the death drop). cont.91.
	for (int type = 1; type < TSW_COUNT; type++)
	{
		pPlayer->DropWeapon(type, 0.0f);
		const s_weapon_info *wi = get_weapon_info((tsweapon_t)type);
		if (wi && wi->iAkimboType != -1)
			pPlayer->DropWeapon(type, 0.0f);
	}
}

#endif // !CLIENT_DLL (drop-into-world helpers)

// Pick up a dropped weapon of `type` carrying `clip` loaded rounds (CTSWorldGun pickup):
// add it to the arsenal (combining if it triggers an akimbo upgrade), then restore the
// carried clip — unless the pickup combined into the akimbo (which sets its own barrels).
// Returns false when the pickup didn't land (won't fit / already-owned duplicate) so the
// caller can leave the world item on the floor (the original PickUpWeapon failure contract).
// Add per-caliber reserve to the player's TS gun (CTSGroundAmmo refill station).
bool TSAddAmmo(CBasePlayer *pPlayer, int caliber, int count)
{
	CTSGun *pGun = GetPlayerTSGun(pPlayer);
	return pGun ? pGun->AddAmmo(caliber, count) : false;
}
