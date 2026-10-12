# BattleTanx: Global Assault native PC runtime

This directory contains the Windows/Linux runtime for the recovered game. It
uses N64Recomp for the original CPU code, RSPRecomp for the audio microcode,
N64ModernRuntime for N64 services, RT64 for display-list rendering, and SDL2
for audio, keyboard/controller input, and rumble. ROMs and generated translated
code are local build inputs and are never committed.

The current Windows build boots the USA Rev 0 ROM, renders through native
Direct3D 12, plays SDL audio, accepts keyboard and SDL game-controller input,
passes the original menus, and enters playable campaign gameplay. Linux uses
RT64's Vulkan backend. This remains development software rather than a packaged
release; in-game save-flow validation, long-session stability, every game
mode, and representative GPU/controller combinations still need broader
validation.

## Run a Windows release

1. Download and extract the Windows x64 ZIP from the repository's
   [Releases](https://github.com/TheGh0stShip/BattleTanx-Global-Assault-PC-Port/releases).
2. Supply your own verified USA Rev 0 big-endian `.z64` ROM; it is not included.
3. Drag the ROM onto **Launch BattleTanx GA.cmd**, or open that launcher and
   paste the ROM path when prompted.

The launcher passes the ROM directly to `BattleTanxGA.exe`; it does not copy,
modify, upload, or bundle it. The runtime validates the ROM before starting.

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
./port/pc/package-windows.ps1 -BuildDirectory build-pc-windows -Version 0.2.0
```

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
