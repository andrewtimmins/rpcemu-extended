#!/usr/bin/env bash
# RPCEmu Extended - MCP server dependency setup
#
# Run this ONCE to install what the MCP server needs, then point your MCP client
# at the interpreter it prints. See README.md for what the server does.
#
# It installs into a virtual environment rather than the system Python, which is
# not fastidiousness: most current distributions mark the system Python as
# externally managed and refuse a plain "pip install" outright (PEP 668), some
# ship no pip at all, and a .deb install's copy of the server is root-owned and
# not somewhere to be writing anyway. A venv answers all three, and brings its
# own pip even when the system has none.

set -e

VENV="${RPCEMU_MCP_VENV:-$HOME/.rpcemu-mcp}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REQS="$HERE/requirements.txt"
SERVER="$HERE/rpcemu_mcp.py"

if [ ! -f "$REQS" ] || [ ! -f "$SERVER" ]; then
	echo "Cannot find the server next to this script ($HERE)."
	echo "Run it from where RPCEmu installed it:"
	echo "  a source or .tar.gz tree   tools/mcp/setup-mcp-env.sh"
	echo "  the .deb                   /usr/share/rpcemu/mcp/setup-mcp-env.sh"
	exit 1
fi

if ! command -v python3 &>/dev/null; then
	echo "python3 is not installed."
	echo ""
	echo "Debian/Ubuntu:  sudo apt install python3 python3-venv"
	echo "Fedora:         sudo dnf install python3"
	echo "macOS:          brew install python3"
	exit 1
fi

# Debian splits the venv module into its own package, and the error you get
# without it names ensurepip rather than the package to install.
if ! python3 -c "import venv" &>/dev/null; then
	echo "The python3 venv module is missing."
	echo ""
	echo "Debian/Ubuntu:  sudo apt install python3-venv"
	echo "Fedora:         sudo dnf install python3-libs"
	exit 1
fi

echo "Creating a virtual environment in $VENV"
python3 -m venv "$VENV"

echo "Installing the server's dependencies"
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$REQS"

# Proves the install rather than assuming it: an MCP client reports a server
# that will not start as a connection failure, with the real reason buried.
if ! "$VENV/bin/python" -c "
from mcp.server.fastmcp import FastMCP
from Crypto.Cipher import DES
" 2>/dev/null; then
	echo ""
	echo "The dependencies installed but will not import. Try again with the"
	echo "output shown:"
	echo "  $VENV/bin/pip install -r $REQS"
	exit 1
fi

# Where the data directory is, for the block printed below.
#
# RPCEmu records it only when the user has chosen one; the file is absent for
# anyone who kept the default, which is most people. So: the recorded answer if
# there is one, ~/RPCEmu otherwise, which is what an installed build uses. Both
# are a suggestion to be checked rather than something this has verified.
if [ "$(uname -s)" = "Darwin" ]; then
	STORE="$HOME/Library/Preferences/RPCEmu/datadir"
else
	STORE="${XDG_CONFIG_HOME:-$HOME/.config}/rpcemu/datadir"
fi

DATADIR="$HOME/RPCEmu"
if [ -r "$STORE" ]; then
	FOUND="$(tr -d '\r\n' < "$STORE")"
	if [ -n "$FOUND" ]; then
		DATADIR="$FOUND"
	fi
fi

# A machine that is actually there, so the block names one rather than leaving
# the reader to invent it. Which machine they want is their business.
MACHINE="MACHINE-NAME-HERE"
for cfg in "$DATADIR"/configs/*.cfg; do
	[ -e "$cfg" ] || break
	MACHINE="$(basename "$cfg" .cfg)"
	break
done

cat <<EOF

Done. The MCP server is ready.

  interpreter : $VENV/bin/python
  server      : $SERVER

Point your MCP client at both - the interpreter matters, because a bare
"python3" is a different Python without these dependencies in it.

  claude mcp add rpcemu -- "$VENV/bin/python" "$SERVER"

Or put this in .mcp.json in your project, which says the same thing. The
interpreter and the server are known to be right; the paths below them say
which machine to talk to and are worth a look - see the "Configuration" table
in $HERE/README.md.

{
  "mcpServers": {
    "rpcemu": {
      "command": "$VENV/bin/python",
      "args": ["$SERVER"],
      "env": {
        "RPCEMU_HOSTCMD_SOCKET": "$DATADIR/machines/$MACHINE/hostcmd.sock",
        "RPCEMU_HOSTFS_DIR": "$DATADIR/machines/$MACHINE/hostfs",
        "RPCEMU_VNC_HOST": "127.0.0.1",
        "RPCEMU_VNC_PORT": "5900",
        "RPCEMU_VNC_PASSWORD": "",
        "RPCEMU_DEBUG_SOCKET": "$DATADIR/machines/$MACHINE/rpcemu-debug.sock"
      }
    }
  }
}
EOF
