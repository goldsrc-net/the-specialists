# mod-ts

**Mission: produce a functional, source-faithful reconstruction of the original
"ts" (The Specialists) Half-Life mod — both the server and client game libraries
— as clean modern C++ on the HL SDK, that compiles, loads in the engine, and
behaves identically to the original.** The project's public name is **reTS**.

We recover the SOURCE that produced the shipped binaries (not a port). The bar
is behavioural fidelity: same entities (`LINK_ENTITY_TO_CLASS`), keyvalues, game
mechanics, save fields — verified by **build + run + behaviour** — plus the
internal structure that server plugins see (below).

**The swap contract is the EXTERNAL INTERFACE.** Our server and our client must each
be a **drop-in swap** for the original — run our client against the original server
(or vice-versa) and behaviour is identical. That makes the *interface* the contract
that must match the original exactly:
- the engine-facing **C interface** (`GetEntityAPI2`/`DLL_FUNCTIONS`/
  `LINK_ENTITY_TO_CLASS` entity factories, `HUD_*`/`ClientFactory` on the client);
- the **client↔server protocol**: user-message **names + wire formats** (e.g.
  `RoundTime`, `TSHealth`, …), **event** names + param packing (`events/fire.sc`),
  **console command** names, and **cvar** names. These are the swap contract — get
  them byte-faithful (this is why the inherited `TSRoundTime` had to become the
  original's `RoundTime`).

**The internal structure is now a target too (changed 2026-10-05).** AMX Mod X plugins
reach into the game DLL (Ham Sandwich hooks by vtable slot, `get/set_pdata` and TSX by
member offset), so the original's internal shape matters for plugin compatibility:
- **Class set: remove only what TS removed.** Keep every SDK class/entity/function the
  original `ts_i386.so` still contains, even if TS never uses it (stock Valve maps load
  under TS through `fallback_dir valve` and place `item_battery`, `xen_*`, …). Remove only
  what the original lacks. `nm -D reference/ts_i386.so` is the test for entities.
- **Base classes** match the original (`reference/coverage/hierarchy.tsv`, read from the
  binary's typeinfo functions).
- **Vtables**: same virtuals, same order, same overrides as the original, EXCEPT the
  halflife-updated shapes we keep on purpose: the virtual destructor (`delete` frees
  entities), `MyTogglePointer`, the HL25 sentence virtuals on `CBaseToggle`, the
  client-prediction virtuals on `CBasePlayerItem`, and `DoRetireWeapon`. Those are the
  same in every halflife-updated mod, which is the planned basis for an AMX Mod X gamedata
  layer for halflife-updated-derived mods.
- **Member offsets**: move them toward the original where feasible. This can only match
  on i386 (64-bit pointers change every later offset), and gcc-2.x put the vptr at +0x60
  where any modern compiler puts it at 0, so exact layout is not reachable; recovered
  offsets in comments stay as provenance and as the target to approach.

We build on the Itanium-ABI `halflife-updated` SDK (vptr @0, `bool` `KeyValue`/`Save`, …),
which is intentionally newer than the original's 2007 SDK. Do not chase gcc-2.x quirks
that cannot be expressed in modern C++.

This repo is part of the **goldsrc.net** effort (see `../CLAUDE.md` in the
parent for the wider browser-playable GoldSrc architecture). It is **private**;
the buildchain and the other goldsrc-net forks are public.

---

## 1. What this repo is

A fork of **`goldsrc-net/halflife-updated`** (branch `64bit-demo`), which is the
modern open HL SDK we reconstruct *onto*. Remotes:

- `origin`   → `git@github.com:goldsrc-net/mod-ts.git` (private)
- `upstream` → `git@github.com:goldsrc-net/halflife-updated.git` (pull SDK fixes
  via `git merge upstream/64bit-demo`). The fork point is `upstream/64bit-demo` @ `8282fa7`.

Work happens on `main` (all earlier branches were merged into it on 2026-10-05; local
`backup/*` tags keep the pre-merge states).

The mod ships as a **standalone game directory** `ts/` (`-game ts`,
`fallback_dir "valve"`). The manifest lives at `game/ts/liblist.gam`; only files
the mod overrides live under `ts/`, everything else falls back to `valve/`.

---

## 2. Reconstruction strategy

The originals + extracted data live in **`reference/`** (see its README). Three
complementary symbol sources, all from one shared codebase (client + server are
built from the same tree, as all HL SDK projects are):

| Artifact | Toolchain | Gives us |
|---|---|---|
| `reference/ts_i386.so` | Linux GCC 2.96, **not stripped** | Complete **server** function inventory (2,426 funcs) + addresses → Ghidra auto-names them. No DWARF/STABS for mod code. |
| `reference/mp.dll` | Windows MSVC (server) | **159** exports → fully-typed signatures (return, params, `__thiscall`, const) via `llvm-undname`. |
| `reference/client.dll` | Windows MSVC (client) | **~1,013** exports incl. HL client iface (`HUD_*`, `ClientFactory`) + VGUI. The client-side ABI spine. |
| `reference/ts_fgd.fgd` | Hammer/Worldcraft | 177 entity defs → keyvalues (member vars), spawnflags (flag bits), I/O for the `CTS*` entity classes. |

`reference/dump-signatures.sh` regenerates the demangled signatures + class
inventory.

### Scope (measured, not estimated)

Diffing the binary's classes against the classes defined in this repo's SDK
source (`comm`):

- **437** stock HL SDK classes (already have source — reuse, do not RE).
- **216** classes present in the `.so` binary.
- **60** classes are **TS-specific** = the reverse-engineering work-list
  (`reference/RECONSTRUCTION.md`). ~**386** functions across them.
- **23** of the 60 are FGD entity-backed (the FGD has 24 `ts_*` entities) →
  keyvalues/spawnflags recoverable directly from the FGD, which anchors the RE
  of those classes.

Biggest TS-specific classes by recovered function count: `CTSGun` (44),
`CTSGameRules` (41), `CTSStunt` (25), `CTSAnimation` (14), `CTSFlag` (13), …

### Per-class workflow

1. Confirm the class is TS-specific (in `reference/RECONSTRUCTION.md`, not in SDK
   source). If it exists in the SDK, reuse/extend that source instead.
2. Decompile its functions: `scripts/decompile.sh reference/ts_i386.so recovered/server <ClassName>`
   → `recovered/server/<ClassName>.c`, each function annotated with its recovered
   prototype (symbols auto-applied; `recovered/` is gitignored).
3. Read the prototype from `reference/signatures-gcc2.txt` (server, all funcs) or
   `signatures-mp.txt`/`signatures-client.txt` (MSVC, fully-typed) for exact
   return/param types.
4. For entity classes, cross-check members against the FGD keyvalues + spawnflags
   in `reference/ts_fgd.fgd`.
5. Write the class into `dlls/` (server) / `cl_dll/` (client) in **modern-SDK
   style** — natural member declarations, `bool`-returning `KeyValue`/`Save`,
   `SetThink`/`SetTouch`, `pev->`. Take the base class from
   `reference/coverage/hierarchy.tsv` and declare virtuals in the original's slot order
   (`reference/coverage/VTABLES.md`). Recovered offsets are provenance comments and the
   layout to approach. Mark uncertain spots with `VERIFY`.
6. Add the file(s) to `dlls/CMakeLists.txt` (explicit source list), then
   **build + verify**: `scripts/build.sh amd64`, then
   `nm -D build-amd64/dlls/hl.so | grep <entity_name>` shows the `T <entity>`
   C factory (engine-spawnable) and `nm … | c++filt | grep <Class>::` shows the
   linked methods. Runtime/behaviour test (spawn the entity in the engine) is the
   final check once enough of the mod is assembled.

---

## 3. Building

mod-ts is **not** a buildchain submodule (clean separation — the buildchain is
provenance-only for the public server stack). It consumes the buildchain's
Docker image so binaries keep the glibc-2.28 floor (run on glibc ≥ 2.28).

```bash
# one-time: build the toolchain image from the buildchain
( cd ../hlds64-buildchain && make image )      # → hlds64-buildchain:debian10

# build hl.so for an arch (default amd64)
scripts/build.sh amd64        # → build-amd64/dlls/hl.so
scripts/build.sh aarch64
scripts/build.sh i386
BUILDTYPE=Debug scripts/build.sh i386   # -O0 + DWARF; use for vtable/layout comparison
```

**Win32 (`hl.dll`, installed as `dlls/mp.dll`, and `client.dll`)**: CI builds them with MSVC on
`windows-latest` (Actions is on for this repo since 2026-10-10). Locally, `scripts/build-win.sh` cross-builds
them with clang-cl + lld-link (MSVC ABI, so Valve's win32 `vgui.lib` links) against an xwin splat of the
x86 MSVC CRT and Windows SDK (`~/.cache/xwin/splat-x86`; the script prints the xwin command if it is
missing). Output: `build-win32-clangcl/`. lld-link rejects `hl.def`'s `SECTIONS` block, so that build
uses `dlls/hl-lld.def`.

**Packages, versions, releases.** `scripts/package.sh` builds the patch for a stock TS 3.0 install
(`dist/reTS-<version>.zip`, laid out by `scripts/assemble-dropin.sh`: TS's own file names, Linux libraries
stripped, README and MANIFEST.tsv); `--debug` packages the Debug profile instead (not stripped, with PDBs).
The version is the pushed `v*` tag (`cmake/Version.cmake`: `$RETS_VERSION`, else `git describe`, else the
commit); Windows builds carry it in a version resource. Pushing a tag (`git tag v0.0.1 && git push origin
v0.0.1`) runs `.github/workflows/release.yml`, which builds through `ci-cd.yml` and creates the GitHub
release with `reTS-<tag>.zip`. CI builds Debug only when run by hand with `debug` ticked.

Always build deployables through the image, never natively (a native build on
this host links glibc 2.38+ and won't run on Debian 12 / AL2023). Verify:
`objdump -T build-<arch>/dlls/hl.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1`.

**CMake is the only build system** (`dlls/CMakeLists.txt`, `cl_dll/CMakeLists.txt`). The legacy
`linux/Makefile*`, the `projects/vs2019/*` MSVC projects and `scripts/check-source-lists.py` were
removed in `94c0bba`. The submodules (`third_party/openvgui` with its `miniutl`,
`third_party/vgui_support` with its `vgui-dev`) must be cloned recursively. Both are public:
vgui_support is goldsrc-net's public fork of FWGS's, branch `64bit` (FWGS's code plus the quit fix;
2026-11-12). The wasm work it used to share a repo with is private in `goldsrc-net/vgui_support-web`;
the wasm client build doesn't compile the shim. CI's checkouts need no token.

The **client** builds too: `scripts/build.sh <arch> client`, `scripts/build.sh <arch> all` for both.
Per-target client linkage:
- **i386** (real GoldSrc): load-time links Valve's `vgui.so`; mouse-look via SDL
  (`SDL_GetRelativeMouseState` + relative-mode grab, guard `TS_INPUT_SDL` in `cl_dll/inputw32.cpp`),
  with `libSDL2` supplied by the engine at runtime. The relative-mode grab is load-bearing: without it
  yaw locks at ±90° and VGUI panels get no cursor.
- **Windows**: Win32 `GetCursorPos`/`SetCursorPos`, no SDL (no `SDL2.lib` is shipped, so any SDL
  reference fails the link).
- **amd64 / aarch64** (Xash): static openvgui + the `vgui_support` shim. **wasm**: the engine hosts
  VGUI, so no shim (`HALFLIFE_VGUI_STATIC` is off for Emscripten). Both take input pushed by the
  engine (`IN_ClientLookEvent`/`IN_ClientMoveEvent`), no SDL.

### Running / testing (this box HAS a GPU + display — not headless)

Two engines run the mod:
- **xash3d-fwgs (graphical client + server, for visual testing).** `scripts/setup-xash-mod.sh`
  assembles a native game root at `~/Projects/goldsrc.net/xash3d-fwgs/dist/client-x64` with
  our `client.so` + `hl.so` (re-run after every rebuild). Launch:
  `cd …/dist/client-x64 && LD_LIBRARY_PATH=. ./xash3d -dev 2 -game ts +deathmatch 1 +maxplayers 8 +map ts_hammertime`.
  **`+deathmatch 1 +maxplayers 8` is REQUIRED** — without it the listenserver runs single-player
  rules, so stock `respawn()` (dlls/client.cpp) does `SERVER_COMMAND("reload")` instead of
  re-spawning, and pressing fire-to-play bounces you straight back to spectate in an endless
  map-reload loop (looks like a crash; it is not). It also installs CHalfLifeRules instead of
  CTSGameRules. With `+deathmatch 1` you spawn clean.
  Kill an old instance with `pgrep -x xash3d | xargs kill` (NOT `pkill -f client-x64/xash3d` —
  that SIGTERMs the shell; also avoid `pgrep -f gdb`, it matches its own command line). Screenshot
  a focused window: `WID=$(xdotool search --class xash | head -1); import -window $WID out.png`.
  NB: xash on x86-64 needs the ARCH-SUFFIXED lib names `hl_amd64.so`/`client_amd64.so` (the setup
  script handles it).
- **ReHLDS (dedicated server + rcbot, for server behaviour).** The rig at
  `~/Containers/hlds/hlds-linux64-test` — see `reference/RECONSTRUCTION.md` "In-engine test rig".

To try a build in the real Steam client, `scripts/steam-pair.sh <commit>` builds the i386 server and client
at that commit in its own worktree and installs them into the Steam `ts/` as a matching pair (native
`hl_linux` only; set aside the files you want back first). Bisecting a real-client regression with it
takes one restart per step.

gdb both `hl.so` (server) and `client.so` (client) the same way — see §4. The current state and next
steps are in `~/Projects/amxxbench/HANDOFF.md` (§7).

---

## 4. Tooling (installed this session)

Two sides: **Ghidra/rizin/objdump read the *original* `ts_i386.so`** (what we're
recovering); **gdb debugs *our own* built `hl.so`** (what we're producing). Reach
for gdb the moment a runtime test crashes — don't guess from the symptom.

- **Ghidra 12.1.2** — primary decompiler. On PATH as `Ghidra` / `ghidra` (GUI)
  and `analyzeHeadless` (batch). Installed at `~/Tools/ghidra`; uses the
  system JDK 21. Drive headless:
  `analyzeHeadless <proj_dir> <proj> -import reference/ts_i386.so -postScript <script>`.
- **rizin `rz-*` tools** (static, `~/Tools/rizin`, on PATH: `rz-bin`, `rz-asm`,
  `rz-find`, …) — fast sidecar for symbol/export/section/disasm queries. NOTE:
  the static package has **no** main `rizin` REPL binary; use Ghidra for deep
  analysis.
- **`llvm-undname-17`** — MSVC demangler (used by `dump-signatures.sh`) for the
  `mp.dll`/`client.dll` exports.
- **`scripts/gnv2demangle.py`** — our own GNU g++ 2.x demangler (this host's
  `c++filt` dropped `gnu-v2`). Recovers typed C++ signatures for the server
  functions including the TS-specific classes that `mp.dll` never exported.
  Back-references (`T<n>`, `N<count><n>`) are indexed as libiberty does: the class is entry 0
  for members/constructors, the list is empty for free functions, and a multi-digit index ends in
  `_` (so `T18USE_TYPE` is `T1` + `8USE_TYPE`). Validated 2026-10-05: all 159 `mp.dll` typed
  signatures match argument for argument. Unparsed names are C/CRT symbols and gcc runtime
  internals. Output: `reference/signatures-gcc2.{map,txt}`.
  `scripts/decompile.sh` feeds the map into Ghidra so each decompiled function
  is annotated with its real prototype (`// CTSSlowMotion::Spawn()`).
- **`scripts/extract_weapons.py`** — pulls the full `s_weapon_info` weapon table
  (38 weapons: models/anim-ext/fire/clip/damage/fire-type) out of each
  `Write_<weapon>` in the `.so` by resolving PIC string `lea`s + numeric
  immediates. Emits the C table for `dlls/ts_weapons.cpp`. Pattern for bulk
  data-table recovery.
- **objdump** — trivial dumps.
- **gdb** — debug **our own `hl.so`** (the engine loads it; the rig keeps debug
  syms). The server-side counterpart to Ghidra. When a runtime test segfaults, get
  the backtrace instead of guessing:
  `cd <rig> && LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH gdb -batch -nx -ex run -ex bt`
  `  --args ./hlds_linux -game ts +sv_lan 1 +maxplayers 8 +map <map>`
  (gdb intercepts SIGSEGV before the engine's breakpad handler). This caught the
  `EV_Precache` and `DeathNotice` crashes directly. valgrind (memory errors) / rr
  (reverse-debug nondeterministic crashes) are heavier complements if needed.

### 4a. Drop-in coverage harness + the typed-Ghidra RE flow (cont.67–68)

The reconstruction is now **gap-driven by a coverage harness** and the Ghidra decompiles are
**typed + de-truncated**. Full how-to in `HANDOFF.md` ("⭐ Coverage harness + Ghidra RE flow").
The loop: `coverage.py` finds a gap → Ghidra (typed) REs it → implement → re-run the harness to
confirm the gap closed.

- **`scripts/coverage/run.py`** regenerates every report under `reference/coverage/` in dependency
  order. It reads `build-amd64/{dlls/hl.so,cl_dll/client.so}`, so build `scripts/build.sh amd64 all`
  first, and it needs the `upstream` remote (`stock_methods.py` and `divergence.py` read
  `upstream/64bit-demo`). The committed reports go stale between runs (`INVENTORY.md` had not been
  regenerated since 2026-06-28 when checked on 2026-10-05); regenerate before trusting a number.
- **`scripts/coverage/coverage.py all`** → `reference/coverage/REPORT.md`: diffs the EXTERNAL swap
  contract (entity classes, cldll exports, server+client commands, server+client cvars w/ default +
  message wire-size mismatch, user messages, events, entity keyvalues + a default.cfg cross-check).
  MISSING = implement/RE; EXTRA = invented/HL-leftover. Cvars/sizes come from the BINARY, not config.cfg.
  Pure objdump/nm/readelf — no Ghidra dep. Also `reconstruction.py` (per-class method %) + `extract_vtables.py`
  (all 226 vtables → slot→method) + `vtcall.py` (resolve/annotate vtable-dispatch offsets).
- **`extract_vtables.py`** bounds each `__vt_` vtable by its symbol size (bounding by the next vtable
  ran `CMultiSource` into the engine's `DLL_FUNCTIONS` table) and labels the gcc2 header word
  `(typeinfo)` and abstract slots `(pure virtual)`. Validated slot-for-slot against an independent
  relocation decode.
- **`hierarchy.py`** reads each class's base from the original's typeinfo code: gcc2's
  `__tf<Class>()` calls `__tf<Base>()` before registering itself, so these are the declared bases.
  The earlier slot-ownership heuristic was wrong for 83 classes (e.g. `CTSKnife`, `CTSShrapnel`,
  `CM61Grenade` derive from `CTSGrenade`; `CTSGun` from `CBasePlayerWeapon`; `CTSTrigger` from
  `CBaseTrigger`), and some of reTS's class declarations still follow the old answers.
- **`scripts/coverage/spawnflags.py`** (2026-10-22) → `reference/coverage/SPAWNFLAGS.md`: the spawnflag bits the
  FGD names that each entity's class chain tests against `pev->spawnflags` in the original and not in reTS's i386
  build (or the reverse). coverage.py's keyvalue check can't see a flag (env_fade's 8, "Restrict Area", passed it).
  It pattern-matches machine code, so a row is a lead: read it in the source and the decompile, and once found
  handled list it in `reference/coverage/spawnflags_checked.tsv` with the evidence. Needs `build-i386`.
- **`scripts/coverage/audit.py`** (cont.72) → `reference/coverage/AUDIT.md`: the INTERNAL-structure audit —
  diffs OUR build vs the original (`ts_inventory.json`) BOTH directions at file/class/method/member/global level.
  **MISSING** (orig has, we don't = work-list) + **INVENTED** (we have, orig doesn't = review for drift) + FILE
  mismatches. `audit.py <Class>` = one class's full bidirectional view; `audit.py --strict` = CI gate (exit 1 on
  any invention). TS-scoped (stock-SDK diffs are `divergence.py`'s job). This is the DRIVER for the code-restructure
  to source parity. `coverage.py` audits the EXTERNAL contract; the two together make the whole json checkable.
  It matches methods by NAME: it cannot see that a method is non-virtual where the original's is
  virtual, or that slots are out of order. The vtable audit covers that (below).
- **Vtable audit** (`/media/Data3/Projects/amxxbench/rounds/rets-audit/vtable_audit.py`, not yet in the repo): compares every
  original vtable with an i386 build of ours, class by class, slot by slot: missing/extra classes,
  missing/extra virtuals (tagged Valve SDK / halflife-updated / reTS), order, and which class supplies
  each override, each reported at the class that introduces it. Run against a `BUILDTYPE=Debug` i386
  build: at `-O2` gcc folds identical functions and slot names come out wrong.
- **`scripts/coverage/offsets.py`** maps original member offsets to reTS's, per arch, from `BUILDTYPE=Debug`
  builds (pahole reads the DWARF; `--root <tree>` or `--so arch=path`). The original side is the checked-in
  spec `reference/coverage/offset_map.tsv` (the fields TSX/TSE/ARP_TS touch, each with its evidence); the
  tool also reads each class's original object size from its `GetClassPtr` and counts the class's own
  original methods touching each spec offset (0 = stale row). `--all` joins every named original member
  (`members.tsv` + `members_re.tsv`) to reTS by name, for layout-drift work. Re-run it after any layout
  change; reTSX's offsets for reTS come from it.
- **`dlls/rets_api.{h,cpp}`** export `ReTS_GetInterface(int version)`: a versioned struct of get/set
  functions for the player fields AMX Mod X's TSX module (reTSX) touches (cash, free slots, kevlar, slow
  motion, bullet time, powerups, kung fu, superjump, award, melee bar, lunge, gun ammo reserve, each weapon's
  owned flag and clip), plus the game's GetSpeedBySlots, SetPlayerSpeed, the ammo max carry, a thrown
  knife's thrower and a powerup's pickup (`CTSPowerUp::WaitTouch`, for `ts_givepwup`). Not in the
  original; TSX uses it instead of offsets whenever the game library exports it. Setting free slots stores
  the difference to the derived count in `CBasePlayer::m_iFreeSlotsAdjust` (cleared in TSInit, where the
  original resets its counter to 81). AMX Mod X keeps an identical copy of `rets_api.h`
  (`modules/ts/tsx/rets_api.h`); change both together and bump `RETS_INTERFACE_VERSION` on any change.
- **`scripts/coverage/hamdata.py`** writes AMX Mod X's Ham Sandwich gamedata for reTS
  (`gamedata/common.games/virtual.games/ts/offsets-rets.txt`, selected by the `gamedll_export`
  `ReTS_GetInterface` condition) from reTS's own `_ZTV` vtables: each stock TS key is named by the
  original's vtable (`vtables_original.json`) and matched to reTS's slot. Pass every built arch
  (`--so i386=… --so amd64=…`); it fails if the slots differ by arch or a key has no reTS virtual.
  Re-run it after any vtable change.
- **`divergence.py`** defaults to the fork point (`upstream/64bit-demo`, exported to a temp dir), not
  the buildchain's halflife-updated working tree. **`baseline.py`** still needs a built base SDK and
  uses the buildchain's `halflife-updated` build; it records which commit that is and warns when it is
  not `8282fa7`. Its TS-STRIPPED list (stock classes/methods absent from the original) is the
  "remove only what TS removed" work-list.
- **`scripts/coverage/client_members.py`** (cont.72): member skeletons for the CLIENT HUD/prediction classes
  (client.dll has no symbols/RTTI) — binmap-matches our built `client.so` → `client.dll`, types `this`
  (`ApplyClientThis.java`), runs `DumpMembers` → `members_auto_client.tsv`. See §4b.
- **DETERMINISM (cont.72):** the member pipeline is now 0-diff across runs. `re.sh analyze` builds a cached
  single-threaded `re-base`; each `re.sh setup` COPIES it (pins the analysis = reproducible AND faster, no
  re-analyze). `-max-cpu 1` on setup/members pins the member SET; `DumpMembers` canonicalizes the type column
  (Ghidra's decompiler integer-type inference wobbles, unreachable by -max-cpu; float/double/ptr stable). Re-run
  `re.sh analyze` only on a binary/analysis change. The reports are byte-stable too: `baseline.py` and
  `export.py` no longer depend on Python's hash seed, so a diff in `reference/coverage/` is a real change.
  Known type gap: curated names on offsets Ghidra typed `int` keep `int` even when the name is a float
  (37 `m_fl*` members as of 2026-10-05).
- **`scripts/coverage/setup-ghidra.sh`** builds persistent projects under `recovered/ghidra/{server,client}`
  (gitignored, reboot-safe) with the noreturn truncation disabled + the gcc2 signatures/entvars_t/MSVC
  return types applied (`scripts/ghidra/{DisableNoReturn,FixNoReturn,ApplyTypes,DecompAt,DecompRefs}.java`),
  so decompiles read with typed `this`/args/returns + `pev->fields`; the client project then gets
  `ApplyWinTypes` through `dc.sh setup client` (§4a.2). The huge `ClientCommand` still
  truncates in Ghidra — use objdump + GOT (`_GLOBAL_OFFSET_TABLE_`=0x165E10) for its dispatch; `readelf -W -r`
  resolves the .data pointers (vtables, cvar_t, message names) that read as zero in the file.

#### 4a.1 The `re.sh` orchestrator + ApplyInventory (the current server-RE front-end)

`scripts/ghidra/re.sh` is **one entry point** for the whole server pipeline — prefer it over hand-chaining
`analyzeHeadless` for the server `.so`. The applied project lives at `recovered/ghidra/re` (gitignored).

```bash
scripts/ghidra/re.sh setup            # (re)build the applied project: run.py (refresh inventory) +
                                      #   fresh import/analyze + ApplyInventory. Do once, and after
                                      #   coverage/script changes. FRESH each time on purpose (below).
scripts/ghidra/re.sh members          # DumpMembers -> reference/coverage/members_auto.tsv skeleton
scripts/ghidra/re.sh <Class>          # decompile every method of a class (addrs via filemap.py)
scripts/ghidra/re.sh 0x7ea30 0x7decc  # decompile by Ghidra address (= nm addr + 0x10000)
scripts/ghidra/re.sh refs <Class|addr># who CALLS/uses it — decompile every referencing function
scripts/ghidra/re.sh export <Class>   # write the class's decompile to recovered/decomp/<Class>.c
```

- **`ApplyInventory.java`** is a **superset of `ApplyTypes.java`** — the JSON/tsv-driven script that makes
  the server decompiles read like source. It is driven by `reference/coverage/` artifacts
  (`ts_inventory.json` omni dump + `members*.tsv`/`hierarchy.tsv`/`precache.tsv`/`funcfile.tsv` + `VTABLES.md`
  + `signatures-gcc2.map` + MSVC returns + `progdefs.h`/`eiface.h`). Per class it builds a member **struct**
  from the save tables (merged up the chain, `pev`@0 → `this->poweruptype`) and a **vtable** struct (typed fn
  ptr per slot at vptr `@0x60` → `this->vtbl->Precache(this)`). Program-wide it applies typed gcc2+MSVC
  prototypes, `entvars_t` (`this->pev->takedamage`, `gpGlobals->time`), `enginefuncs_t` + the PIC GOT slots
  (`g_engfuncs->pfnSetModel(...)`), typed `.rodata` FP constants relabelled with their value (`dc_0_1`/`fc_64`),
  clean demangled per-class names, and a plate comment per function (its `.cpp` + assets/cvars/commands it refs).
  It consumes `hierarchy.tsv`, so projects built before the 2026-10-05 hierarchy fix merged member
  structs up the wrong chains for the affected classes: re-run `re.sh setup`. The chain merge takes
  named members (saved, curated) from the whole chain before any `members_auto` placeholder, so an
  intermediate class's `field_0xNN` can't hide an ancestor's real name (it hid `m_pPlayer` in `CTSGun`).
  `re.sh export <Class>` matches the namespaced name (`CTSGun::Deploy`), skips PLT thunks, and rewrites
  each `<Class>.c` per run; before 2026-10-05 it only found functions ApplyInventory had not renamed.
- **Not idempotent** — ApplyInventory *renames* functions, so re-applying breaks the mangled-name lookup.
  That's why `re.sh setup` always rebuilds the project FRESH; the decompile/refs/export paths only read it (fast).
- **`DumpMembers.java`** recovers transient (unsaved) members: their names aren't in the binary, but their
  OFFSET + TYPE are recoverable from how each class's methods access `this`. It emits `members_auto.tsv` (a
  placeholder-name skeleton, excluding the save-table offsets). **Curate** those placeholders into
  `members_re.tsv` with RE-grounded names (e.g. `field_0x88` → `m_flExpire`), which ApplyInventory then layers
  on top. (Workflow: `run.py`→`members.tsv` = saved/named/authoritative; ApplyInventory+DumpMembers→
  `members_auto.tsv` = unsaved offset+type; you curate → `members_re.tsv`.)
- **`DecompRefs.java`** now takes **multiple** addresses and decompiles every unique referencing function —
  the static caller-finder ("who calls/uses this?"). `re.sh refs <Class>` resolves a class to all its methods
  first. **Correctness guards** baked into ApplyInventory (don't trust an unverified label): class structs are
  PADDED so an unsaved member renders honestly as `this->field_0x88` (never wraps to `this[1].<wrong field>`),
  and `entvars_t` array-size macros (`controller[NUM_ENT_CONTROLLERS]` etc.) are resolved before parsing so
  entvars doesn't drift and mislabel every field past `~scale`. See `[[ghidra-correctness-gotchas]]`.

#### 4a.2 `dc.sh` — decompile-by-address for client.dll / mp.dll (the re.sh counterpart)

`re.sh` is server-only (`ts_i386.so`, the `re` project). For the **Windows binaries** use
**`scripts/ghidra/dc.sh <client|mp|server> 0x<addr> [0x<addr>…]`** (built cont.69v) — it picks the right
Ghidra project (`recovered/ghidra/{client clientdll, mpdll mpdll, re re}`), applies the correct address base
(PE imagebase `0x10000000 + RVA` for client/mp; `addr + 0x10000` for the server `.so`), runs `DecompAt.java`,
and cleans the output with the same multi-line filter as re.sh. Feed it the **RVA straight from a live trace**
(`live.py resolve`/`bt`). It replaces the long hand-typed `analyzeHeadless …` incantation. For the server,
prefer `re.sh <Class>` (it resolves Class→methods + applies the full inventory); `dc.sh server` is the quick
by-address path. The mp.dll has 159 typed MSVC exports → readable `Class::Method` signatures (use it to name a
live-trace RVA, then find the SAME function in `ts_i386.so` and decompile it with the typed `re.sh` flow).

**Project setup: `dc.sh setup <client|mp>`** imports `reference/client.dll` / `mp.dll` into
`recovered/ghidra/{client,mpdll}` when the project is missing (noreturn discovery off) and applies
`scripts/ghidra/ApplyWinTypes.java`. A decompile with no project runs it first; it is idempotent, so `setup`
also re-applies it to an existing project. `DC_GHIDRA_DIR=<dir>` puts the projects elsewhere (a throwaway
fresh import). Why: both DLLs link the 2007 MSVC CRT statically and Ghidra's Function ID databases only know
VS2012-2019, so sprintf & co. were FUN_ with a two-argument guess and every argument after a format was
dropped (0x10001000 read `RandomLong(0,4); sprintf(buf, fmt)` for `sprintf(buf, "{shot%i", RandomLong(0, 4) + 1)`,
and a port took {shot0..4}). ApplyWinTypes applies:
- variadic signatures found from the callers: a function handed a printf-format literal that takes the address
  of the argument after it (va_start) gets `f(..., const char *format, ...)`. The CRT ones are named from their
  bodies (sprintf, _snprintf, sscanf, vsprintf, _vsnprintf, _output); mp.dll's three by role (UTIL_VarArgs,
  UTIL_LogPrintf, VoiceServerDebug, the server's only variadics). Other callees keep FUN_ with the type.
- `__chkstk` named, with the cspec's `alloca_probe` call-fixup, so functions with over a page of locals keep
  their frame.
- the engine tables typed from this repo's SDK headers (engine/, common/, pm_shared/), each cut to the size
  the binary copies: the client's `gEngfuncs` (cl_enginefunc_t, 98 entries, Initialize's copy, with its
  pTriAPI/pEfxAPI/pEventAPI/pDemoAPI/pNetAPI/pVoiceTweak sub-tables), `IEngineStudio` (43) and `pmove`;
  mp.dll's `g_engfuncs` (enginefuncs_t, 155), `gpGlobals`, `gFunctionTable` (DLL_FUNCTIONS) and `pmove`.
  Addresses come from the PE export directory, not symbols (ApplyClientThis's binmap renamed client.dll's
  HUD_GetStudioModelInterface export to `CHudGeiger::Init`). The by-value structs match an i386 gcc build
  member for member (playermove_t, globalvars_t, physent_t, usercmd_t, pmtrace_t).
- a call-site prototype for each call to a variadic function or table member whose format is a literal, with
  exactly the arguments the format asks for, so a call whose pushes straddle branches still shows them all
  (mp.dll's kill log line showed 6 of its 17 without it). A call with fewer pushes than its format, or with
  pushes reached through a jump, keeps the decompiler's guess.

`dc.sh scan <client|mp> out.tsv` (`ScanVarargs.java`) lists every variadic call with its format, the decompiled
call and the raw instruction behind each variadic argument; `scripts/ghidra/varargs-ports.py --client c.tsv
--mp m.tsv` joins it with the addresses cited in cl_dll/ and dlls/. The server project (`re.sh`) still has the
blind spot: ApplyTypes/ApplyInventory drop the "..." of the gcc2 signatures and type g_engfuncs entries as an
argumentless `engine_fn`, so `re.sh 0x9e330` shows `UTIL_VarArgs("%s returned the briefcase! %i #TS_Frg")`
with no arguments. Read such calls in objdump, or in mp.dll (same source) through dc.sh.

The whole client/server RE + live-debug toolchain, at a glance: `re.sh` (server decompile, typed) · `dc.sh`
(client/mp/server decompile by addr) · **`scripts/live.py`** (drive the running original — `pid`/`base`/
`resolve`/`bt`/`read`/`readabs`/`vmethod`/`watch`/`break`; see §4b) · `scripts/coverage/*` (the gap harness).

#### 4a.2b Caller-finding: `refs` (direct) vs `vrefs` (virtual) + the member-name producer (cont.71 cont.3)

- **`re.sh vrefs <Method>`** (`scripts/coverage/vrefs.py`) — the **reverse virtual-call finder**. A virtual call
  goes through `this->vtbl[slot]` and NEVER references the method's address, so `re.sh refs`/`DecompRefs.java`
  AND Ghidra's `get_function_callers` all find NOTHING for an overridden method (this is why tracing
  `CTSGun::PrimaryAttack`'s callers returned empty). `vrefs` inverts `vtables_original.json` (method→slots),
  scans the objdump disasm for `call *0xSLOT(%reg)` sites (and `mov 0xSLOT(%vptr),%r` + `call *%r`), and ranks:
  **self-dispatch** (the calling fn's own class has the method at that slot — high confidence, e.g.
  `vrefs PrimaryAttack` → `CBasePlayerWeapon::ItemPostFrame`) vs same-slot-other-class (possible). Calls
  through a register loaded from the PIC GOT (`X(%ebx)`, i.e. `g_engfuncs` and other global tables) are
  dropped: before 2026-10-05 they were reported as virtual calls (`REMOVE_ENTITY` at `0x58` showed up as
  a `TakeDamage` caller). Use `refs` for non-virtual/direct, `vrefs` for virtual. (Neither is the
  receiver-exact truth that a live gdb bt gives — see §4b — but `vrefs` is the static first cut.)
- **`scripts/coverage/curate_members.py`** — the **producer for `members_re.tsv`** (which was an ORPHAN: no
  script made it, so it stayed empty and every transient member rendered as `this->field_0xNN`). It scrapes our
  reconstruction headers' offset provenance (`m_weaponType … (gun+0xc8)`, struct-relative `// +0xNN`, `name@0xNN`)
  and **cross-checks each against the BINARY-derived `members_auto.tsv`** — emitting only corroborated
  `(class,offset)` rows (type from the binary, name from our source), and reporting the rest as UNVERIFIED rather
  than inventing a wrong-but-plausible label. Wired into **`re.sh members`** (runs after `DumpMembers`); re-apply
  with `re.sh setup`. The member loop is now: `members.py`(saved) → `DumpMembers`(unsaved skeleton) →
  **`curate_members.py`**(names, from our RE) → `ApplyInventory`(merges saved>curated>auto).
- ✅ **FIXED (cont.71 cont.4):** `members_auto.tsv` used to **miss the biggest transient-only TS classes** —
  `CTSGameRules`/`CTSStunt`/`CTSAnimation` had ZERO recovered members. Root cause was NOT demangling (all their
  symbols demangle): `ApplyInventory` only gives a class a real struct (so the decompiler renders `this->field_0xNN`)
  when it's in `members.tsv`/`hierarchy.tsv`; a class of pure transient state is in neither, so its `this` stays an
  OPAQUE pointer and accesses read `*(t*)(this + 0xNN)`. **Fix = teach `DumpMembers` to ALSO parse that opaque form**
  (not make every class a giant struct — that was a dead end: padding all 223 classes to 0x8000 overflowed the
  decompiler's datatype budget and made it FAIL on stock functions, cratering coverage to 174). With the opaque
  parse, `members_auto` went 860→1028 rows / 125→**137 classes**, recovering CTSStunt/CTSGameRules/CTSAnimation/
  CTSSharedEmitter/CBulletManager with **zero regression**, and `curate_members` then named them from our source.
- **Completeness over speed (cont.71 cont.4):** all Ghidra-script decompile timeouts raised to **900s/fn**
  (`DumpMembers`/`DecompAt`/`DecompRefs`/`DecompExport`) so a slow function is never silently dropped.

#### 4a.3 The Ghidra **MCP** — interactive front-end ON TOP of our tooling (cont.71 cont.3)

There's a **Ghidra MCP** server (`mcp__ghidra-mcp__*`, 184 tools) that attaches to a **live Ghidra GUI
instance** over a UDS socket. It is **not a replacement** for `re.sh`/`dc.sh`/`coverage` — it's a faster,
interactive *front-end* on the SAME projects those scripts build. Use it for **reading + navigating**;
keep the scripts for **building the typed project + tracking completeness**. Three tools, three jobs:

- **Ghidra MCP** → interactive decompile + **xref/callgraph** navigation. `decompile_function`,
  `get_xrefs_to`, `get_function_callers/callees`, `get_full_call_graph`, `search_functions`, `list_*`.
  Returns a decompile in ~1s (no per-call `analyzeHeadless`/JVM spin-up), and it does the thing `re.sh`
  CAN'T: "who calls this / what xrefs / follow the chain" without dropping to `objdump`+grep. **This is now
  the first reach for "read/trace this function and who calls it."**
- **`re.sh` / `dc.sh`** → BUILD the typed `re`/`clientdll`/`mpdll` projects (ApplyInventory) + batch-decompile
  a whole class + the reproducible/scriptable path. The MCP only has the typed view BECAUSE `re.sh setup`
  built it; a fresh MCP `import_file` is **untyped** (`*(int*)(param_1+0xcc)`, not `this->m_pActiveItem`).
- **`scripts/coverage/*`** → the gap harness / inventory / completeness metric. The MCP does NONE of this.
- **`scripts/live.py` (gdb)** → RUNTIME behaviour. No static tool — MCP included — answers held-vs-click,
  recoil, or any "what actually happens at run time" question. That's always the live trace (§4b).

**Setup to get the BEST of both:** run **`scripts/ghidra/mcp-up.sh`** first — it checks the project lock, finds
the live MCP socket, and prints the exact steps. Then in the Ghidra GUI open **`recovered/ghidra/re/re.gpr`** +
the `ts_i386.so` program; then `list_instances` → `connect_instance("re")` → `open_program("/ts_i386.so")` →
`unload_tool_group("malware")` (drop the malware/crypto/IOC tools we never use — less noise). Now MCP decompiles
are typed (`CTSGun::ShotgunFire(CTSGun *this)`, `pev->`, `/* TS_gun.cpp */` plate) AND you get interactive
xrefs. (A blank instance with no project open → `import_file` fails "No project is currently open"; open/create
a project first.)

- ⚠️ **`.lock` gotcha (the real one):** a Ghidra project (`*.rep/`) is **single-writer** — it takes a lock
  when opened. So **you cannot run `re.sh`/`dc.sh` against a project the GUI currently has open, and vice-versa**
  — the headless `analyzeHeadless` fails on the lock (and `re.sh setup`, which rebuilds `re` FRESH, would
  clobber what the GUI sees). While the GUI holds `re`, drive it via the **MCP** (reads are fine); to go back
  to `re.sh <Class>`/`dc.sh server`/`re.sh setup`, **close the program/project in the GUI first**. (`dc.sh
  client`/`dc.sh mp` use the separate `clientdll`/`mpdll` projects, so they're unaffected unless the GUI also
  has THOSE open.) Corollary: **only READ via the MCP on the `re` project — don't rename/mutate it**, or it
  drifts from the `re.sh`-generated state that `re.sh <Class>`/`export` rely on (mutate a scratch project).

### 4b. When static RE dead-ends: LIVE gdb trace + the Windows `mp.dll` cross-reference (cont.69u)

Static reading dead-ends on **virtual/indirect calls** (you can't follow `entity->vtable[slot](...)` by
eye) — and a wrong guess there is "making shit up vs the RE" (the cardinal sin; the goal is drop-in
fidelity). **The user KEEPS THE ORIGINAL TS RUNNING in `hl.exe` (Proton/Wine) and it is ALWAYS available
for you to debug live — reach for it as a first-class tool whenever static RE is uncertain or dead-ends.
Don't guess; trace it.** Two tools settle it definitively:

- **★ Live gdb backtrace on the running ORIGINAL.** The real TS runs in `hl.exe` under Proton/Wine (the
  server is `ts/dlls/mp.dll`, the client `ts/cl_dlls/client.dll`). This box can `gdb -p <pid>` it
  (ptrace_scope=1 but same user works; gdb shows the 32-bit code as garbage by default — read raw bytes,
  not the auto-disasm). **`scripts/live.py` scripts the whole boilerplate**: `live.py pid` / `base <module>`
  (reloc-free prologue scan) / `resolve 0xADDR…` and `bt` (raw live addr / piped gdb backtrace → module +
  RVA + nearest export) / `read <module> 0xRVA [f|d|u|p]` / `readabs 0xADDR` / `vmethod 0xOBJ <slot>`
  (this→vtable[slot] → resolve) / **`watch <module|abs> 0xRVA|0xADDR --go "msg"`** and **`break … --go`**
  (run BACKGROUNDED: prints `>>> ARMED — <msg>` once set so you tell the user the action, blocks until it
  trips, then prints the resolved backtrace). A value-`watch` fires only on CHANGE → e.g. watching
  `cl.viewangles` with no mouse isolates the recoil (this is how cont.69v located the angle-recoil). Recipe:
  0. **Find the live PID each session** (it changes): `pgrep -f 'Half-Life/hl.exe'` (pick the actual
     `hl.exe` Wine process — the big RSS one, not the launcher/reaper). Ask the user to confirm it's
     running if `pgrep` is empty.
  1. `cat /proc/<pid>/maps | grep mp.dll` → the load region. **Wine maps PE by RVA, and the live base ≠
     `region_start + file_offset`.** So find a known function's TRUE address by searching process memory
     for its prologue bytes: read `reference/mp.dll` at the function's file offset (`rz-bin -E` gives
     paddr/vaddr), then `python3` scan `/proc/<pid>/mem` over the r-xp region for those bytes →
     `live_addr`; **effective base = `live_addr − RVA`** (RVA = `vaddr − 0x10000000`). (Client side:
     same with `client.dll`.)
  2. `gdb -p <pid> -batch -x script.gdb` with a **HARDWARE** bp (`hbreak *<addr>`; hardware = no code
     patching, auto-clears on detach — safe for the user's live game), `continue`, then on hit dump
     `x/200xw $esp` + regs + `detach`. Run it backgrounded with a `timeout`; tell the user to reproduce
     the exact action (e.g. "throw a knife at glass — only the knife") and wait for the hit.
  3. Resolve the stack's return addresses (values in the mp.dll r-xp range) to RVAs (`addr − base`) →
     names via `rz-bin -E reference/mp.dll`. That's the EXACT call chain — the definitive caller-finder.
     (Read args at a fn entry via the stack: `__thiscall` = `this` in `ecx`, args at `[esp+4]`, `[esp+8]`…)
- **`mp.dll` cross-reference (Ghidra project `recovered/ghidra/mpdll`).** mp.dll has **159 fully-typed
  MSVC exports** (readable `Class::Method` signatures, far clearer than gcc2 mangling). Use it to (a)
  resolve live-trace RVAs to readable names, and (b) confirm a function's name so you can find the SAME
  function in `ts_i386.so` (same source, gcc2) and decompile it with the typed flow above. Build it like
  the others: `analyzeHeadless recovered/ghidra/mpdll mpdll -import reference/mp.dll -preScript
  DisableNoReturn.java`, then `DecompAt.java` at `0x10000000+RVA`.
- **★ `scripts/binmap.py` — resolve a `.so` function NAME to its live `mp.dll` RVA (cont.71 cont.4).** mp.dll
  exports only ~325 of ~2400 functions, so to `live.py break` a NON-exported one (e.g. `ShotgunFire`) you need
  its address. `binmap.py <name>` matches the two binaries (same source) two ways: **export-name** (exact, 325)
  + **string-anchor** (a fn referencing `"events/pump.sc"` in the `.so` references it in mp.dll — 286 confident
  + 170 flagged `string?` ambiguous, when several siblings share strings). `binmap.py CTSGun::Precache` →
  `mp.dll 0x54190` → feed to `live.py break mp.dll 0x54190`. ⚠ heed the `string?` AMBIGUOUS flag — verify those
  with a live trace (or breakpoint a uniquely-string'd caller and step in); a string-poor fn like `ShotgunFire`
  won't match at all (fall back to a caller). Cache: `binmap.py --build` → `reference/coverage/binmap.tsv`.
  binmap is **env-parameterizable** (`BINMAP_SO/MP/GOT/MAP/OUT`) so the SAME machinery does the CLIENT pair.
- **★ `scripts/coverage/client_members.py` — member skeletons for the CLIENT (HUD/prediction) classes (cont.71 cont.4).**
  `client.dll` has NO symbols, NO RTTI (only `.?AVtype_info@@`), and the HUD classes aren't exported — so the
  server's symbol-driven member recovery doesn't transfer. Instead it brings the NAMES over from OUR build:
  (1) binmap-match our `build-i386/cl_dll/client.so` (full symbols) → `client.dll` RVAs (→ `binmap_client.tsv`);
  (2) `ApplyClientThis.java` marks those functions `__thiscall` + renames them into class namespaces;
  (3) `DumpMembers.java` (it now reads the class from the NAMESPACE and parses the `(int)this + 0xNN` cast form
  that MSVC's `void*` auto-`this` produces) → `members_auto_client.tsv`. Yields the CHud* skeletons (CHud/
  CHudHealth/CHudSpectator/CHudTSWeaponBar/…, ~51 classes). Gotchas hit building it: Ghidra's `__thiscall this`
  is an AUTO-param (`setDataType` throws "Auto-parameter may not be modified" — use `updateFunction`), and a
  pointer to a SIZE-0 opaque struct renders `void *this` (pad it, or just read the namespace + `(int)this` form).

**Lesson (the knife→glass break):** I claimed I "couldn't find" it and nearly invented an angle/
accumulation model — but it was in `ts_i386.so` all along (`WaitThink__8CTSKnife`, a per-frame swept
`TakeDamage`); I'd just missed it in a grep. The live trace + mp.dll names pointed straight at it. **So
when a static trace dead-ends, do the live gdb backtrace BEFORE reconstructing — don't guess.** (See the
`flag-unfound-re-early` memory.)

---

## 5. Change ledger (Serena)

We maintain a running ledger of reconstruction progress as **Serena memories**
(MCP) scoped to this project — one entry per meaningful step (class reconstructed,
ABI verified, decision made). At the start of a session, read the Serena memories
to see what's done; append a new ledger entry when you complete a unit of work.
Keep the ledger factual: what was reconstructed, how it was verified, what's next.

---

## 6. Conventions

- **No `Co-Authored-By` trailers in commits** (user preference across all
  goldsrc-net repos).
- Match the SDK's surrounding code style when writing reconstructed source.
- Anything reTS does differently from TS 3.0, whether added on purpose (a cvar, an export) or forced by
  the build (layout, vtable slots), goes in `reference/DEVIATIONS.md`, with its default and what it breaks.
- `reference/` is read-only source-of-truth — never modify the original
  binaries; regenerate derived data with `dump-signatures.sh` (MSVC signatures, raw gcc2 symbols) and,
  for `signatures-gcc2.{map,txt}`, run `scripts/gnv2demangle.py` over `symbols-gcc2.txt` (map = the
  parsed `name  =>  signature` lines in input order, txt = `--sig` output sorted unique).
- A class is "built" when it compiles + links + exports its entity factory; it's
  "done" when its **behaviour** is verified in the engine — not just when it
  compiles. Be honest about which.
- This repo's `.git/config` sets `commit.gpgsign = false` (overriding the global `true`), so
  commits here are unsigned.

---

## 7. Picking this up

`CLAUDE.md` is **tracked in this repo** (it was force-added past the user's global
`~/.gitignore`, which matches it), so edits here are commits. The current pickup doc for reTS is
**`~/Projects/amxxbench/HANDOFF.md`** (repo state, releases, the live server, next steps); this repo's own
`HANDOFF.md` is the record of the reconstruction sessions up to July 2026, for history. Progress per change
lives in the Serena ledger (`.serena/memories/ledger.md`) and `reference/RECONSTRUCTION.md`.
