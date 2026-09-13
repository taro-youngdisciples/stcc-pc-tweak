# Development guide / 開発ガイド

This document is for people who want to build stcc-pc-tweak (the `stccfix` DLL), understand how it works, or continue
the reverse engineering. The project is a work in progress.
Player documentation is in [README.md](../README.md) / [README.ja.md](../README.ja.md).

Source comments and the internal handoff notes (`HANDOFF.md`) are mostly in Japanese.

The code, tools, reverse engineering notes and documentation were written with extensive assistance from Claude
(Anthropic) through Claude Code, directed and tested on real hardware by the project owner. `HANDOFF.md` is the
working log of those sessions. See [AI assistance](../README.md#ai-assistance).

- [Repository layout](#repository-layout)
- [Prerequisites](#prerequisites)
- [Getting the source](#getting-the-source)
- [Building and deploying](#building-and-deploying)
- [Architecture](#architecture)
- [How widescreen works](#how-widescreen-works)
- [Address database](#address-database)
- [Diagnostics](#diagnostics)
- [Rules and conventions](#rules-and-conventions)
- [Release packaging checklist](#release-packaging-checklist)
- [Not implemented yet](#not-implemented-yet)

## Repository layout

```
CMakeLists.txt              Win32 build: MinHook (static lib), stccfix (dinput.dll), joylog.exe
HANDOFF.md                  Project handoff notes (Japanese): goals, decisions, test results
data/addresses/
  versions.toml             Known STCC.EXE versions (SHA-256, size, link date, supported flag)
  stcc_jp-1.02.toml         Functions and globals found in v1.02 (address, name, confidence, notes)
docs/
  inventory.md              Disc/install inventory summary (file layout, DirectX generation, settings files)
src/stccfix/                The dinput.dll proxy
  dllmain.cpp               DllMain: config, log, version check, byte patches, hook installation
  proxy_dinput.cpp          Exports forwarded to the real System32/SysWOW64 dinput.dll (lazy-loaded)
  dinput.def                Export table
  config.cpp / common.h     stccfix.ini parsing, Config struct, shared declarations
  log.cpp                   stccfix.log writer
  patch.cpp                 Version detection and verified byte patches
  vtable_hook.cpp/.h        COM vtable hooking helper
  hooks_ddraw.cpp           DirectDraw/Direct3D hooks: widescreen, logging
  hooks_dinput.cpp          DirectInput hooks: device filter/type, axis remapping, force feedback
  hooks_window.cpp          Window sizing, aspect-locked resizing, borderless fullscreen
  hooks_hud.cpp             Race HUD anchoring in widescreen
  hooks_game.cpp            Traces of the game's own Direct3D init functions (LogGraphics)
  stccfix.ini               Settings template (ASCII only), shipped in releases
third_party/minhook         MinHook v1.3.4 (git submodule, BSD-2-Clause)
tools/
  build.ps1                 Configure, build and optionally deploy stccfix
  aspect.ps1                Set AspectRatio (stccfix.ini) and Resolution (dgVoodoo.conf) together
  wrapper.ps1               Deploy/remove dgVoodoo2 in the game folder
  package.ps1               Build the release zip into dist\
  dgvoodoo/dgVoodoo.conf    dgVoodoo2 configuration for this game (shipped in releases)
  joylog/joylog.cpp         Stand-alone DirectInput 8 controller logger
  ghidra/                   Ghidra headless wrapper and scripts
  exe_probe.py              PE/GUID/constant probe for STCC.EXE
  testpatch.py              Throwaway research patches applied to a copy of the exe
  stcc-inventory.ps1        File inventory and diff of disc/install folders
  requirements.txt          Python dependencies
```

Excluded by `.gitignore` (never commit): game executables and data (`*.exe`, `*.bin`, `*.mrg`, `*.bmp`, `*.dat`, `*.wav`, `stcc.ini`, ...),
disc images, `inventory/`, Ghidra projects, logs and dumps, `build/`, `*.dll`, `*.pdb`, `*.asi`, `.venv/`.

## Prerequisites

| Tool | Version used | Needed for |
|---|---|---|
| Windows | 11 x64 | Everything (the game is a 32-bit process) |
| Git | any recent | Cloning with submodules |
| Visual Studio Build Tools 2026 | 18.x, MSVC 14.51 | Building. Workloads/components: C++ desktop development with the **x86** MSVC toolset and **C++ CMake tools for Windows** (`Microsoft.VisualStudio.Component.VC.CMake.Project`, which `build.ps1` looks for) |
| Windows SDK | 10.0.26100 | Building |
| CMake | 3.24+ | Bundled with the VS CMake component; `build.ps1` uses that copy |
| Python | 3.13 (3.14.2 also worked) | `tools/*.py` only. Create `.venv` and install `tools/requirements.txt` (pefile, capstone, tomli-w) |
| Ghidra | 12.1.3 | Optional, static analysis |
| JDK | 21 (Eclipse Temurin) | Optional, required by Ghidra |
| dgVoodoo2 | 2.87.4 | Running the game |

`build.ps1` also accepts Visual Studio 2022 (major version 17). <!-- TODO: VS 2022 builds are untested -->

Python environment:

```powershell
py -3.13 -m venv .venv
.venv\Scripts\pip install -r tools\requirements.txt
```

## Getting the source

```powershell
git clone --recursive https://github.com/taro-youngdisciples/stcc-pc-tweak.git
# or, in an existing clone:
git submodule update --init
```

## Building and deploying

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1                     # Release build only
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Config Debug       # Debug build
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Deploy             # build and copy to the game folder
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Remove             # remove dinput.dll/.pdb from the game folder
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Deploy -GameDir "E:\STCC"
```

- `build.ps1` locates Visual Studio with `vswhere`, configures `build\` with `-A Win32` on first run, and builds.
  Output: `build\Release\dinput.dll` (and `build\Release\joylog.exe`).
- `-Deploy` copies `dinput.dll` and `dinput.pdb` to `-GameDir` (default `D:\Games\STCC`). It copies
  `src\stccfix\stccfix.ini` only if the game folder does not already have one, so your settings are kept.
  After adding new ini keys, merge them into the deployed file by hand.
- `-Remove` deletes `dinput.dll` and `dinput.pdb` only; `stccfix.ini` and `stccfix.log` stay.
- If you copied the repository from another machine, delete `build\CMakeCache.txt` (or the whole `build\`) before building.
- CMake refuses 64-bit configurations. The CRT is linked statically, so the DLL has no runtime dependencies.
  Compiler options: C++20, `/W4 /utf-8 /permissive-`.

Related scripts:

```powershell
powershell -ExecutionPolicy Bypass -File tools\wrapper.ps1 status|enable|disable [-GameDir ...] [-DgVoodooDir ...]
powershell -ExecutionPolicy Bypass -File tools\aspect.ps1 <W:H> [-RenderHeight 1920] [-GameDir ...]
```

`wrapper.ps1 enable` copies `MS\x86\DDraw.dll` and `MS\x86\D3DImm.dll` from `-DgVoodooDir` (default `D:\Games\STCC_work\dgVoodoo2`)
plus `tools\dgvoodoo\dgVoodoo.conf` into the game folder; `disable` removes them, for comparing against the unwrapped game.

Local paths assumed by the scripts (override with parameters): game `D:\Games\STCC`, work area `D:\Games\STCC_work`,
Ghidra `D:\Tools\ghidra_12.1.3_PUBLIC`.

## Architecture

### Loading

`STCC.EXE` imports `DINPUT.dll` statically, so Windows loads `dinput.dll` from the game folder before the system copy.
`DllMain` therefore runs before the game's `WinMain`:

1. Open `stccfix.log` (overwritten on every start) and read `stccfix.ini` (`config.cpp`).
2. Optionally declare Per-Monitor V2 DPI awareness (`DpiAware`), before the game creates its window.
3. If widescreen or `LogGraphics` is enabled, replace the exe's IAT entry for `DDRAW!DirectDrawCreate` (version independent).
4. Detect the game version. If it is not jp-1.02, stop here: only DirectInput forwarding stays active.
5. Apply the byte patches and install the MinHook game/window/HUD hooks for jp-1.02.

`DllMain` never calls `LoadLibrary` (loader lock). The real `dinput.dll` is loaded from `GetSystemDirectory()`
(redirected to SysWOW64 under WOW64) on the first call through `InitOnceExecuteOnce`. `DirectInputCreateA` is the
point where the DirectInput hooks are installed on the returned interface.

Why `dinput.dll` and not `ddraw.dll`: dgVoodoo2 provides `DDraw.dll`, and input processing lives in the same DLL.

### Version check

`patch.cpp` compares the PE header of the running exe: `TimeDateStamp == 0x3533084F` and `SizeOfImage == 0x00EC0000` → jp-1.02.
This avoids hashing a 1 MB file inside `DllMain`. The SHA-256 values in `data/addresses/versions.toml` are the
reference for tools (`testpatch.py` checks them) and for users. The result is logged as `game version: jp-1.02` / `unknown`.

### Byte patches

A patch is `{va, original bytes, replacement bytes, description}`. `ApplyPatches()` first compares **every** original
byte sequence and writes nothing if any of them differs; then it writes with `VirtualProtect` and
`FlushInstructionCache` and logs each patch.

| Address | Setting | Change |
|---|---|---|
| `0x0043964D` | `Windowed` | `InitInstance`: initialize `g_bFullscreen` to 0 (start windowed) |
| `0x00436548` | `D3DWindowedVideoMemory` | `Gfx_CreateRenderSurface`: create the windowed Direct3D render target with `VIDEOMEMORY | 3DDEVICE` instead of `SYSTEMMEMORY`. Without it `CreateDevice(HAL)` fails with `D3DERR_SURFACENOTINVIDMEM`. |

### Hooks

| Target | Mechanism | Purpose |
|---|---|---|
| DirectDraw / Direct3D (`IDirectDraw2`, surfaces, `IDirect3D2`, `IDirect3DDevice2`, viewports) | IAT hook of `DirectDrawCreate`, then vtable patching of each COM object created | Widescreen surface/vertex/viewport/blit adjustments, call logging |
| DirectInput 5 (`IDirectInputA`, `IDirectInputDevice2A`, effects) | vtable hooks on the interface returned by `DirectInputCreateA` | `DeviceName` filter and subtype override in `EnumDevices`; hiding `DIDC_FORCEFEEDBACK`; axis remapping in `GetDeviceState`; force feedback scaling and continuous playback |
| Game functions (`Gfx_InitDirectDraw` 0x435390, `Spr_RenderQueue` 0x46D020, D3D init functions) | MinHook detours (naked for argument-less functions) | Borderless window before DirectDraw init, HUD anchoring, init tracing |
| user32 (`SetMenu`, `SetWindowPos`, `MoveWindow`) and the game window's messages | MinHook | Borderless fullscreen (hide menu), window size logging, `WM_WINDOWPOSCHANGING` handling |

Notes:

- Watch the `IDirect3DDevice2` vtable order (after `GetCaps` come `SwapTextureHandles`, `GetStats`, `AddViewport`,
  `DeleteViewport`, `NextViewport`). Wrong indices crashed the game at startup.
- The game recreates its Direct3D viewport every frame (80–110 `CreateViewport` calls per second). Keep per-call logging
  out of hot paths; per-line `FlushFileBuffers` once caused visible stutter.

### Window handling (`hooks_window.cpp`)

- The game sizes its window to 640x480 (menus) or the Screen Mode size (races) with `SetWindowPos` from `0x436750`
  whenever the mode changes. stccfix replaces those requests in `WM_WINDOWPOSCHANGING` with the scaled size and nudges
  the window 300 ms later so the game gets a fresh `WM_SIZE`.
- `WindowScale=0` picks the largest integer multiple that fits the work area; dragging is locked to `AspectRatio`;
  the user's size is remembered.
- Borderless: dgVoodoo decides its presentation target from the window shape at `SetCooperativeLevel`, so the frame is
  removed before `Gfx_InitDirectDraw` runs. The game's 640x480 requests are pinned to the monitor rectangle and the
  MFC `SetMenu` call (from 0x43C530) is held back. Toggling at runtime does not work: dgVoodoo follows a shrinking
  window but not an enlarged one.

### Input (`hooks_dinput.cpp`)

- The game uses `DINPUT_VERSION 0x500`, enumerates joysticks only and uses at most two.
- F5 Device Settings offers modes based on the `DIDEVTYPE` subtype: GAMEPAD (4) → "Game Pad", WHEEL (6) → "Steering Wheel",
  anything else → "Joystick". `DeviceType` rewrites the subtype in the enumeration callback.
- Joystick/Wheel modes read a single Y axis (up = accelerate). `TriggerPedals` synthesizes Y from two `DIJOYSTATE` axes;
  on devices whose real subtype is a wheel, the original Y is never passed through (on the CSL DD, Y is the clutch).
- Force feedback: the game creates `AUTOCENTER` and `ConstantForce` effects, but only updates the force for its known
  device codes 3/8 (`FF_SetConstantForceA` 0x468220 from the car physics, `FF_SetConstantForceB` 0x468290 for impacts).
  `Mode=off` (default) hides `DIDC_FORCEFEEDBACK` in `GetCapabilities`, so no effects are created.
  `Mode=game` renames devices that have an FF driver to "DIforce2 Serial Joystick Device" (code 8, prefix match;
  F5 offers it as "Per4mer Racing Wheel", while code 3 would be a joystick). The game's force arrives as integer units
  x40000, irregularly (a few to ~30 updates per second, sometimes none for seconds). stccfix treats it as a target:
  units/100 through `Curve` and `Gain`, capped by `MaxForce`, dropped to 0 after `HoldMs`, slew-limited by `Smoothing`,
  faded in by `FadeInMs` after a pause, and sent every frame from `GetDeviceState` as one infinite-duration constant
  force (the game's own 30 ms effect gapped at ~30 fps). Autocentre is off unless `AutoCenter=1`.
  The `DeviceName` filter runs on the original product name, before the rename. **Experimental and not satisfying yet**:
  on a CSL DD the force is mostly felt in collisions and very fast corners, and qualifying starts with a kick.
  A telemetry-based force (speed, lateral G read from memory) is the likely next step.
- The game never re-enumerates after `DIERR_INPUTLOST` / `DIERR_UNPLUGGED`.

## How widescreen works

Findings that shaped the design:

- The game projects everything itself. 3D and 2D are sent as `D3DTLVERTEX` quads in 640x480 screen space via
  `IDirect3DDevice2::DrawPrimitive`. No `SetTransform`, no Z buffer (sz is always 0). Large off-screen polygons have x
  values of tens of thousands and are clipped by Direct3D.
- There are no 4/3, 640 or 480 float constants in the exe (Saturn-derived fixed-point code).
- The HUD is not Direct3D: it is software sprites (`Spr_Enqueue` 0x421D10 → `Spr_RenderQueue` 0x46D020) written into
  the render surface while the game holds it locked (`Gfx_LockRenderSurface` 0x4367C0 / `Gfx_UnlockRenderSurface` 0x436890).

Approach: **a wider render surface** (Hor+).

1. **Surface.** In `IDirectDraw2::CreateSurface`, a non-primary `3DDEVICE` surface of 640x480 or 320x240 is created
   `height × W/H` wide. `offset = (wideWidth − baseWidth) / 2`. 16:9 → 854x480, offset 107; 32:9 → 1706x480.
2. **2D.** `Lock` returns `lpSurface + offset` and a width of 640, so the game's sprite writes land in the centre 4:3
   area, unstretched. On frames without 3D, the side margins are cleared to black.
3. **3D.** `DrawPrimitive` shifts each TL vertex x by `+offset` (no scaling). Full-width polygons (x exactly 0..640,
   such as the sky) are stretched edge to edge; with `WideBackground=extend` their u coordinates are widened too.
4. **Viewport/Clear/Blt.** Full-screen `SetViewport2` and `Clear` rectangles are widened. Partial viewports, such as the
   rear-view mirror, are shifted by `offset`. The present `Blt` copies the full wide width. `Blt` onto the render
   surface is shifted by `offset`, and so is `BltFast` x.
5. **Presentation.** The window uses the same ratio; dgVoodoo scales the wide back buffer to `Resolution`.

HUD anchoring (`hooks_hud.cpp`, `HudAnchor=edges`):

- Active only when the race-screen flag (0x56C190) is set and 3D was drawn before the lock.
- Queued sprites are grouped into clusters: same row (vertical overlap of more than half) and horizontally close.
  Clusters on the left move to the left edge, clusters on the right move to the right edge.
- A frame containing a full-width sprite (divider lines or bands, as on the pre-race grid screen) is a layout screen
  and stays centred.
- The sprite renderer writes at `lpSurface + x*bpp + y*lPitch` and clips only against `g_SprScreenW/H`
  (0x10E4C40/44). During the hooked call stccfix temporarily passes the start of the wide surface and the wide width.

Known gaps: culling and LOD still assume a 4:3 frustum (pop-in at the edges); menus stay 4:3.
An earlier anamorphic approach (scale x by (4/3)/(W/H), let dgVoodoo stretch) was dropped because the HUD stretched.

## Address database

`data/addresses/` is the single source of truth for reverse engineering findings. The Ghidra project is **not** in git.

- `versions.toml`: one `[[version]]` per known exe (`id`, `label`, `sha256`, `size`, `linked`, `supported`).
  Only `jp-1.02` is `supported = true`.
- `stcc_<version>.toml` (currently `stcc_jp-1.02.toml`):
  ```toml
  [meta]
  version = "jp-1.02"
  image_base = 0x00400000

  [[function]]
  name = "Gfx_InitDirectDraw"      # usable as a C identifier
  addr = 0x00435390
  confidence = "confirmed"         # confirmed | likely | guess
  note = "..."

  [[global]]
  name = "g_hWndMain"
  addr = 0x0056C184
  type = "HWND"
  confidence = "confirmed"
  ```
  The exe has no `.reloc` section and loads at 0x00400000, so all addresses are absolute.

**Rule:** every address you find, or use in code, is written to the TOML with its meaning and confidence before (or together
with) the code change. Code comments refer to the TOML names. The TOML is intended as the source for generated headers and
Ghidra label import/export.
<!-- TODO: no header/label generator exists in the repository yet; confirm wording -->

## Diagnostics

### stccfix.log

Written next to `dinput.dll` and overwritten on every start. It always contains the build date, the parsed config,
the exe header values, the detected version, each patch and each hook status. Extra switches in `[Debug]`:

| Switch | Logs | Cost |
|---|---|---|
| `LogGraphics=1` | DirectDraw/Direct3D calls, surface descriptions, the game's D3D init return values (`hooks_game.cpp`), per-second draw and lock statistics | High, large logs |
| `LogInput=1` | Enumerated devices (and whether they were hidden/overridden), `SetProperty` ranges, `GetDeviceState` failures, state changes (post-remap values, at most every 50 ms) | Low |
| `LogWindow=1` | `SetWindowPos` / `MoveWindow` callers, `WM_SIZE` | Low |

### joylog

A stand-alone controller logger built alongside the DLL, independent of the game:

```powershell
build\Release\joylog.exe [seconds=600] [output=joylog.txt] [name-part[#N]]
```

It opens one controller with DirectInput 8 (non-exclusive, background), logs axis/button/POV changes, detects
disconnects and times reconnects, and every 10 s records the connection state and whether `steam.exe` is running.
**Axis layout differs from the game's DirectInput 5 view** (for example the Fanatec accelerator is Y in joylog and Z in
the game). Use `stccfix.log` with `LogInput=1` for mappings.

### Ghidra headless

One-time setup: import and analyse `STCC.EXE` into the project `D:\Games\STCC_work\ghidra\STCC`.
<!-- TODO: document the exact analyzeHeadless import command used -->

```powershell
powershell -ExecutionPolicy Bypass -File tools\ghidra\run-headless.ps1 <Script.java> [args...] `
    [-GhidraHome D:\Tools\ghidra_12.1.3_PUBLIC] [-ProjectDir D:\Games\STCC_work\ghidra] [-ProjectName STCC] [-Program STCC.EXE]
```

The wrapper runs read-only with `-noanalysis`, sets `JAVA_HOME` from an installed Temurin JDK if unset, and prints only the script output.

| Script | Arguments | Output |
|---|---|---|
| `FindImportCallers.java` | import names (default: DirectInputCreateA, DirectDrawCreate, DirectDrawEnumerateA, DirectSoundCreate, mciSendCommandA, timeGetTime) | Callers, including through thunks |
| `DecompileFunctions.java` | hex addresses | Decompiled functions containing each address. Missing functions (reached only through pointer tables) are created in memory, not saved. |
| `FindDataRefs.java` | hex addresses | Read/write references to globals, with the containing function and instruction |

### Other tools

| Tool | Use |
|---|---|
| `tools\exe_probe.py <STCC.EXE>...` | PE info, `.reloc` presence, embedded DirectX GUIDs, `DINPUT_VERSION`, candidate aspect/resolution constants |
| `tools\testpatch.py list` / `apply <name> [--src] [--out]` | Apply research patches to a **copy** of the exe (checks the v1.02 SHA-256). Never the final mechanism. |
| `tools\stcc-inventory.ps1 -Source <dir> -Name <n>` / `-Compare a,b` | File inventory (tree, extensions, PE info, SHA-256 CSV) into `inventory\` (git-ignored), and diffs |

## Rules and conventions

- **No hex-edited exe as a deliverable.** Experiments on exe copies are fine; confirmed changes move into the DLL as verified
  byte patches or hooks.
- **Record findings in `data/addresses/*.toml`.**
- **Keep `stccfix.ini` and `dgVoodoo.conf` ASCII-only.** The Win32 profile API and dgVoodoo read them as ANSI.
- New ini keys: add them to `Config` (`common.h`), `LoadConfig` (`config.cpp`), the startup config log line (`dllmain.cpp`),
  the template `stccfix.ini` with a comment, and the settings table in both READMEs.
- Game-specific addresses are only used after the version check succeeded.
- Do not add per-call logging to per-frame paths without throttling.

## Release packaging checklist

### Zip contents

| File | Source |
|---|---|
| `dinput.dll` | `build\Release\dinput.dll` (Win32, Release) |
| `stccfix.ini` | `src\stccfix\stccfix.ini` |
| `dgVoodoo.conf` | `tools\dgvoodoo\dgVoodoo.conf` |
| `LICENSE` | Repository root (MIT requires the notice to accompany copies) |
| `THIRD_PARTY_NOTICES.md` | Repository root (BSD-2-Clause requires the MinHook notice in binary distributions) |
| `README.md`, `README.ja.md` | Repository root |

`tools\package.ps1 -Version <x.y.z>` builds Release and writes `dist\stcc-pc-tweak-<x.y.z>.zip` with exactly these files,
then prints the SHA-256.

### Never include

- `STCC.EXE` or any other game executable, original or modified, any version
- Disc images or tracks (`.bin`, `.cue`, `.iso`, `.img`, `.flac`, `.wav`, ...)
- Any game data or generated game files (`Data\`, `*.BIN`, `*.MRG`, `*.BMP`, replays, `STCCD3D.DAT`, `stcc.ini`)
- dgVoodoo2 files (`DDraw.dll`, `D3DImm.dll`, the dgVoodoo2 archive); users download dgVoodoo2 themselves
- `stccfix.log`, `joylog.txt`, crash dumps (they contain local paths and device names)
- `inventory\` output, Ghidra projects, decompiler output
- `dinput.pdb` (contains local build paths)

### Steps

1. Commit everything and tag the release as `vMAJOR.MINOR.PATCH` (pre-releases while the project is a work in progress).
2. Clean build: delete `build\`, run `tools\package.ps1 -Version <x.y.z>`.
3. Check the template `stccfix.ini`: all `[Debug]` switches are 0 and `AspectRatio` matches `Resolution` in `dgVoodoo.conf`
   (release defaults: `16:9` / `3413x1920`).
4. Test the zip on a clean game folder (v1.02 + dgVoodoo2 + zip only): windowed start, Display > Direct 3D, a race in
   widescreen, controller selection in F5, normal exit. Check `stccfix.log` for `game version: jp-1.02` and no patch/hook failures.
5. Build the zip from the list above only, and look inside it before uploading.
6. Publish the SHA-256 of `dinput.dll` and the zip, and the tested dgVoodoo2 version, in the release notes.

## Not implemented yet

From the project plan in `HANDOFF.md`; none of this exists in the code today.

- CD-less play: hook `mciSendCommandA` and the disc check, play FLAC audio from files
- Automatic controller reconnection after a disconnect
- Frame limiter inside the DLL (currently dgVoodoo `FPSLimit = 60`)
- Redirecting `C:\WINDOWS\stcc.ini` writes (VirtualStore)
- Wider culling frustum for widescreen
- Force feedback computed from telemetry (speed, lateral G) instead of the game's own force (`Mode=game` is an unfinished experiment)
- Steering gain/rotation scaling in the DLL
- Loading `plugins\*.asi`
- Free camera in replays
