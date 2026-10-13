# Online multiplayer status and testing

The PC port contains a native, relay-first multiplayer transport foundation.
It is deliberately offline by default: ordinary game startup performs no DNS
lookup, opens no socket, and contacts no service. **F10 → Online** contains the
explicit opt-in actions.

## What works today

- Master-service lobby registration and discovery.
- Ed25519-signed host and client relay admission tokens.
- Independent host/client UDP binding through the public Azure relay.
- Portable controller-frame encoding, wrapping, forwarding, unwrapping, session
  and peer validation, and replay/old-tick rejection.
- Public aggregate status at <https://btga.thegh0stship.com/>.
- No UPnP, NAT-PMP, inbound forwarding, or peer address disclosure.
- Native asynchronous lobby discovery, hosting, joining, relay binding,
  heartbeat, and disconnect controls in **F10 → Online**.
- Experimental two-player deterministic runtime synchronization: host is
  original N64 controller port 1 and joining player is port 2. A pre-boot
  rendezvous agrees on the ROM/session seed before either machine enters N64
  code. Inputs use a four-poll jitter buffer, while the game update clock uses
  a strict common frame zero and a bounded four-update lead window.
- An authoritative gameplay snapshot at each match epoch. Snapshot chunks
  tolerate loss, duplication, reordering, and a lost acknowledgement; applying
  the initial snapshot rebases both the simulation clock and the complete
  delayed-input window.
- Per-frame selected-state digests. Three consecutive mismatches stop the
  online session instead of allowing silent divergent play. Automatic rollback
  awaits a complete serialization of pointer-owned world state.
- Ten-second fail-open watchdogs on every in-game synchronization wait. A
  failed or departed peer cannot leave an already-running game in an infinite
  network wait; the online session is dropped and local play resumes. The
  pre-boot lobby intentionally waits for a peer until the user cancels it.

On Windows, the normal path is **F10 → Online → Host/Join**. After lobby
selection, the application relaunches itself into the synchronized pre-boot
rendezvous. This is automatic; neither player has to close the game, pass
launch arguments, or reproduce menu navigation. These commands remain useful
as developer fallbacks:

```text
BattleTanxGA.exe --online-host BattleTanxGA.z64
BattleTanxGA.exe --online-join LOBBY_ID BattleTanxGA.z64
```

The host writes the generated ID to `online-lobby.txt` in its data directory.
Both peers remain outside N64 execution until the signed relay handshake and a
Hello/Start/Ack/Go rendezvous complete. Host input then drives the original
multiplayer menus on both processes, while the joining player's controller
occupies port 2.

The October 2026 Windows validation kept paired seed, game-frame and input
checkpoints equal through title/menu transitions without the previous freeze.
A subsequent native two-process test completed a two-player campaign mission:
both peers reached victory and advanced into the next mission together, with
the same input epoch, RNG seed and game-frame reset on each process. Automatic
rollback/resimulation, disconnect recovery, longer multi-mission validation,
and one full-screen local camera per device remain open.
Accordingly, this is an experimental synchronization path rather than a claim
that Internet multiplayer is release-ready.

## Reproduce the public PC transport test

Requirements: CMake 3.20+, a C++20 compiler, Git, and OpenSSL development
headers on Linux. Windows uses WinHTTP and the operating-system certificate
store.

```sh
# Linux: core tests, randomized synchronization soak, and one public lifecycle.
./port/pc/scripts/test-live-multiplayer.sh

# Repeat the complete public lifecycle 20 times without launching the game.
./port/pc/scripts/test-live-multiplayer.sh port/pc/build-live-test 20

# Native Windows PowerShell; without -Rom this does not launch the game.
.\port\pc\scripts\test-headless-multiplayer.ps1 -RelayRuns 20

# Add a real paired, windowless game cold boot using your local verified ROM.
.\port\pc\scripts\test-headless-multiplayer.ps1 `
    -RelayRuns 20 -Rom 'C:\path\to\BattleTanxGA.z64' `
    -ControllerPak 'C:\path\to\BattleTanxGA_pak1.mpk' `
    -GameRuns 5 -BootSeconds 60

# Fault injection: terminate the joining peer after gameplay synchronization
# and prove that the host watchdog leaves its wait instead of freezing.
.\port\pc\scripts\test-headless-multiplayer.ps1 `
    -Rom 'C:\path\to\BattleTanxGA.z64' `
    -ControllerPak 'C:\path\to\BattleTanxGA_pak1.mpk' `
    -GameRuns 1 -KillClientAfterEpoch

```

The script builds and runs `btga_pc_native_online_e2e`. That executable uses
the same native master client, TLS stack, UDP adapter, and relay state machine
as the runtime. It creates and discovers a leased lobby, requests separate host
and client tokens, binds two native clients, exchanges encoded controller
frames in both directions through the public relay, changes the dashboard state
to `playing`, and removes the
temporary lobby through an authenticated cleanup guard.

The synchronization soak is local and does not contact the service. It runs
500 randomized snapshot epochs with packet loss, reordering, duplicates,
corruption, retransmission and simulated acknowledgement loss, followed by
12,000 delayed-input frames across 50 authoritative timeline rebases. The
public lifecycle count is explicit because it creates real short-lived Azure
lobbies; the default is one.

When `-Rom` is supplied, the Windows harness additionally launches pairs of real
recompiled runtimes with isolated data directories. The first creates a lobby,
the second reads its generated lobby ID and joins, both complete the signed
pre-boot relay rendezvous, run with a hidden native renderer and discarded
audio for the bounded interval, navigate the retail two-player front end with
deterministic scripted controller input, and enter a real two-human gameplay epoch,
require both peers to report an acknowledged snapshot and continued post-rebase
frames, require every sampled gameplay checksum to match, reject any digest
mismatch or synchronization timeout, and exit. A local controller-pak file may
be copied into each isolated profile with `-ControllerPak`; it is never uploaded.
The input script includes bounded periodic confirmation pulses so it can advance
from valid retail front-end states without forcing player count or game state.
Logs are retained under
`build-pc-windows/headless-online-evidence/`; no window or controller input is
required. Five paired boots run by default; `-GameRuns` changes that count. The
ROM stays local and is never copied or uploaded.

The current automated Windows evidence is five consecutive three-minute cold
boots. Every pair entered gameplay at simulation frame 381, advanced through
the end-of-match/intermission states, established a second identical gameplay
epoch at frame 3714, and remained synchronized through frame 4740. Every
sampled gameplay checksum matched, no peer emitted a digest mismatch, abort or
timeout, and every public lobby was removed afterward. The harness compares
the complete epoch and sampled-checksum sequences, so legitimate match
transitions are covered without mistaking them for duplicate snapshot
application.

The optional `-KillClientAfterEpoch` fault mode terminates the joining runtime
immediately after both peers establish a real gameplay epoch. It requires the
host to trip its bounded frame/input watchdog, leave the relay wait, finish its
headless interval, and remove the lobby. This continuously guards against the
historical “both games froze after one peer disappeared” failure mode.

Do not place admission tokens, host secrets, ROMs, or packet captures containing
private test data in bug reports or commits.
