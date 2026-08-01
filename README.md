# Demo Chat via logos-libp2p-module
[![CI](https://github.com/logos-co/libp2p-module-demo-chat/actions/workflows/ci.yml/badge.svg)](https://github.com/logos-co/libp2p-module-demo-chat/actions/workflows/ci.yml)

This repository is a standalone terminal project that demonstrates
`logos-libp2p-module` with one global GossipSub room named **Demo Chat**.

Each node has a local `--id`. The first time an id is used, Demo Chat generates a
libp2p private key and stores it in `.demo-chat/state/<id>/identity.json`. Later
runs with the same id reuse the same peer identity.

## Build

Run from the workspace root:

```bash
nix --extra-experimental-features 'nix-command flakes' develop . --command bash -lc 'cmake -B build -S . && cmake --build build -j'
```

## Run Demo Chat

Build the project (see the Build section). Open three terminals and run the following commands in each.

Terminal 1 starts the bootstrap/rendezvous node. Keep it running:

```bash
./build/demo-chat --id bootstrap --bootstrap --listen /ip4/127.0.0.1/tcp/9900
```

Terminal 2 starts the first chat node:

```bash
./build/demo-chat --id alice --bootstrap-peer "$(cat .demo-chat/state/bootstrap/bootstrap-peer.txt)"
```

Terminal 3 starts the second chat node:

```bash
./build/demo-chat --id bob --bootstrap-peer "$(cat .demo-chat/state/bootstrap/bootstrap-peer.txt)"
```

Type a message in any Demo Chat terminal and press Enter. Peers connected to the
same bootstrap node and topic receive the message.

## Optional Nicknames

If `--nick` is omitted, Demo Chat uses `demo-<id>`.

```bash
nix --extra-experimental-features 'nix-command flakes' develop . --command bash -lc './build/demo-chat --id alice --nick Alice --bootstrap-peer "$(cat .demo-chat/state/bootstrap/bootstrap-peer.txt)"'
```

## Options

```text
demo-chat --id <id> [--bootstrap] [--bootstrap-peer <peerId>@<multiaddr>[,<multiaddr>]]
          [--listen <multiaddr>] [--nick <name>] [--topic <topic>]
```

- `--id <id>` selects the local profile under `.demo-chat/state/<id>/`.
- `--bootstrap` runs a rendezvous node for service discovery.
- `--bootstrap-peer` connects a normal node to the bootstrap peer.
- `--listen` defaults to `/ip4/0.0.0.0/tcp/0`.
- `--nick` defaults to `demo-<id>`.
- `--topic` defaults to `logos.demo-chat.v1`.

Every started node writes its current connection string to:

```text
.demo-chat/state/<id>/bootstrap-peer.txt
```

The bootstrap commands above use this file so chat nodes can connect without
manually copying the bootstrap peer ID.

## Wire Format

Demo Chat messages are published on the configured GossipSub topic as JSON:

```json
{
  "version": 1,
  "app": "Demo Chat",
  "peerId": "string",
  "id": "string",
  "nick": "string",
  "sentAt": "2026-07-28T11:00:00Z",
  "body": "string"
}
```

Service discovery uses the topic string as the service id. The advertised
service data is intentionally compact because `logos-libp2p-module` currently limits service
data to 33 bytes:

```json
{"app":"Demo Chat","version":1}
```
