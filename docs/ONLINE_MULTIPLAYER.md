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

The validated runtime path connects before cold boot:

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
./port/pc/scripts/test-live-multiplayer.sh
```

The script builds and runs `btga_pc_native_online_e2e`. That executable uses
the same native master client, TLS stack, UDP adapter, and relay state machine
as the runtime. It creates and discovers a leased lobby, requests separate host
and client tokens, binds two native clients, exchanges encoded controller
frames in both directions through the public relay, changes the dashboard state
to `playing`, and removes the
temporary lobby through an authenticated cleanup guard.

Do not place admission tokens, host secrets, ROMs, or packet captures containing
private test data in bug reports or commits.
