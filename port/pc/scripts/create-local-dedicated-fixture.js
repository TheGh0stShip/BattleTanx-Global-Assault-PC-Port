"use strict";

// Generates short-lived, signed relay credentials for the local dedicated
// transport gate. It never starts a master service or contacts Azure.

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

async function main() {
    const [output, networkingRoot, portText, relayHost = "127.0.0.1"] =
        process.argv.slice(2);
    const port = Number(portText);
    if (!output || !networkingRoot || !Number.isInteger(port) ||
        port < 1 || port > 65535) {
        throw new Error(
            "usage: node create-local-dedicated-fixture.js OUTPUT NETWORKING_ROOT PORT [HOST]"
        );
    }
    const corePath = path.join(networkingRoot, "server", "master", "src", "core.js");
    const { MasterService, MemoryLobbyStore } = require(corePath);
    const { privateKey, publicKey } = crypto.generateKeyPairSync("ed25519");
    const service = new MasterService(
        new MemoryLobbyStore(),
        crypto.randomBytes(32),
        () => Date.now() / 1000,
        { address: relayHost, udp_port: port },
        privateKey
    );
    const build = "pc-port-v2";
    const host = await service.register({
        name: "Local Dedicated Transport Gate",
        protocol: 1,
        build,
        platform: "pc",
        capabilities: 2,
        region: "local",
        max_players: 4,
        current_players: 1,
        udp_port: port
    }, "127.0.0.1");
    const clients = [];
    for (let seat = 0; seat < 4; ++seat) {
        clients.push(await service.join(host.lobby.id, {
            protocol: 1,
            build,
            platform: "pc"
        }, "127.0.0.1"));
    }
    const document = {
        session: BigInt(`0x${host.session_id}`).toString(10),
        relay_host: relayHost,
        relay_port: port,
        relay_public_key: publicKey.export({
            format: "der", type: "spki"
        }).toString("base64"),
        authority_token: host.relay_token,
        client_tokens: clients.map((client) => client.join_token)
    };
    fs.writeFileSync(output, `${JSON.stringify(document)}\n`, {
        encoding: "utf8", mode: 0o600
    });
}

main().catch((error) => {
    process.stderr.write(`${error.stack || error.message}\n`);
    process.exitCode = 1;
});
