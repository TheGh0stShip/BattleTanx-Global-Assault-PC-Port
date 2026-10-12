# Online multiplayer status and testing

The PC port contains a native, relay-first multiplayer transport foundation.
It is deliberately offline by default: ordinary game startup performs no DNS
lookup, opens no socket, and contacts no service. A future user-facing
**Online Multiplayer** action will be the explicit opt-in boundary.

## What works today

- Master-service lobby registration and discovery.
- Ed25519-signed host and client relay admission tokens.
- Independent host/client UDP binding through the public Azure relay.
- Protocol-valid gameplay datagram wrapping, forwarding, unwrapping, and
  validation in the native PC C/C++ client.
- Public aggregate status at <https://btga.thegh0stship.com/>.
- No UPnP, NAT-PMP, inbound forwarding, or peer address disclosure.

The automated live test does **not** claim that the translated game currently
has an online lobby screen or synchronized simulation. Those are the next
integration milestones.

## Reproduce the public PC transport test

Requirements: CMake 3.20+, a C++20 compiler, Git, `curl`, and `jq`.

```sh
./port/pc/scripts/test-live-multiplayer.sh
```

The script builds `btga_pc_live_multiplayer_test`, creates a leased test lobby,
requests separate host and client tokens, and launches two independent native
processes. The client sends a protocol-valid `PING` gameplay datagram through
the public relay; the host must receive and decode the marker. A shell trap
removes the temporary lobby and terminates the host on success or failure.

To test a development master service, set the API root explicitly:

```sh
BTGA_MASTER_API=https://example.test/api/v1 \
  ./port/pc/scripts/test-live-multiplayer.sh /tmp/btga-live-build
```

Do not place admission tokens, host secrets, ROMs, or packet captures containing
private test data in bug reports or commits.
