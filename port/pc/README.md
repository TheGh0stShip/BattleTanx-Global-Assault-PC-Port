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
release; save-data, long-session stability, every game mode, and representative
GPU/controller combinations still need broader validation.

## Controls

- WASD: analog stick
- X / Z: N64 A / B
- Left Shift: Z trigger
- Enter: Start
- Q / E: L / R
- Arrow keys: D-pad
- I / J / K / L: C buttons

SDL game controllers use the left stick, face and shoulder buttons, left
trigger, D-pad, and rumble.

## Windows developer build

Use a native Windows checkout on NTFS and run PowerShell—not the Linux binary
through `wsl.exe`. Visual Studio's **Desktop development with C++** workload is
required. After the pinned dependencies and locally generated recompilation
inputs described in [`recomp/README.md`](recomp/README.md) are present:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
./port/pc/build-windows.ps1 -Rom 'C:\Games\BattleTanxGA.z64' -Run
```

The script configures an optimized x64 Ninja build, builds
`BattleTanxGA.exe`, copies SDL2 and the DirectX Shader Compiler runtime beside
it, and optionally launches it with the supplied local ROM. It rejects WSL UNC
source paths because MSVC/Ninja dependency handling is unreliable there.

The ROM must be the big-endian USA Rev 0 dump documented in the repository
README. Runtime validation rejects any other file.

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
