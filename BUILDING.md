# Building reTS

reTS builds two libraries per platform: the server (`hl.so` / `hl.dll`, installed under TS's names) and
the client (`client.so` / `client.dll`). CMake is the only build system; the scripts below drive it.

## Getting the source

```sh
git clone --recursive git@github.com:goldsrc-net/mod-ts.git
```

The client needs the submodules: `third_party/openvgui` (with its `miniutl`) and
`third_party/vgui_support` (with its `vgui-dev`), both public: openvgui is FWGS's, vgui_support is
goldsrc-net's fork of FWGS's at its `64bit` branch (FWGS's code plus the quit fix the client needs).

## Linux (i386, amd64, aarch64)

The Linux libraries build in the Docker image from
[goldsrc-net/buildchain](https://github.com/goldsrc-net/buildchain), a Debian 10 image with the cross
toolchains. Building there keeps the libraries on glibc 2.28, so they load on servers back to about 2018.
A native build on a current distro links a newer glibc and won't load on Debian 12 or Amazon Linux 2023.

Build the image once, from a buildchain checkout:

```sh
git clone git@github.com:goldsrc-net/buildchain.git hlds64-buildchain
( cd hlds64-buildchain && make image )      # -> hlds64-buildchain:debian10
```

Then, from `mod-ts`:

```sh
scripts/build.sh i386 all       # Steam's Half-Life and HLDS: build-i386/dlls/hl.so, build-i386/cl_dll/client.so
scripts/build.sh amd64 all      # 64-bit Xash3D and ReHLDS
scripts/build.sh aarch64 all    # arm64 servers
```

- The second argument is `hl` (server, the default), `client` or `all`.
- `BUILDTYPE=Debug` builds -O0 with debug info; use it for gdb and for comparing vtables and member layout
  with the original.
- `BUILD_DIR=<dir>` builds somewhere other than `build-<arch>`.
- `IMAGE=<image>` uses another build image.

Check the glibc floor of a build:

```sh
objdump -T build-i386/dlls/hl.so | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1
```

On i386 the client links Valve's `vgui.so` and gets mouse input from SDL, which the engine provides. The
64-bit clients link openvgui statically and take input from the engine, with no SDL.

## Windows (Win32)

`scripts/build-win.sh` cross-builds `hl.dll` and `client.dll` on Linux with clang-cl and lld-link. They
use the MSVC ABI, so Valve's win32 `vgui.lib` links as it does under Visual Studio. It needs LLVM (by
default `/usr/lib/llvm-21/bin`, or set `LLVM_BIN`) and the x86 MSVC runtime and Windows SDK from
[xwin](https://github.com/Jake-Shadle/xwin), about 300 MB:

```sh
cargo install xwin
xwin --accept-license --arch x86 splat --output ~/.cache/xwin/splat-x86
scripts/build-win.sh            # -> build-win32-clangcl/dlls/hl.dll, build-win32-clangcl/cl_dll/client.dll
```

`BUILDTYPE=Debug` builds with PDBs, and needs xwin's debug runtime (`splat --include-debug-libs`).
lld-link rejects the `SECTIONS` block in `dlls/hl.def`, so this build uses `dlls/hl-lld.def`.

CI builds the same two libraries with MSVC on `windows-latest`.

## Packaging and releases

```sh
scripts/package.sh              # -> dist/reTS-<version>.zip
scripts/package.sh --debug      # -> dist/reTS-<version>-debug.zip, not stripped, with PDBs
```

`package.sh` builds the i386 and Win32 libraries and lays out the patch with TS's own file names (see
[INSTALL.md](INSTALL.md)), plus a `README.txt` and a `MANIFEST.tsv` with each file's sha256. It refuses a
tree with uncommitted changes, so every package is a commit. The version comes from `git describe`: the
`v*` tag on a tagged commit, `<tag>-N-g<hash>` after one.

Pushing a `v*` tag runs `.github/workflows/release.yml`, which builds everything in CI and publishes a
GitHub release with the zip:

```sh
git tag v0.0.3 && git push origin v0.0.3
```

Windows builds carry the version in their version resource.

## Testing a build

- `scripts/setup-xash-mod.sh` puts the amd64 server and client into a native Xash3D game root, for
  playing and screenshotting on a desktop. Run the map with `+deathmatch 1 +maxplayers 8`; without them
  the listen server runs single-player rules.
- `scripts/setup-rehlds-mod.sh` drops the amd64 server into a ReHLDS dedicated server rig, loaded through
  Metamod so RCBot runs.
- `scripts/steam-pair.sh <commit>` builds the i386 server and client at a commit, in its own worktree, and
  installs them as a pair into Steam's `ts/` folder for testing in the real client under `hl_linux`.
  Useful for bisecting.

## Checking the reconstruction

`reference/` holds TS 3.0's original binaries and the inventories derived from them. After
`scripts/build.sh amd64 all`, `scripts/coverage/run.py` regenerates the reports in `reference/coverage/`,
which compare reTS with the original: entities, commands, cvars, user messages, events, classes, methods,
members and vtables.
