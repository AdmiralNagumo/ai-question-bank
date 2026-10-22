#!/usr/bin/env bash
set -euo pipefail
valid_node() {
    "$1" -e 'const [a,b]=process.versions.node.split(".").map(Number); process.exit(a>22 || (a===22 && b>=19) ? 0 : 1)' 2>/dev/null
}
if [ -n "${AI_NODE_BIN:-}" ]; then
    if valid_node "$AI_NODE_BIN"; then printf '%s\n' "$AI_NODE_BIN"; exit 0; fi
    echo 'AI_NODE_BIN requires Node >=22.19' >&2; exit 1
fi
candidate=$(command -v node || true)
if [ -n "$candidate" ] && valid_node "$candidate"; then printf '%s\n' "$candidate"; exit 0; fi
candidate="${XDG_DATA_HOME:-$HOME/.local/share}/ai-question-bank/node/bin/node"
if [ -x "$candidate" ] && valid_node "$candidate"; then printf '%s\n' "$candidate"; exit 0; fi
echo 'Pi needs Node >=22.19. Run bash scripts/install_ai_node.sh or set AI_NODE_BIN.' >&2
exit 1
