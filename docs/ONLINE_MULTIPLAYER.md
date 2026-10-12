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
- Experimental two-player runtime input synchronization: host is original N64
  controller port 1, joining player is port 2, and input is exchanged at the
  controller-poll boundary with a bounded matching-tick wait.

The current runtime path requires both processes to begin at the same retail
menu state. Host input then drives the original multiplayer menus on both
processes, while the joining player's controller occupies port 2. Automatic
state reconciliation, a formal ready/loading barrier, loss recovery, snapshots,
disconnect recovery, and one full-screen local camera per device remain open.
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
