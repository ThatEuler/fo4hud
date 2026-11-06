#!/usr/bin/env bash
set -euo pipefail
fo4hud_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -x "$fo4hud_root/.venv-mcp/bin/python" ]]; then
    echo 'Run make setup-mcp in the fo4hud checkout first.' >&2
    exit 1
fi
exec "$fo4hud_root/.venv-mcp/bin/python" "$fo4hud_root/mcp_server/fo4hud_server.py"
