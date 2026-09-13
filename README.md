# stcc-pc-tweak

English | [日本語](README.ja.md)

> **Work in progress.** This is an early preview (v0.1.0 pre-release). Settings and behaviour may still change, and
> issues and pull requests are limited for now.

Download: [Releases](https://github.com/taro-youngdisciples/stcc-pc-tweak/releases)

A compatibility and enhancement mod for the 1998 Windows version of **Sega Touring Car Championship** (STCC).
It makes the original game run on Windows 11 and adds widescreen, borderless fullscreen and modern controller and
steering wheel support.

The mod is **stccfix**, a `dinput.dll` proxy. The game loads it from its own folder at startup; stccfix then patches the game
in memory and hooks DirectDraw, Direct3D and DirectInput. The game executable on disk is never modified.
It is designed to be used together with [dgVoodoo2](#4-install-dgvoodoo2).

> **You need your own original disc.** This project does not contain or distribute the game, disc images, game data,
> a modified executable, or dgVoodoo2.

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [First launch and recommended settings](#first-launch-and-recommended-settings)
- [Controllers and steering wheels](#controllers-and-steering-wheels)
- [Settings reference](#settings-reference)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [License and legal](#license-and-legal)

## Features

| Area | What stccfix does |
|---|---|
| Windows 11 | Starts in a window with the game's menu bar. Direct3D mode works in a window (the original game cannot do this). The window is enlarged to the largest size that fits the monitor and keeps its size when the game switches modes. |
| Widescreen | Any ratio such as 16:9, 21:9 or 32:9. The 3D view fills the whole width (more is visible at the sides); the 2D HUD is not stretched. During races the HUD groups are moved to the left and right screen edges. The sky is extended rather than stretched. The rear-view mirror in the bumper camera is placed correctly. |
| Fullscreen | Borderless fullscreen (applied at startup) and DPI awareness for Windows display scaling. |
| Analog controllers | Unlocks the game's analog "Joystick" and "Steering Wheel" modes for modern pads. Analog triggers can be used as accelerator and brake. Steering deadzone and response curve. |
| Steering wheels | Choose which device the game sees, map separate pedal axes, invert and deadzone. Force feedback is off by default; an experimental game-driven mode exists but is not finished. |
| Resolution | With dgVoodoo2, Direct3D renders the 3D scene at a high resolution such as 3413x1920. |

Tested with: a DualShock 4-compatible pad (USB and Bluetooth), Fanatec CSL DD with pedals,
16:9 and 32:9 displays/windows, dgVoodoo2 2.87.4.
<!-- TODO: 21:9 has not been confirmed in-game yet (16:9 and 32:9 were). Update this line after testing. -->

## Requirements

| Item | Details |
|---|---|
| Game | **Japanese Windows version, v1.02.** This is the only supported executable. The disc installs v1.00; the v1.02 update is on the same disc. |
| Disc image | A 1:1 image of your disc **including the audio tracks** (for example BIN/CUE), mounted as a virtual CD-ROM drive. See below. |
| Windows | Windows 11 with the **DirectPlay** feature enabled. <!-- TODO: Windows 10 untested --> |
| dgVoodoo2 | Tested with version 2.87.4. Download it yourself from the official site. |
| DirectX | Nothing to install. Do not install the DirectX runtime from the disc. |

### Why a mounted disc image is required

At startup (and again later) the game looks for a drive of type CD-ROM that contains `\stcc\stcc.exe` and
`\stcc\data\bg\sky.bmp`. If there is none, it stops with a "Please insert the CD" message. The drive letter and volume
label do not matter. The background music is played as CD audio (CD-DA) from that drive, so the image must contain the
audio tracks as well. An image with only the data track may start, but you will have no music.

Use a virtual drive tool that can mount BIN/CUE images with audio tracks. The built-in Windows "Mount" command only
handles ISO/VHD files and cannot play audio tracks. ImgDrive was used during development.

### Checking your game version

1. In PowerShell, in the game folder:
   ```powershell
   Get-FileHash .\STCC.EXE -Algorithm SHA256
   ```
   | Version | SHA-256 | Size |
   |---|---|---|
   | v1.02 (supported) | `6B4E92CA0A4C156363435A3425DA5704B2E64E9EF2218CF51449F9AA42C4319D` | 1,009,152 bytes |
   | v1.00 (disc `Stcc\STCC.EXE`, not supported) | `897E9AE4F33B24068EC5E207B2C6D42BC5DF2CCB28F0A0D1EF4B22852817C6CA` | 946,688 bytes |
2. After starting the game once with stccfix installed, open `stccfix.log` in the game folder.
   It must contain `game version: jp-1.02`. If it says `unknown`, stccfix does not apply its game patches.

## Installation

The examples use `D:\Games\STCC` as the game folder. Any folder works.
<!-- TODO: confirm whether installing under C:\Program Files causes problems (write access / VirtualStore); the project only tested D:\Games\STCC -->

### 1. Enable DirectPlay

Press Win+R, run `optionalfeatures`, expand **Legacy Components** and tick **DirectPlay**.
Without it Windows shows a "this app needs DirectPlay" dialog when the game starts (the executable links to DirectPlay).

### 2. Install the game

1. Mount your disc image.
2. Run `Setup.exe` from the disc and install the game, for example to `D:\Games\STCC`.
   Skip the DirectX installation. Setup stores the install paths in `C:\WINDOWS\stcc.ini`.
   <!-- TODO: confirm the location of Setup.exe on the disc and whether Setup offers to install DirectX -->

### 3. Update the game to v1.02

1. On the disc, `D3D\updatej.exe` is a self-extracting archive (WinZip) that contains the v1.02 `STCC.EXE`.
2. Extract it to a temporary folder.
3. Back up the installed `STCC.EXE`, then copy the extracted `STCC.EXE` over it.
4. Check the SHA-256 as shown above.

<!-- TODO: confirm whether running the included PatchInstaller.exe works on Windows 11; the project copied STCC.EXE manually -->

### 4. Install dgVoodoo2

1. Download dgVoodoo2 from its official sources: <https://dege.freeweb.hu/> or <https://github.com/dege-diosg/dgVoodoo2/releases>.
2. From the dgVoodoo2 archive, copy `MS\x86\DDraw.dll` and `MS\x86\D3DImm.dll` into the game folder.
3. Copy **`dgVoodoo.conf` from the stccfix release** into the game folder (not the one shipped with dgVoodoo2).
   It is set up for this game: windowed, 16bpp desktop, GDI hook for the menu bar and dialogs, 60 fps limit,
   fixed render resolution.

Some antivirus products, including Windows Defender, have flagged the dgVoodoo2 archive. See [Troubleshooting](#troubleshooting).

### 5. Install stccfix

Copy `dinput.dll` and `stccfix.ini` from the release zip into the game folder.

The game folder should now contain:

| File | Source |
|---|---|
| `STCC.EXE` (v1.02) and the other game files | Your disc |
| `DDraw.dll`, `D3DImm.dll` | dgVoodoo2 |
| `dgVoodoo.conf` | stccfix release |
| `dinput.dll`, `stccfix.ini` | stccfix release |
| `stccfix.log` | Created by stccfix on every start |

### 6. Set the aspect ratio

Two settings must use the same ratio:

- `stccfix.ini`, section `[Display]`: `AspectRatio=16:9`
- `dgVoodoo.conf`, section `[DirectX]`: `Resolution = 3413x1920`

The release files are preset to 16:9. The dgVoodoo `Resolution` is the render size: a height of your choice multiplied
by the ratio. Use about the height of your window, or the monitor height for borderless fullscreen.

| Ratio | `AspectRatio` | `Resolution` (height 1920, e.g. 4K window) | `Resolution` (height 1440) |
|---|---|---|---|
| Original | `4:3` | `2560x1920` | `1920x1440` |
| 16:9 | `16:9` | `3413x1920` | `2560x1440` |
| 21:9 | `21:9` | `4480x1920` | `3360x1440` |
| 32:9 | `32:9` | `6827x1920` | `5120x1440` |

`AspectRatio` accepts any `W:H` of whole numbers, so you can also enter a monitor's exact ratio (for example `64:27` for 2560x1080).
<!-- TODO: exact ratios like 64:27 follow from the code but are untested -->

**Manual:** edit both lines in a text editor. Keep `stccfix.ini` plain ASCII (no Japanese or other non-ASCII characters),
because Windows reads it as ANSI.

**Script (from the repository):** `tools\aspect.ps1` changes both files at once:

```powershell
powershell -ExecutionPolicy Bypass -File tools\aspect.ps1 21:9 -GameDir "D:\Games\STCC"
powershell -ExecutionPolicy Bypass -File tools\aspect.ps1 32:9 -RenderHeight 1440 -GameDir "D:\Games\STCC"
```

`-RenderHeight` defaults to 1920 and `-GameDir` defaults to `D:\Games\STCC`.
<!-- TODO: decide whether aspect.ps1 should also be shipped in the release zip -->

### 7. Optional: borderless fullscreen

In `stccfix.ini`:

```ini
[Display]
Windowed=1
BorderlessFullscreen=1
DpiAware=1
AspectRatio=32:9
```

- Set `AspectRatio` to your monitor's ratio and the dgVoodoo `Resolution` to the monitor resolution.
  Example for a 5120x1440 monitor: `tools\aspect.ps1 32:9 -RenderHeight 1440`.
- `Windowed=1` is required: borderless fullscreen is a borderless window.
- `DpiAware=1` is needed when Windows display scaling is not 100%.
- The menu bar is hidden. F-keys and Alt+F4 still work. Set `BorderlessFullscreen=0` when you need the menus.
- The setting is read at startup only. Switching while the game is running is not supported.

## First launch and recommended settings

1. Start `STCC.EXE`. A window with a menu bar opens.
2. **Display > Direct 3D.** DirectDraw mode is software rendering that is only scaled up, and widescreen needs Direct3D.
3. **Settings > Screen Mode > 640x480 16bit.** 320x240 looks clearly blurrier.
4. **F5** (Device Settings): select your controller, see [Controllers and steering wheels](#controllers-and-steering-wheels).
5. Quit with **Game > Exit** or **Alt+F4**. The Direct3D choice is saved when the game exits normally.

<!-- TODO: confirm the exact menu labels as they appear in the Japanese version -->

| Key | Function |
|---|---|
| F3 | Toggle pause / menu mode (shows the mouse cursor and menus) |
| Esc | Leave menu mode |
| F5 | Device Settings (controller selection) |
| F5-F9 | Game settings dialogs <!-- TODO: list what F6, F7, F8 and F9 open --> |
| Alt+F4 | Quit |

## Controllers and steering wheels

How the game handles controllers:

- It uses up to two controllers. Each player's controller type is chosen in **F5 Device Settings**.
- The modes offered in F5 depend on the device type Windows reports. A modern pad is reported as a gamepad, which only
  unlocks "Game Pad" with digital accelerator and brake. `[Input] DeviceType` changes what the game sees.
- In "Joystick" and "Steering Wheel" mode the game reads a single Y axis: up = accelerate, down = brake.
  `[Input] TriggerPedals=1` builds that axis from two separate axes (triggers or pedals).

Copy a block below into `stccfix.ini`, replacing the existing keys of the same name.

### DualShock 4 / PS4-compatible pad

```ini
[Input]
DeviceType=joystick
DeviceName=
TriggerPedals=1
AccelAxis=Ry
BrakeAxis=Rx
AccelInvert=0
BrakeInvert=0
PedalDeadzone=5
SteerDeadzone=0
SteerLinearity=100
```

Then in the game: **F5 > Player 1 > Joystick > Next**. Keep the default assignment
(accelerator = Stick Up, brake = Stick Down). Shift, confirm, cancel, view and start are button assignments in the
same dialog.

Result: left stick = steering, R2 = accelerator, L2 = brake (both analog). When neither trigger is pressed, the left
stick's up/down still works as accelerator/brake.

- Steering drifts at centre: try `SteerDeadzone=3`. Steering too twitchy near centre: try `SteerLinearity=150`.
- If another controller (for example a wheel) is also connected, set `DeviceName` to part of the pad's name so only the
  pad is shown to the game. The name is listed in `stccfix.log` with `LogInput=1`.
  <!-- TODO: add the product name a DS4 reports in DirectInput -->

### Fanatec CSL DD with pedals

```ini
[Input]
DeviceType=auto
DeviceName=FANATEC#1
TriggerPedals=1
AccelAxis=Z
BrakeAxis=Rz
AccelInvert=1
BrakeInvert=1
PedalDeadzone=2
SteerDeadzone=0
SteerLinearity=100

[ForceFeedback]
Mode=off
```

Then in the game: **F5 > Player 1 > Steering Wheel (T2) > Next**.

- In FanatecApp set the wheel rotation (SEN) to **about 240 degrees**. At 1080 degrees the game only uses a small part
  of the wheel's travel.
- Windows lists two "FANATEC Wheel" devices; the second one never moves. `DeviceName=FANATEC#1` shows only the first
  one to the game and also hides any other controllers.
- As seen by the game, the pedals are clutch = Y, accelerator = Z, brake = Rz, and all read maximum when released,
  hence `AccelInvert=1` and `BrakeInvert=1`.
- `Mode=off` (the default) hides force feedback from the game. With `Mode=native` the game only plays its initial effect
  on modern wheels, which causes odd jolts at the start of a race. Set the centring spring/damper you like in FanatecApp.

Tested with Fanatec driver 0.53.1 and FanatecApp. <!-- TODO: confirm whether the pedals were connected through the base or by USB -->

Other wheels are untested. If Windows reports the device as a wheel, `DeviceType=auto` offers "Steering Wheel (T2)";
otherwise try `DeviceType=wheel`. Find the pedal axes as described in [Finding your axes](#finding-your-axes).

### Force feedback (experimental, unfinished)

**Not recommended yet.** With the Fanatec CSL DD the force was mostly felt in collisions and very fast corners, and
qualifying sessions can start with a sudden kick. Keep `Mode=off` unless you want to experiment.

```ini
[ForceFeedback]
Mode=game
MaxForce=40
Gain=100
Curve=60
Invert=0
AutoCenter=0
HoldMs=100
Smoothing=80
FadeInMs=1500
```

With `Mode=game`, stccfix presents a force feedback wheel to the game as a 1998 force feedback device it supports
("DIforce2 Serial Joystick Device"), so the game drives its own steering force. stccfix maps the game's force with
`Curve` and `Gain`, caps it at `MaxForce`, releases forces the game stopped updating (`HoldMs`), limits how fast it
changes (`Smoothing`) and fades forces back in after a pause (`FadeInMs`).

- In the game, open **F5 > Player 1 > Per4mer Racing Wheel > Next** and redo the pedal calibration. When you go back
  to `Mode=off`, select **Steering Wheel (T2)** again.
- **Hold the wheel on the first run.** Direct drive wheels can be very strong; start with a low `MaxForce`.
- If the wheel pulls further into a turn instead of back towards centre, set `Invert=1`.
- `AutoCenter=1` lets the game enable the wheel's built-in centring spring (only relevant with `Mode=game`).

### Finding your axes

For any other controller:

1. In `stccfix.ini` set `[Debug] LogInput=1`. Temporarily set `TriggerPedals=0`, `SteerDeadzone=0` and
   `SteerLinearity=100`, because the log shows the values after stccfix has processed them.
2. Start the game, press **F5**, select a non-keyboard mode for your controller and close the dialog.
   The game only reads the controller after it has been selected.
3. Move one axis at a time to its limits (steering, then each pedal or trigger), with a short pause in between.
4. Quit the game and open `stccfix.log` in the game folder:
   - `device ... product="..." -> cb=1` lines list the controllers the game was shown
     (`-> hidden (DeviceName)` = filtered out).
   - `state X=... Y=... Z=... Rx=... Ry=... Rz=... S0=... S1=... POV=... btn=0x...` lines show the values as they
     change. The axis that moves with your pedal is the one to use. If it is high when released, set the matching
     `...Invert=1`.
5. Enter the names in `AccelAxis` / `BrakeAxis` (`X`, `Y`, `Z`, `Rx`, `Ry`, `Rz`, `Slider0` for S0, `Slider1` for S1),
   set `TriggerPedals=1` again and set `LogInput=0`.

`stccfix.log` is overwritten every time the game starts. Copy it before starting again.

Diagnostic tools that use DirectInput 8 (including this project's `joylog`) can show a different axis layout from the
one the game sees through DirectInput 5. For example, the Fanatec accelerator appears as Y in DirectInput 8 but as Z in
the game. **Always trust `stccfix.log`.**

## Settings reference

All settings are in `stccfix.ini` next to `dinput.dll`. Keep the file ASCII-only.
"Release" is the value in the shipped `stccfix.ini`; "If missing" is what stccfix uses when the key or the whole file is absent.

### [Display]

| Key | Release | If missing | Values | Meaning |
|---|---|---|---|---|
| `Windowed` | `1` | `1` | 0 / 1 | 1 = start in a window with the menu bar. 0 = original fullscreen start; window sizing, borderless fullscreen and HUD anchoring are then unavailable. |
| `D3DWindowedVideoMemory` | `1` | `1` | 0 / 1 | 1 = create the windowed Direct3D render target in video memory. Without it, Display > Direct 3D fails in a window. |
| `WindowScale` | `0` | `0` | 0, 1, 2, ... | Window size as a multiple of 640x480 (widened to `AspectRatio`). 0 = largest that fits the monitor. |
| `KeepAspect` | `1` | `1` | 0 / 1 | 1 = keep `AspectRatio` while resizing the window by dragging. |
| `AspectRatio` | `16:9` | `4:3` | `W:H` | Display aspect ratio. Anything other than 4:3 enables widescreen (Direct3D mode, windowed or borderless). Set dgVoodoo `Resolution` to the same ratio. |
| `WideBackground` | `extend` | `extend` | `extend` / `stretch` | Full-width backgrounds (sky) in widescreen. `extend` shows more of the texture; `stretch` stretches the 4:3 background. |
| `HudAnchor` | `edges` | `edges` | `edges` / `center` | Race HUD in widescreen. `edges` moves left-side HUD groups to the left edge and right-side groups to the right edge; `center` keeps the HUD in the centre 4:3 area. Menus are always centred. |
| `RememberWindowSize` | `1` | `1` | 0 / 1 | 1 = keep the window size you dragged to when the game re-initializes graphics. |
| `BorderlessFullscreen` | `0` | `0` | 0 / 1 | 1 = borderless window covering the whole monitor, no menu bar. Startup only. Needs `Windowed=1`. Set dgVoodoo `Resolution` to the monitor size. |
| `DpiAware` | `0` | `0` | 0 / 1 | 1 = make the game DPI aware. Use it when Windows display scaling is not 100% (otherwise the window is blurred). |

### [Input]

| Key | Release | If missing | Values | Meaning |
|---|---|---|---|---|
| `DeviceType` | `joystick` | `auto` | `auto` / `joystick` / `wheel` / `gamepad` | Device type reported to the game; decides which modes F5 offers. `auto` = as reported by Windows, `joystick` unlocks "Joystick", `wheel` unlocks "Steering Wheel (T2)", `gamepad` forces "Game Pad". |
| `DeviceName` | (empty) | (empty) | text, or `text#N` | Only show game controllers whose name contains this text (not case-sensitive). `#N` = only the N-th match. Empty = all. |
| `TriggerPedals` | `1` | `0` | 0 / 1 | 1 = build the game's accelerate/brake Y axis from `AccelAxis` and `BrakeAxis`. When both are released, a pad's own stick Y passes through (not on wheels). |
| `AccelAxis` | `Ry` | `Ry` | `X` `Y` `Z` `Rx` `Ry` `Rz` `Slider0` `Slider1` | Source axis for the accelerator. |
| `BrakeAxis` | `Rx` | `Rx` | same as above | Source axis for the brake. |
| `AccelInvert` | `0` | `0` | 0 / 1 | 1 = the accelerator axis reads maximum when released (typical for wheel pedals). |
| `BrakeInvert` | `0` | `0` | 0 / 1 | Same for the brake. |
| `PedalDeadzone` | `5` | `5` | percent | Travel ignored at the start of each pedal/trigger. |
| `SteerDeadzone` | `0` | `0` | percent | Deadzone at the centre of the steering (X) axis. |
| `SteerLinearity` | `100` | `100` | percent, min. 10 | Steering response curve. 100 = linear, 150 = softer near centre, 200 = much softer. |

### [ForceFeedback]

| Key | Release | If missing | Values | Meaning |
|---|---|---|---|---|
| `Mode` | `off` | `off` | `off` / `native` / `game` | `off` = hide force feedback from the game. `native` = leave it to the game (only works properly on its 1998 devices). `game` = experimental, see [Force feedback](#force-feedback-experimental-unfinished). |
| `MaxForce` | `40` | `40` | 0-100 | Cap in percent of the device's full force (`Mode=game`). |
| `Gain` | `100` | `100` | percent | Share of `MaxForce` reached at a strong in-game force (`Mode=game`). |
| `Curve` | `60` | `60` | percent, 10-300 | Response curve. 100 = linear; lower values strengthen weak low-speed forces (`Mode=game`). |
| `Invert` | `0` | `0` | 0 / 1 | 1 = reverse the force direction (`Mode=game`). |
| `AutoCenter` | `0` | `0` | 0 / 1 | 1 = allow the game to enable the device's built-in centring spring (`Mode=game`). |
| `HoldMs` | `100` | `100` | ms | Release a force the game stopped updating after this time (`Mode=game`). |
| `Smoothing` | `80` | `80` | ms | Time to ramp from 0 to `MaxForce`; 0 = off (`Mode=game`). |
| `FadeInMs` | `1500` | `1500` | ms | Fade forces back in after they stopped for a second; 0 = off (`Mode=game`). |

### [Debug]

All logs go to `stccfix.log` in the game folder. Leave them at 0 unless you are diagnosing a problem; logging can slow the game down and make the log very large.

| Key | Release | If missing | Meaning |
|---|---|---|---|
| `LogGraphics` | `0` | `0` | Log DirectDraw/Direct3D calls and the game's Direct3D initialization results. |
| `LogInput` | `0` | `0` | Log DirectInput devices, axis properties and controller state changes. |
| `LogWindow` | `0` | `0` | Log window size changes. |

## Known limitations

- **Fullscreen is applied at startup only.** Switching between window and borderless fullscreen while the game runs is not supported.
- **Menus, the title screen and other non-race screens stay 4:3 in the centre** with black bars at the sides.
- **Objects can pop in at the screen edges in widescreen**, for example grandstands. The game decides what to draw for a 4:3 view. Slight seams in the road surface may also be visible.
- **HUD and text sprites are low-resolution** 640x480 artwork scaled up.
- **Widescreen needs Direct3D mode** and `Windowed=1`. DirectDraw mode is unchanged 4:3 software rendering.
- **A DualShock 4 over Bluetooth may disconnect during play while Steam is running.** Closing Steam avoided it in testing.
  <!-- TODO: check whether turning off Steam's PlayStation controller support also helps -->
- **The game does not detect a controller again after it disconnects.** Reconnect it, then open F5 and confirm the selection again.
- **Force feedback is not finished.** Use `Mode=off` (the default); `Mode=game` is an experiment.
- Only the Japanese v1.02 executable is supported.
- The mounted disc image is still required.
- The game runs at a 60 fps limit set in `dgVoodoo.conf` (`FPSLimit = 60`) as a guard against it running too fast on high refresh rate displays. Keep that setting.

## Troubleshooting

`stccfix.log` in the game folder (next to `dinput.dll`) is the first place to look. It is recreated on every start and
lists the settings read, the detected game version, the patches applied and the hooks installed.
If there is no `stccfix.log` after starting the game, `dinput.dll` was not loaded: check that it is in the same folder as `STCC.EXE`.

| Problem | Solution |
|---|---|
| "Please insert the CD" | Mount your disc image as a CD-ROM drive. It must contain `\stcc\stcc.exe`. Use an image with audio tracks (BIN/CUE) for music. |
| Windows asks for DirectPlay | Enable DirectPlay, see [Installation step 1](#1-enable-directplay). |
| Solid green screen | `dgVoodoo.conf` must contain `DesktopBitDepth = 16`. Use the `dgVoodoo.conf` from the stccfix release and make sure it is in the game folder next to `DDraw.dll`. |
| Menu bar or F5 dialogs are invisible | Use the release `dgVoodoo.conf` (`SystemHookFlags = gdi`) and `Windowed=1`. With `BorderlessFullscreen=1` the menu bar is hidden on purpose. |
| "Cannot start in Direct3D mode" (Direct3Dモードでは起動できません) | Set `D3DWindowedVideoMemory=1`. If the game now refuses to start every time, move `STCCD3D.DAT` (the saved Direct3D choice) out of the game folder; the game falls back to DirectDraw. <!-- TODO: confirm where STCCD3D.DAT is written --> |
| `stccfix.log` says `game version: unknown` | Update to v1.02 and check the SHA-256, see [Checking your game version](#checking-your-game-version). |
| No widescreen | Select Display > Direct 3D, set `Windowed=1` and an `AspectRatio` other than 4:3. The log shows a `widescreen: render surface` line when it is active. |
| Image stretched or squashed | `AspectRatio` in `stccfix.ini` and `Resolution` in `dgVoodoo.conf` use different ratios. |
| Blocky or blurry picture | Use Display > Direct 3D, Settings > Screen Mode 640x480 16bit, and a large dgVoodoo `Resolution`. With Windows display scaling, set `DpiAware=1`. |
| Only "Keyboard" and "Game Pad" can be selected in F5 | Set `DeviceType=joystick` (pads) or `auto` / `wheel` (wheels). |
| The wrong controller is used, or the wheel shows up twice | Set `DeviceName`, for example `FANATEC#1`. |
| Car accelerates or brakes with the pedals released | Set `AccelInvert=1` / `BrakeInvert=1`, or check the axes with [Finding your axes](#finding-your-axes). |
| Controller stopped working | It disconnected. Reconnect it and open F5 again. |
| Wheel jolts at the start of a race | Set `[ForceFeedback] Mode=off` (the default). |
| Wheel pulls into the turn (`Mode=game`) | Set `Invert=1`. |
| Windows Defender flags the dgVoodoo2 archive | Defender has reported dgVoodoo2 2.87.4 as `Trojan:Win32/Kepavll!rfn`. This is reported to be a false positive, but it has not been confirmed. Download dgVoodoo2 only from its official source; whether to allow the file is your decision. |

During the preview, issues and pull requests are limited. When reporting a problem later, please attach `stccfix.log`.

## Building from source

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## License and legal

- stcc-pc-tweak (stccfix) is released under the [MIT License](LICENSE).
- It includes [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause License). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- dgVoodoo2 is a separate product by its author, is not included, and is subject to its own license.
- **This project contains no part of the game**: no executable (original or modified), disc image, music, graphics or other game data. You must own the original disc.
- The game features real licensed cars and brands. All of them belong to their respective owners, and nothing from the game is included here.
- This project is not affiliated with, endorsed by or sponsored by SEGA. "SEGA" and "Sega Touring Car Championship" are trademarks of their respective owners. All other trademarks belong to their respective owners.
