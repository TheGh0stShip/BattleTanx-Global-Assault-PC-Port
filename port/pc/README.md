# BattleTanx: Global Assault native PC runtime

This directory contains the Windows/Linux runtime for the recovered game. It
uses N64Recomp for the original CPU code, RSPRecomp for the audio microcode,
N64ModernRuntime for N64 services, RT64 for display-list rendering, and SDL2
for audio, keyboard/controller input, and rumble. ROMs and generated translated
code are local build inputs and are never committed.

The current Windows build boots the USA Rev 0 ROM, renders through native
Direct3D 12, plays SDL audio, accepts keyboard and SDL game-controller input,
passes the original menus, and enters playable campaign gameplay. Linux uses
RT64's Vulkan backend. A verified ROM-free Windows x64 package is published on
the repository's Releases page. This remains development software: in-game
save-flow validation, long-session stability, every game mode, and
representative GPU/controller combinations still need broader validation.

## Run a Windows release

1. Download and extract the Windows x64 ZIP from the repository's
   [Releases](https://github.com/TheGh0stShip/BattleTanx-Global-Assault-PC-Port/releases).
2. Supply your own verified USA Rev 0 big-endian `.z64` ROM; it is not included.
3. Double-click **Launch BattleTanx GA.cmd** to choose a ROM, or double-click
   `BattleTanxGA.exe` to reuse the last verified ROM. On the first run the
   executable opens the ROM picker. Dragging the ROM onto either file is also
   supported.

The launcher passes the ROM directly to `BattleTanxGA.exe`; it does not copy,
modify, upload, or bundle it. The runtime validates the ROM before starting and
shows an error dialog for a wrong revision, bad dump, missing file, or startup
failure instead of exiting silently. Unexpected startup failures are appended
to `crash.log` in the per-user data directory, and the dialog displays that
path so it can be included in a support report.

The last verified ROM and up to five recent ROM paths are remembered in
`%LOCALAPPDATA%\BattleTanxGA\settings.ini`; the ROM itself is never copied.
Run `BattleTanxGA.exe --choose-rom` (or use the CMD launcher) whenever you want
to select a different dump. If a remembered file was moved, deleted, or no
longer validates, the native picker opens again automatically.

The F10 overlay's **System** tab lists recently verified ROM paths and lets you
choose which one should be used on the next launch, request the native picker,
open the per-user save/settings directory, or open the project's support page.
ROM changes intentionally take effect after restarting so no live N64 state is
carried between different cartridge images.

For a self-contained USB or removable-drive installation, create an empty
file named `portable.txt` beside `BattleTanxGA.exe` before launching it. The
runtime then stores settings, Controller Pak images, crash logs, and texture
packs under a local `userdata` folder beside the executable instead of using
`%LOCALAPPDATA%`. Removing the marker restores the normal per-user location.
Advanced launchers can set `BTGA_DATA_DIR` to choose an explicit data folder.

## Controls

- WASD: analog stick
- X / Z: N64 A / B
- Left Shift: Z trigger
- Enter: Start
- Q / E: L / R
- Arrow keys: D-pad
- I / J / K / L: C buttons

Up to four SDL game controllers are assigned to N64 ports in discovery order
and use the left stick, face and shoulder buttons, left trigger, D-pad, and
rumble. Keyboard input is combined with the first controller for player one.

Press **F10** in game to open the native settings overlay. It supports keyboard
and controller navigation and exposes window mode and size, internal render
scale, selectable original/match-window/16:9/16:10/21:9/32:9 presentation,
antialiasing, master volume, per-player controller assignment,
deadzones, sensitivity, inversion, rumble strength, complete keyboard and
per-player controller rebinding (including triggers and stick directions),
VSync and presentation frame limiting, and restore defaults. **Apply & Save**
writes the settings atomically under `%LOCALAPPDATA%\BattleTanxGA` and applies
the supported video, audio, and input changes. Master volume scales the final
SDL mix; the music and effects controls independently scale the two master
buses already present in the original libmus audio engine. Mono audio is also
available and safely averages each stereo frame into both output channels.

The aspect-ratio selector drives RT64's projection and extended-origin settings
without changing the game's simulation. This is the renderer foundation for
true widescreen; game-specific HUD placement, split-screen layouts, cutscenes,
menus, and framebuffer effects still require visual validation before the
widescreen milestone is considered complete.

### Optional gameplay mods

The **Mods** tab contains two opt-in changes. Both default to off, so a new
installation retains the original game rules and presentation.

- **Extended visibility** raises the game's projection and world-culling range
  to its safe signed limit and omits the frame's fog geometry mode, fog colour,
  and fog-position commands. Levels may reveal distant geometry that retail
  never intended to draw simultaneously.
- **Independent dual-stick turret aiming** applies only to player-controlled
  vehicles whose tank definition has a rotating turret. The left stick keeps
  the original hull movement, while the right stick rotates the turret. The
  camera requests the game's existing turret-relative transform, also used by
  its presentation code. Existing weapon spawn rules remain in charge, so
  rounds and turret-aimed weapons follow the turret while mines, gun buddies,
  bouncing betties, and other deployables keep their original behavior.

For mouse and keyboard, WASD drives, left click supplies primary fire, and
holding right click captures relative mouse movement for turret aiming. Release
right click to return the cursor. Mouse turret sensitivity is adjustable in the
Mods tab. Open F10 settings before changing windows or
leaving the game; mouse capture is disabled while the settings overlay is open.

### Texture packs

The **Textures** tab discovers RT64 texture packs under the per-user
`texture-packs` directory. On Windows this is
`%LOCALAPPDATA%\BattleTanxGA\texture-packs`; on Linux it is
`$XDG_DATA_HOME/BattleTanxGA/texture-packs` (or
`~/.local/share/BattleTanxGA/texture-packs`). Use **Open texture-packs folder**
to create and open the correct location.

Copy either a packaged `.rtz` file or an unpacked directory containing
`rt64.json` into that folder, select **Rescan now**, and then select
**Apply & Save**. All discovered packs load together in filename order; later
packs take precedence where they replace the same texture. The persistent
**Enable texture packs** switch disables or restores the entire set without
moving files. Packs are never copied into the installation or uploaded.

On Windows, a packaged `.rtz` can also be dragged onto `BattleTanxGA.exe`.
The runtime copies it into the per-user pack folder (updating a pack with the
same filename) and then starts the remembered ROM.

RT64 streams DDS replacements asynchronously. Pack authors should include its
low-mipmap cache to avoid visible pop-in; PNG is accepted for pack development
but is not recommended for distribution.

## Renderer diagnostics

RT64's developer inspector can be enabled for renderer debugging without
changing the normal player configuration. Set `BTGA_RT64_DEVELOPER=1` before
launching, then press F1 in game to open or close the inspector. This is meant
for diagnosing draw calls, materials, textures, and framebuffer state;
ordinary launches leave it disabled.

```powershell
$env:BTGA_RT64_DEVELOPER = '1'
.\BattleTanxGA.exe --play .\battletanx_ga.z64
```

For automated captures where synthesizing F1 is unreliable, set
`BTGA_RT64_INSPECTOR=1` to open the inspector immediately at startup. It also
enables developer mode.

## Windows developer build

Use a native Windows checkout on NTFS and run PowerShell—not the Linux binary
through `wsl.exe`. Install Git for Windows and Visual Studio 2022's **Desktop
development with C++** workload. For a first build, the setup script verifies
your own local ROM, obtains the pinned dependencies, generates all ignored
translation inputs, builds the native executable, and optionally launches it:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
./port/pc/setup-windows.ps1 -Rom 'C:\Games\BattleTanxGA.z64' -Run
```

After setup, use `build-windows.ps1` for fast code-only rebuilds. Neither
script uploads, embeds, or commits the ROM or generated translation output.

The script configures an optimized x64 Ninja build, builds
`BattleTanxGA.exe`, copies SDL2 and the DirectX Shader Compiler runtime beside
it, and optionally launches it with the supplied local ROM. It rejects WSL UNC
source paths because MSVC/Ninja dependency handling is unreliable there.

Create a redistributable, ROM-free ZIP after building:

```powershell
./port/pc/package-windows.ps1 -BuildDirectory build-pc-windows -Version 0.2.1
```

Pass `-Portable` to produce a separately named USB-ready ZIP with
`portable.txt` already included. Its settings, Controller Pak, logs, and
texture packs will remain inside `userdata` after extraction.

The archive contains the executable, launcher, required DLLs, license, and PC runtime
instructions. It deliberately contains no ROM, extracted assets, generated
translation output, save data, or developer diagnostics.

The ROM must be the big-endian USA Rev 0 dump documented in the repository
README. Runtime validation rejects any other file.

The original cartridge has no EEPROM save chip; BattleTanx stores profiles on
an N64 Controller Pak. The port provides a real 32 KiB pak filesystem and
persists it as a standard `.mpk` image, allowing interchange with common N64
emulators. Save data is stored outside the installation directory. Windows uses
`%LOCALAPPDATA%\BattleTanxGA`; Linux uses
`$XDG_DATA_HOME/BattleTanxGA` or `~/.local/share/BattleTanxGA`. Closing the
window requests an orderly runtime shutdown; pak writes are committed
immediately with a recoverable backup file.

The SDL audio queue keeps a small scheduling cushion but caps accumulated
latency. If a skipped cutscene or transition produces audio faster than real
time, the runtime drains or resynchronizes the surplus instead of allowing all
later effects to play seconds late.

## Headless and Linux development

The translated runtime can still be exercised without a display using the
bounded `--boot` mode. Linux RT64 builds use Vulkan:

```sh
cmake -S port/pc -B build-pc-rt64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBTGA_BUILD_RECOMP=ON \
  -DBTGA_BUILD_RUNTIME_PROBE=ON \
  -DBTGA_BUILD_RT64=ON
cmake --build build-pc-rt64 --target btga_runtime_probe
./build-pc-rt64/BattleTanxGA --play /absolute/path/to/BattleTanxGA.z64
```

The small `battletanx_ga_pc` target is an earlier native-engine bring-up
harness. It is retained for portable core tests, but `BattleTanxGA` is the
complete-game compatibility runtime.
