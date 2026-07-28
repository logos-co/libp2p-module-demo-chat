#!/usr/bin/env bash
set -euo pipefail

cat <<'EOF'
# Terminal 1: build and start the Demo Chat bootstrap node
cd libp2p-module-demo-chat
nix --extra-experimental-features 'nix-command flakes' develop ../logos-libp2p-module --command bash -lc 'cmake -B build -S . && cmake --build build -j && ./build/demo-chat --id bootstrap --bootstrap --listen /ip4/127.0.0.1/tcp/9900'

# Terminal 2: start the first Demo Chat node
cd libp2p-module-demo-chat
nix --extra-experimental-features 'nix-command flakes' develop ../logos-libp2p-module --command bash -lc './build/demo-chat --id alice --bootstrap-peer "$(cat .demo-chat/state/bootstrap/bootstrap-peer.txt)"'

# Terminal 3: start the second Demo Chat node
cd libp2p-module-demo-chat
nix --extra-experimental-features 'nix-command flakes' develop ../logos-libp2p-module --command bash -lc './build/demo-chat --id bob --bootstrap-peer "$(cat .demo-chat/state/bootstrap/bootstrap-peer.txt)"'
EOF
