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

For the complete player-facing reference, including menu operation, controller
defaults, rebinding, simultaneous mouse/controller input, and independent
turret aiming, see [../../docs/CONTROLS.md](../../docs/CONTROLS.md).

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

The **Accessibility** tab includes a screen-shake intensity control. Reducing
it scales only random camera displacement; vehicle recoil, gameplay state, and
the original shake decay timing remain intact.

The aspect-ratio selector drives RT64's projection and extended-origin settings
without changing the game's simulation. This is the renderer foundation for
true widescreen; game-specific HUD placement, split-screen layouts, cutscenes,
menus, and framebuffer effects still require visual validation before the
widescreen milestone is considered complete.

### Optional gameplay mods

The **Mods** tab contains two opt-in changes. Both default to off, so a new
installation retains the original game rules and presentation.

- **No Fog / Increased Draw Distance** (**Extended visibility**) raises the
  game's projection and world-culling range
  to its safe signed limit and omits the frame's fog geometry mode, fog colour,
  and fog-position commands. Levels may reveal distant geometry that retail
  never intended to draw simultaneously.
- **Aim-Down-Turret / Independent Turret Aiming** (**Independent dual-stick
  turret aiming**) applies only to player-controlled
  vehicles whose tank definition has a rotating turret. The left stick keeps
  the original hull movement, while the right stick rotates the turret. W/S
  move along the hull's heading and A/D turn the hull; turret or camera heading
  does not reorient the hull while aim mode is held. Releasing RMB (or centering
  the right stick) restores the player's selected retail control mechanics. The
  camera currently retains the game's retail follow behavior; a dedicated
  turret-relative camera remains experimental because applying the game's
  presentation transform globally also affects vehicle-selection previews.
  Existing weapon spawn rules remain in charge, so rounds and turret-aimed
  weapons follow the turret while mines, gun buddies, bouncing betties, and
  other deployables keep their original behavior.

For mouse and keyboard, WASD drives, left click supplies primary fire, and
holding right click captures relative mouse movement for turret aiming. Release
right click to return the cursor. Mouse turret sensitivity is adjustable in the
Mods tab. Each player also has independent right-stick turret sensitivity and
horizontal inversion in the Controllers tab. Open F10 settings before changing windows or
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

## Online multiplayer status

The PC core compiles against a pinned GPL-3.0-only revision of the shared
[BattleTanx: Global Assault networking project](https://github.com/TheGh0stShip/BattleTanx-Global-Assault-Networking).
The native PC transport passes a public end-to-end test through the live
master and Azure UDP relay: two independent PC client processes bind as host
and client, then exchange and decode controller frames in both directions.
The translated runtime now assigns the host to original controller port 1 and
the joining player to port 2. Its validated cold-boot path completes the relay
handshake before N64 execution, shares the initial seed and input epoch, buffers
four gameplay frames, and bounds either simulation to four game updates ahead
of its peer. Front-end input remains poll-synchronized until a live match
exists. Networking starts
offline and opens no socket or DNS request during normal startup. Only a user
action in **F10 → Online** opts in and initializes the master/relay path;
leaving that flow returns the client fully offline.

The included Windows/Linux UDP adapter is also inert until that explicit
transition. It resolves and opens the configured relay endpoint on demand,
uses bounded nonblocking datagrams, and closes the platform socket completely
when the online flow ends.

The design is relay-first and uses outbound UDP only. It does not use UPnP,
NAT-PMP, inbound port forwarding, or expose a host's home connection. Each
device runs the original two-controller simulation but now receives its own
full-screen local camera: the runtime-private ROM copy expands that player's
verified viewport/scissor template and suppresses the remote camera. The
user's ROM is never modified, and offline/local multiplayer is unchanged.
Some duplicate remote-player HUD elements remain visible and are tracked as a
presentation-only cleanup.

The public service dashboard at
[`btga.thegh0stship.com`](https://btga.thegh0stship.com/) shows active games,
open lobbies, and players without exposing addresses or credentials.
The F10 settings overlay's **Online** tab can refresh compatible lobbies, host
a public lobby, join a listed lobby, authenticate with the relay, maintain the
host lease, disconnect, and open the dashboard. Merely opening the tab performs
no network access.

The normal Windows experimental play flow is:

1. Both players launch normally with their own verified local ROM.
2. The host opens **F10 → Online** and selects **Host**. The other player uses
   **Refresh** and **Join** on that lobby.
3. Each application automatically restarts into a synchronized pre-boot relay
   rendezvous. No command-line arguments or manual cold boot are required.
4. Both processes remain outside N64 code until the handshake completes. Host
   input occupies original controller port 1 and the joining player's input
   occupies port 2.

The automatic restart is the state guarantee: an online session never attempts
to attach one live emulation instance to another. Developer fallback commands
remain available as `--online-host ROM` and `--online-join LOBBY_ID ROM`.
General rollback/resimulation and full-campaign validation are still under
development. The peer-hosted path is being superseded by a four-player native
dedicated authority. The authority runs the translated gameplay simulation,
waits for four authenticated seats before tick zero, continues when a seat
times out, publishes canonical input frames, and sends periodic canonical
gameplay snapshots. A real translated Windows client plus three protocol peers
has completed this flow through the public service. Recovery state is captured
at tick entry. Snapshot delivery targets one subscribed seat every eight ticks,
avoiding the old broadcast bandwidth spike. A returning connection atomically
reclaims its stable seat with a rotating resume token. The authority reports
its exact connected-seat mask so the master renews active reservations without
preserving abandoned anonymous joins. A rendered Windows client has separately
passed beyond tick 1,800
with every checkpoint byte-identical and no authority timeout. Longer
interactive campaign recovery remains under development. A restarted process
now reclaims its seat, cold-boots tick zero, replays the authority's targeted
canonical journal, and reconverges in the unattended gate. A changed-RDRAM
experiment was rejected after it corrupted live RSP/audio and scheduler state;
portable checkpoints are still needed to prune long histories. This remains a
development path rather than a finished public mode.

Build and run the current dedicated gate on Windows with:

```powershell
.\port\pc\build-windows.ps1
.\port\pc\scripts\test-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanxGA.z64' -BuildDirectory build-pc-windows
```

For an actual persistent authority, use the supervisor rather than launching
the executable directly:

```powershell
.\port\pc\scripts\run-dedicated-server.ps1 `
    -Rom 'C:\Games\BattleTanxGA.z64' -BuildDirectory build-pc-windows
```

It retains recovery state and per-generation logs in
`build-pc-windows\dedicated-state`, restarts a failed authority with bounded
backoff, and prints the active `ONLINE_LOBBY_ID`. If Azure has expired the
saved lobby, the replacement authority discards only that stale control record
and creates a new lobby. Players must join the newly printed/listed ID; a copied
expired ID returns `404 lobby not found` by design. Press Ctrl+C once to stop
the supervisor and its child authority.

Add `-RenderedClient` to run that unattended gate through the actual Windows
renderer/input client instead of the headless translated client.
Add `-ReconnectClient -ReconnectCycles 2` to kill and cold-restart the
translated client twice. Every replacement process must reclaim the same seat,
rotate its resume credential, replay the canonical journal, and reconverge at a
positive authority tick.
Add `-JournalMemoryFrames 64 -RequireDiskReplay` to force that test across the
disk-journal boundary. Production retains 4,096 recent frames in memory and
spools older canonical input frames as fixed-width, endian-stable records in a
per-process temporary file, which is removed on clean authority shutdown.

For a visible client, leave the authority running, copy the printed lobby ID,
and launch `BattleTanxGA.exe --dedicated-join LOBBY_ID BattleTanxGA.z64`.
For a single-command Windows session with a rendered client, dedicated
authority, and three automated seats, run:

```powershell
.\port\pc\scripts\play-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanxGA.z64' -BuildDirectory build-pc-windows
```

Closing the rendered game stops the authority and automated seats.

Developers can reproduce the fully native PC transport test with CMake and a
C++20 compiler:

```sh
./port/pc/scripts/test-live-multiplayer.sh
```

The script builds a native client that creates and discovers a temporary lobby,
obtains separate signed host/client admission tokens, binds both endpoints,
verifies bidirectional encoded controller delivery, updates the public game state, and removes the
lobby through an authenticated cleanup guard. See
[online multiplayer testing](../../docs/ONLINE_MULTIPLAYER.md) for scope and
security details.

The native Windows runtime can also exercise hosting, joining, synchronized
gameplay, peer loss, and a subsequent clean-session recovery without opening
windows or requiring human input. Supply only local ROM and Controller Pak
paths; neither file is copied into the repository:

```powershell
.\port\pc\scripts\test-headless-multiplayer.ps1 `
  -Rom C:\path\to\BattleTanxGA.z64 `
  -ControllerPak C:\path\to\battletanx-ga.n64.us.1.0_pak1.mpk `
  -GameRuns 3 -PeerLossRun 2 -BootSeconds 60
```

This sequence runs a healthy session, terminates the client during the second
session, then creates a new lobby and requires the third session to reach
synchronized gameplay. A progress watchdog rejects frozen pairs, and
`headless-online-evidence/summary.json` records the result, tested executable
SHA-256, and source Git revision. Temporary ROM copies, saves, and logs remain
build artifacts and must not be committed.

By default this unattended gate runs the real game simulation and networking
without creating a GPU renderer, which keeps graphics-driver failures from
masquerading as synchronization failures. Add `-ShowWindows` to exercise the
visible RT64 path as a separate diagnostic.
