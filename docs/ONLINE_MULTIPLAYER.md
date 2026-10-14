# Online multiplayer status and testing

The PC port contains a native, relay-first multiplayer transport foundation.
It is deliberately offline by default: ordinary game startup performs no DNS
lookup, opens no socket, and contacts no service. **F10 → Online** contains the
explicit opt-in actions.

## What works today

### Dedicated authority (current direction)

- A rendererless server executes the translated N64 gameplay routines at 30 Hz.
- Four authenticated client seats submit inputs; the server selects the
  canonical input set and broadcasts it with a canonical state digest.
- The server waits for all configured seats before tick zero, but does not
  freeze the match when a connected seat later times out.
- Snapshot delivery is opt-in and destination-specific. The authority selects
  subscribed seats in round-robin order and incrementally paces each transfer
  through bounded nonblocking network pumps instead of broadcasting state or
  sleeping through an entire checkpoint transfer.
- A returning connection presents a rotating resume credential and atomically
  reclaims the same seat. A freshly restarted process cold-boots the same match,
  installs the newest versioned portable checkpoint, and replays only the tail
  since that 30-tick checkpoint while the authority is paused at a stable head.
- Interactive clients keep one owner-only rotating credential per lobby under
  the port's user-data directory. A rejected stale credential is retired and
  followed by one ordinary join; it is never retried in a loop or reused for a
  different server.
- Authority heartbeats report the exact connected-seat mask. The master uses
  that mask to confirm and renew only genuinely connected reservations; an
  abandoned anonymous join remains provisional and becomes reusable.
- The public master, Azure UDP relay, four protocol clients, and a real
  translated headless client are covered by an unattended PowerShell gate.
- A headless authority consumes no player seat. Empty dedicated lobbies report
  `0 / 4`; the dashboard and lobby browser count only authenticated gameplay
  seats confirmed by the authority's seat mask.
- Dedicated lobbies advertise a protocol capability distinct from peer-hosted
  sessions. **F10 → Online** lists only those authorities and relaunches the
  selected game directly into its authenticated checkpoint/replay client.
- Every dedicated seat (1–4) selects its own full-screen camera. The port
  patches both verified two-player and four-player viewport/scissor templates
  in the runtime-private ROM copy, expanding the local seat and clipping the
  other cameras; local/offline multiplayer and the user's ROM remain unchanged.
- The authority consumes every refreshed relay token returned by its 15-second
  master heartbeat and renews slot zero without reopening the match. A client
  that receives no authority traffic for ten seconds uses its owner-only,
  rotating resume credential to reclaim the same lobby seat and rebind its UDP
  socket. The returned session, seat and relay endpoint must all match before
  gameplay traffic resumes. This lets a restarted relay or changed NAT mapping
  recover without transferring authority to a player or cold-booting the game.

The translated headless Windows client and three protocol peers pass an
unattended four-seat public-relay gate with every simulated tick checked against
the authority digest. A rendered client has also run beyond tick 1,800 without
an authority stall or digest mismatch; every periodic snapshot was confirmed
byte-for-byte. The portable checkpoint schema excludes host-local audio handles,
clocks, scheduler state, and per-viewport projection matrices. Fresh-process
recovery uses explicit gameplay, collision, and gameplay-BSS ranges plus the
authority's canonical input journal; opaque RDRAM copying is prohibited. A cold
replacement at tick 600 restored checkpoint 600, replayed a four-frame tail,
released the barrier only after reaching the frozen head, and stayed
byte-synchronized beyond tick 720. A separate four-seat authority-replacement
stress run killed the authority three times in one session; the replacements
restored checkpoints 150, 300, and 450, reclaimed the same lobby, and continued
through tick 600 without a client desync or authority timeout. The corresponding
native Windows gate also passed three authority replacements through the public
HTTPS master and UDP relay. Longer interactive
campaign coverage, server-driven
interpolation, and cross-architecture validation remain required before this
is exposed as a finished menu option.

The 32,340-byte gameplay snapshot remains the per-tick digest and tick-zero
bootstrap format. It is deliberately not used alone for live restoration. The
dedicated path persists a versioned `BTGASNP3` checkpoint with a 123,225-byte
fixed base containing that canonical prefix, collision state, and gameplay BSS.
A `BTGAGRD1` trailer then stores the variable-size cell-head arrays referenced
by the active collision-grid descriptors. Omitting those arrays mixes restored
node links with freshly allocated cell heads and can create cyclic collision
lists after a cold restart. The checkpoint is captured every 30 ticks, so
recovery cost is bounded by the checkpoint interval rather than total match age.

Repeatedly invoking the translated gameplay update is not a safe shortcut for
accelerating that replay. A four-seat loopback experiment caught up a restarted
client by roughly 1,050 ticks in 20.4 seconds, matched every digest during the
burst, and then diverged 161 ticks after returning to the normal outer loop.
The outer loop therefore owns additional delayed state not reproduced by the
known mode counters. That implementation was rejected. Recovery must either
execute the complete retail outer loop or restore a complete, versioned
checkpoint; do not add a recursive update-only fast-forward path.

The persistent input journal is an endian-stable `BTGAJNL3` stream. Every
40-byte frame carries an FNV-1a checksum over its tick, four controller inputs,
canonical digest, receipt mask and RNG seed. A replacement authority truncates
only an incomplete final record and rejects any full-length corrupted record
before restoring gameplay.

After bootstrap the authority transmits a checkpoint only to a seat that asks
to recover; it does not broadcast periodic snapshots. Clients verify every
canonical frame digest. Extended checkpoint chunks are paced to avoid overflowing
UDP receive buffers. Journal replay offers one bounded window for each client
acknowledgment and performs no deliberate sleeps in the simulation path.

A cold or restarted client first receives the newest portable checkpoint while
the authority continues running. A snapshot request by itself cannot freeze
healthy players. Once that endpoint acknowledges the matching checkpoint
generation, the authority acquires a recovery barrier and freezes the 30 Hz
simulation at a stable journal head. All clients then wait at the same canonical
tick while the recovering process replays its short journal tail through the
normal retail outer loop. The checkpoint-generation identifier prevents a
delayed packet from an older process satisfying the barrier. When the recovering
seat acknowledges that stable head, the authority releases the barrier and all
four seats continue together. A disconnected recovery seat is
removed from the barrier after a 10-second recovery grace period, so a failed
restart cannot freeze the match indefinitely. Its last controls are neutralized
after two seconds, independently of that grace period. This separation keeps a
lost snapshot eligible for the normal three-second retransmission without
letting a disconnected tank continue driving or firing.

Once a client has proved snapshot installation, replay progress is watched
separately from packet liveness. If its acknowledged replay cursor does not
advance for 15 seconds, the authority releases that seat's barrier so one
reachable-but-broken endpoint cannot freeze the other players indefinitely.
Its replay cursor remains active and may continue catching up in the background;
its controls remain neutral until accepted traffic reconnects the seat.

Transport liveness and gameplay-input progress are distinct. Snapshot pings,
duplicate UDP input packets, and repeated stale batches keep a recovery channel
reachable but do not keep a tank's controls active. Only a strictly advancing
input horizon reconnects the seat and refreshes its two-second control lease.

If a rendered client nevertheless detects that its end-of-tick digest differs
from the authority, it stops before the next translated tick, clears frames
from that rejected state lineage, advertises a cold acknowledgment, and
installs the authority's newest persisted checkpoint before replaying the
canonical journal. Three failed automatic resynchronizations within five
minutes quarantine that client: it sends no more traffic, allowing the
authority to neutralize and release its seat instead of repeatedly pausing the
other players at the shared recovery barrier. A quarantined or permanently
incompatible client requests a clean runtime shutdown rather than blocking the
translated tick hook and leaving an unresponsive game window.

While the barrier is closed, the authority broadcasts a 250 ms recovery-status
heartbeat containing the stable journal head and recovering-seat mask. Healthy
clients therefore show a synchronized recovery pause instead of reporting an
authority timeout. Replay delivery is matched to client consumption at eight
frames per acknowledgment (about 32 supplied versus 30 consumed per second),
which avoids nonblocking UDP backpressure without lengthening the replay.

The checkpoint schema includes both complete inner and outer retail mode-state
blocks. Its restore boundary compensates for the client hook occurring after the
retail outer-loop counter transition. Per-player projection matrices are
presentation state and are normalized out, preventing headless and rendered
viewports from causing false desyncs. Persistent envelope magic is `BTGASNP3`,
and the collision-grid trailer is `BTGAGRD1`; old or structurally incomplete
checkpoints fail closed and require a new session.

### Authority-process recovery (development gate)

On Windows, the supported operator entry point is the restart supervisor:

```powershell
.\port\pc\scripts\run-dedicated-server.ps1 `
    -Rom 'C:\Games\BattleTanxGA.z64' -BuildDirectory build-pc-windows
```

Linux authorities use the equivalent supervisor (the ROM remains local and is
never copied into the repository):

```sh
./port/pc/scripts/run-dedicated-server.sh \
    "$HOME/Games/BattleTanxGA.z64" \
    ./build-pc/BattleTanxGA \
    ./build-pc/dedicated-state
```

`BTGA_SERVER_MAX_FAILURES` and `BTGA_SERVER_STABLE_SECONDS` configure its bounded
restart policy. Both supervisors preserve the same durable authority state and
write separate stdout/stderr logs for every process generation. They also hold
an exclusive lock for the state directory; a second supervisor fails before it
can reuse lobby credentials, packet-sequence reservations, or journal files.
At startup they pin the SHA-256 identities of the executable and local ROM;
Windows also pins every sibling runtime DLL. They verify those identities again
before every replacement generation. If a build or file copy changes one, the
supervisor stops instead of silently restoring an older session with different
code or cartridge data; deliberately upgrading a server therefore requires
stopping and restarting the supervisor.
The same identities are persisted in the authority state directory. Reusing a
live journal with a different executable, Windows DLL set, or ROM is refused
even across supervisor invocations; an intentional upgrade uses a new state
directory and creates a new lobby rather than attempting an unsafe migration.
The Linux supervisor creates all state under an owner-only umask. The authority
updates a secret-free `authority-health.json` only when its translated
main loop advances. A live process whose health stops advancing for 60 seconds,
or whose previously observed health record disappears for that long, or which
never publishes health within 120 seconds, is terminated and recovered
through the same bounded restart path. Relay-thread activity alone cannot mask a
stalled simulation. These thresholds are configurable with the supervisor
parameters on Windows or `BTGA_SERVER_HANG_SECONDS` and
`BTGA_SERVER_STARTUP_HEALTH_SECONDS` on Linux.

It owns the child process, durable state directory, generation logs, bounded
restart backoff, and clean Ctrl+C shutdown. When the saved master-service lobby
still exists, a replacement process reclaims it. When that lobby has genuinely
expired, a heartbeat returns 404; the authority removes the stale control
record, creates a replacement lobby, persists the new credentials, and prints
the new `ONLINE_LOBBY_ID`. Other HTTP errors are not converted into new lobbies.
Clients should refresh the Online tab and select the active listing rather than
reusing an old copied ID.

Set `BTGA_DEDICATED_STATE_DIR` to a private directory on durable local storage
before starting `--dedicated-server-public`. The authority then persists:

- the lobby ID, host credential, gameplay session ID and relay endpoint in an
  owner-only control record (never a short-lived admission token);
- the canonical 40-byte-per-tick checksummed input journal; and
- two alternating, checksummed portable gameplay checkpoints.

After a process crash, the replacement process authenticates to the existing
lobby, obtains a fresh signed authority token, reclaims relay slot zero,
restores the newest complete portable checkpoint, and runs every later journal
frame through the real translated update. It aborts instead of publishing if
any replayed digest differs. Torn final journal records and incomplete
checkpoints are ignored or rejected without inventing state.

The unattended Windows gate has an authority-restart mode:

```powershell
.\port\pc\scripts\test-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanxGA.z64' -ReconnectAuthority
```

This path preserves an already-running match. A replacement authority exposes
its validated portable checkpoint before translated gameplay resumes, so a
brand-new process with no prior acknowledgment can install that positive-tick
checkpoint, replay the journal tail, and release the shared recovery barrier.
It never relabels recovered live state as a tick-zero snapshot.

```powershell
.\port\pc\scripts\test-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanxGA.z64'
```

Add `-RenderedClient` to exercise the actual Windows renderer/input client
unattended with the same digest and authority-timeout checks.
For repeatable recovery coverage, add
`-ReconnectClient -ReconnectCycles 2`. The gate cold-restarts the translated
client twice and requires seat reclaim, resume-token rotation, a server-side
recovery barrier, bounded journal replay, and an explicit replay-complete
acknowledgment before the authority resumes after every restart.
Use `-JournalMemoryFrames 64 -RequireDiskReplay` with that gate to force replay
to cross from the disk spool into the bounded in-memory tail. Production keeps
4,096 frames resident. Each older canonical frame occupies a fixed 40-byte,
big-endian checksummed record (about 4.1 MiB per hour at 30 Hz), eliminating the previous
nine-hour rejection cliff without memory growth. Without
`BTGA_DEDICATED_STATE_DIR` the journal remains ephemeral and is removed on
clean shutdown; setting it enables the durable recovery mode above.

The same gate can deterministically discard live gameplay datagrams after
relay admission. This exercises the translated authority and translated client
through the real public relay while leaving the three protocol witnesses
reliable:

```powershell
.\port\pc\scripts\test-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanx - Global Assault (USA).z64' `
    -Seconds 30 -DropOutgoingEvery 10 -DropIncomingEvery 10
```

An interval of 10 discards every tenth datagram in that direction. Zero keeps
the direction lossless; one is rejected. Fault injection is test-only,
default-off, activated after the signed relay handshake, and the gate fails if
the authority and translated client do not both confirm the requested profile.
Bootstrap retries retain one snapshot generation and rotate chunk order, so a
partially received checkpoint accumulates across retries even under periodic
or burst-correlated loss. A cold client remains paused until that checkpoint is
complete; authoritative input frames alone can never bootstrap unknown state.

The same four-player loss gate can run without consuming public lobby quota.
Give it a local checkout of the networking repository; it creates fresh signed
credentials, owns a private loopback relay, archives a unique evidence folder,
and closes every child process when the run ends:

```powershell
.\port\pc\scripts\test-local-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanx - Global Assault (USA).z64' `
    -NetworkingRoot 'C:\src\BattleTanx-Global-Assault-Networking'
```

This local gate defaults to four players, 30 seconds, and deterministic 10%
loss in each direction on the translated authority/client path. Node.js is
needed only to create local signed credentials and run the relay; Azure and the
public master service are never contacted. CI may instead provide a fresh
fixture with `-FixturePath ... -ExternalRelay`.

Add `-ColdJoinAfterAuthorityRestart` to force-kill both authority and translated
client after checkpoint 120, restart the authority from its disk state, and
require a cold client to install a positive-tick checkpoint under the same loss
profile. The gate also requires journal replay, barrier release, and canonical
input echoes from every remaining seat.

Use `-ClientReconnectCycles 3` as a separate recovery gate. It keeps the
authority and three witness seats alive while force-terminating and replacing
the translated client three times with successively rotated admission tokens.
Each process must bind the same seat, install a positive-tick checkpoint,
replay the canonical journal, and cause exactly one recovery barrier to
complete. Any desync, authority timeout, missing checkpoint, missing barrier,
or witness failure fails the run. The fixture intentionally pre-rotates only
three credentials, so the accepted range is zero through three cycles.

Use `-ReconnectEverySeat` for the broader seat-identity gate. It replaces all
four authenticated seats sequentially during one live authority session. The
translated game client and each protocol witness must install a positive-tick
snapshot, acknowledge that snapshot, complete journal catch-up, release one
recovery barrier, and then continue receiving canonical frames. This verifies
that recovery is keyed by authenticated seat identity rather than special
treatment of the translated client. It is mutually exclusive with the cold
authority-restart and repeated-seat gates.

`-AllSeatReconnectCycles 3` is the sustained form of the same gate. It performs
three complete four-seat sweeps (12 authenticated process replacements) and
uses the fixture's next rotated credential for each seat on every sweep. The
one-cycle `-ReconnectEverySeat` switch remains as a quick shorthand. Each
successful sweep must leave the live authority ready for the next one; stale
snapshot generations, replay cursors, or recovery-barrier bits therefore fail
the following cycle rather than escaping detection.

Authority and client recovery can also be composed in one persistent session:

```powershell
.\port\pc\scripts\test-local-dedicated-multiplayer.ps1 `
    -Rom 'C:\Games\BattleTanx - Global Assault (USA).z64' `
    -NetworkingRoot 'C:\src\BattleTanx-Global-Assault-Networking' `
    -ColdJoinAfterAuthorityRestart -AllSeatReconnectCycles 2
```

This first kills and restores the translated authority from its portable disk
checkpoint, requires a cold translated client to converge with that replacement,
then performs two complete all-seat credential rotations on the same canonical
timeline. The local fixture has three rotations per seat; the authority restart
uses seat 0's first one, so the combined gate is intentionally capped at two
subsequent sweeps.

`-InterruptInitialBootstrap` covers the bootstrap boundary itself. It kills the
translated client while the tick-zero checkpoint is still being transferred,
allows the authority to publish its first positive-tick checkpoint, and then
resumes the same authenticated seat. The replacement must supersede the stale
generation, install the newer checkpoint, replay through a frozen authority
head, and release the barrier. It cannot be combined with other recovery modes.

Run the complete four-player recovery acceptance matrix with one command:

```powershell
.\port\pc\scripts\test-dedicated-recovery-matrix.ps1 `
    -Rom 'C:\Games\BattleTanx - Global Assault (USA).z64' `
    -NetworkingRoot 'C:\src\BattleTanx-Global-Assault-Networking'
```

It runs the interrupted-initial-bootstrap gate, a persistent authority restart
followed by two all-seat credential rotations, three further all-seat
rotations, and a real client-state corruption followed by automatic checkpoint
repair. The corruption changes canonical RNG state at tick 120; the gate
requires exactly one digest mismatch, a checkpoint request, journal catch-up,
barrier release, and continued observation by all four seats. Each case uses a
distinct relay port and evidence directory. The wrapper fails on the first
broken invariant and writes a machine-readable JSON summary only after all four
gates pass. Every case also reads two consecutive
authority-health records and requires the translated main-loop sequence to
advance, covering the same signal used by the production supervisors.

Bootstrap and replay phase ownership lives in `BootstrapRecovery`, rather than
being inferred from independent runtime masks. For each seat it owns the
pending phase, expected snapshot generation and encoded replay target; it also
owns the shared recovery barrier. Transport retry timing remains outside this
state machine. `RecoveryProgressWatchdog` separately owns the acknowledged
cursor and stall deadline, so duplicate or backwards acknowledgments cannot
extend a broken recovery forever. A snapshot request alone cannot acquire the
barrier, and snapshot receipt alone cannot release it: the client first proves
the matching generation was installed, then must acknowledge the stable
authority head. A claim beyond the current canonical head is rejected; it
cannot be used to skip replay or release the barrier.

The older peer-hosted flow documented below remains available for comparison,
but it is not the architecture being taken to production.

- Master-service lobby registration and discovery.
- Ed25519-signed host and client relay admission tokens.
- Independent host/client UDP binding through the public Azure relay.
- Portable controller-frame encoding, wrapping, forwarding, unwrapping, session
  and peer validation, and replay/old-tick rejection.
- Public aggregate status at <https://btga.thegh0stship.com/>.
- No UPnP, NAT-PMP, inbound forwarding, or peer address disclosure.
- Native asynchronous peer-lobby discovery, hosting, joining, relay binding,
  heartbeat, and disconnect controls retained in the legacy implementation.
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

This peer-hosted mode is no longer exposed by F10. These commands remain useful
only as developer comparison paths:

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
A subsequent five-run native gate completed a two-player match, traversed
victory/intermission and advanced into a second synchronized gameplay epoch on
both peers. Each peer now receives a distinct full-screen local camera from an
exactly guarded runtime viewport/scissor patch; the user's ROM and offline
split-screen behavior are unchanged. Duplicate remote-player HUD elements
remain a known presentation issue. General rollback/resimulation and full
campaign validation remain open.
Accordingly, this is an experimental synchronization path rather than a claim
that Internet multiplayer is release-ready.

## Targeted gameplay hardening

The dedicated path keeps the original 30 Hz translated simulation and existing
relay/checkpoint protocol, with four narrowly scoped latency and availability
rules:

- A client assigns one controller sample to one tick. Later packets resend up
  to eight contiguous samples with their original ticks; they never copy the
  current input or a keepalive's neutral input into unobserved future ticks.
- Input commitment deadlines use monotonic nanoseconds and an exact rational
  30 Hz conversion. They do not use the former `elapsed_ms / 33` approximation.
- Checkpoint chunks are paced incrementally through bounded, nonblocking
  network pumps. Journal replay likewise offers one bounded window without
  sleeping in the authority's gameplay-critical path.
- A relay-authenticated cold-bootstrap generation opens a fresh packet replay
  window for that seat immediately. Recovery progress is bounded by the
  existing watchdog; a stalled seat is disconnected and cannot hold the other
  seats behind the recovery barrier indefinitely.

The release-quality impairment gate is
`port/pc/scripts/test-four-runtime-multiplayer.ps1`. Unlike the protocol-witness
tests, it launches four translated game clients. Its default 30-minute run uses
deterministic changing drive/turn/fire inputs, 50 ms one-way delay (about
100 ms RTT), and one dropped outgoing datagram in 50 (2%). It requires equal
canonical digests at a shared late tick, sustained progress from every client,
no timeout or quarantine, proof of recovery after any transient digest
mismatch, and a complete match-state transition. Test-only
delay and scripted input require `BTGA_NET_TEST_FAULTS=1` and are never enabled
by a normal game launch.

```powershell
pwsh -NoProfile -File port/pc/scripts/test-four-runtime-multiplayer.ps1 `
  -Rom "C:\path\to\BattleTanx - Global Assault (USA).z64" `
  -NetworkingRoot "C:\src\BattleTanx-Global-Assault-Networking"
```

This gate is deliberately limited to four players and does not authorize
rollback, a replacement transport, 60 Hz gameplay, or a new replication model.

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

# Recovery matrix: healthy session, peer loss, then a new lobby/session that
# must reach synchronized gameplay. Produces summary.json for unattended runs.
.\port\pc\scripts\test-headless-multiplayer.ps1 `
    -Rom 'C:\path\to\BattleTanxGA.z64' `
    -ControllerPak 'C:\path\to\BattleTanxGA_pak1.mpk' `
    -GameRuns 3 -PeerLossRun 2 -ProgressTimeoutSeconds 30

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
pre-boot relay rendezvous, run the real game and networking without creating a
GPU renderer (and with discarded audio) for the bounded interval, navigate the
retail two-player front end with deterministic scripted controller input, and
enter a real two-human gameplay epoch. Both peers must report an acknowledged
snapshot and continued post-rebase frames; every sampled gameplay checksum
must match, with any digest mismatch or synchronization timeout failing the
run. A local controller-pak file is
required for deterministic native gameplay scenarios and is copied into each
isolated profile; it is never uploaded.
The input script includes bounded periodic confirmation pulses so it can advance
from valid retail front-end states without forcing player count or game state.
Logs are retained under
`build-pc-windows/headless-online-evidence/`; no window or controller input is
required. Five paired boots run by default; `-GameRuns` changes that count. The
ROM stays local and is never copied or uploaded.

The latest recovery matrix ran ten consecutive native Windows sessions. Runs
1–4 synchronized normally, run 5 deliberately terminated the joining peer,
and runs 6–10 all created fresh lobbies and synchronized normally afterward.
Every stable session reached the same final sampled tick and checksum, no peer
emitted a digest mismatch, abort, or timeout, and every public lobby was
removed afterward. The harness compares the complete epoch and
sampled-checksum sequences, so legitimate match transitions are covered
without mistaking them for duplicate snapshot application.

The optional `-KillClientAfterEpoch` fault mode terminates the joining runtime
immediately after both peers establish a real gameplay epoch. It requires the
host to trip its bounded frame/input watchdog, leave the relay wait, finish its
headless interval, and remove the lobby. This continuously guards against the
historical “both games froze after one peer disappeared” failure mode.
`-PeerLossRun N` injects the same fault into one run of a longer sequence. Runs
after it must create fresh lobbies and synchronize normally, guarding against
stale process/session state. Stable sessions also have a synchronization-log
progress watchdog. Results are written to
`headless-online-evidence/summary.json`, including the tested executable's
SHA-256 and source Git revision so results cannot be confused with a stale
local build. All logs, saves, and temporary ROM copies remain untracked build
artifacts.

Pass `-ShowWindows` when the visible RT64 rendering path itself needs testing.
The default rendererless mode isolates simulation and networking failures from
graphics-driver or window-system failures; it does not replace visible
multiplayer testing.

Do not place admission tokens, host secrets, ROMs, or packet captures containing
private test data in bug reports or commits.
