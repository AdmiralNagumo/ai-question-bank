#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
AI_NODE=$(bash ../scripts/find_ai_node.sh)
"$AI_NODE" tests/chat/check_front_client.mjs
