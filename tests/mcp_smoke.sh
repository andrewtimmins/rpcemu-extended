#!/usr/bin/env bash
# The MCP server sets up and starts.
#
# Two questions, and they are not the same one. The setup script installs the
# dependencies; loading the server proves they are the ones it imports. A
# version range can resolve to a release that installs perfectly and has moved
# the module the server asks for, which is a failure a client reports as "cannot
# connect" with nothing about why.
#
# Runs what the README tells a user to run, rather than something like it, so a
# setup script that stops working is a failing build rather than a bug report.
#
# Linux and macOS. The Windows equivalent drives setup-mcp-env.ps1, which needs
# PowerShell and a different path to the interpreter.

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${RPCEMU_MCP_VENV:-${RUNNER_TEMP:-/tmp}/mcp-smoke-venv}"

rm -rf "$VENV"

echo "== setup script =="
RPCEMU_MCP_VENV="$VENV" "$REPO/tools/mcp/setup-mcp-env.sh"

PY="$VENV/bin/python"
if [ ! -x "$PY" ]; then
	echo "FAIL: the setup script did not leave an interpreter at $PY"
	exit 1
fi

echo
echo "== server loads =="
"$PY" "$REPO/tests/mcp_probe.py" "$REPO/tools/mcp/rpcemu_mcp.py"

echo
echo "== config it prints is usable =="
RPCEMU_MCP_VENV="$VENV" "$REPO/tools/mcp/setup-mcp-env.sh" 2>/dev/null |
    sed -n '/^{/,/^}/p' > "$VENV/printed.json"

"$PY" - "$VENV/printed.json" <<'EOF'
import json
import os
import sys

with open(sys.argv[1]) as f:
    doc = json.load(f)          # invalid JSON is a failure in itself

server = doc["mcpServers"]["rpcemu"]

# The two the script knows for certain, as opposed to the data directory and
# machine name it suggests - those depend on the machine it is printed on and
# are not the script's to be sure about.
for label, path in (("command", server["command"]), ("server", server["args"][0])):
    if not os.path.exists(path):
        sys.exit(f"FAIL: printed {label} does not exist: {path}")

print("printed config is valid JSON and its paths exist")
EOF

rm -rf "$VENV"
echo
echo "MCP server smoke test passed"
