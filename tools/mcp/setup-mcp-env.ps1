# RPCEmu Extended - MCP server dependency setup (Windows)
#
# Run this ONCE to install what the MCP server needs, then point your MCP client
# at the interpreter it prints. See README.md for what the server does.
#
# The PowerShell counterpart of setup-mcp-env.sh, which needs bash and so is for
# Linux and macOS. Same job, two differences that Windows forces: the venv keeps
# its interpreter in Scripts rather than bin, and the launcher is "py" or
# "python" rather than "python3".
#
#   powershell -ExecutionPolicy Bypass -File setup-mcp-env.ps1
#
# The -ExecutionPolicy argument is not a warning sign: Windows refuses unsigned
# scripts by default, and this one is not signed.

$ErrorActionPreference = 'Stop'

$venv = if ($env:RPCEMU_MCP_VENV) { $env:RPCEMU_MCP_VENV }
        else { Join-Path $env:USERPROFILE '.rpcemu-mcp' }

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$reqs   = Join-Path $here 'requirements.txt'
$server = Join-Path $here 'rpcemu_mcp.py'

if (-not (Test-Path $reqs) -or -not (Test-Path $server)) {
    Write-Host "Cannot find the server next to this script ($here)."
    Write-Host "Run it from where RPCEmu installed it, which on Windows is the"
    Write-Host "tools\mcp folder of the release you unpacked."
    exit 1
}

# The Windows launcher is "py"; a python.org install also puts "python" on the
# PATH, and the Store's stub is "python" too. Prefer py, which picks the newest.
$launcher = $null
foreach ($c in @('py', 'python')) {
    if (Get-Command $c -ErrorAction SilentlyContinue) { $launcher = $c; break }
}
if (-not $launcher) {
    Write-Host "Python is not installed, or is not on PATH."
    Write-Host ""
    Write-Host "Install it from https://www.python.org/downloads/windows/ and tick"
    Write-Host '"Add python.exe to PATH" in the installer, or: winget install Python.Python.3'
    exit 1
}

Write-Host "Creating a virtual environment in $venv"
& $launcher -m venv $venv
if ($LASTEXITCODE -ne 0) { exit 1 }

$py  = Join-Path $venv 'Scripts\python.exe'
$pip = Join-Path $venv 'Scripts\pip.exe'

Write-Host "Installing the server's dependencies"
& $pip install --quiet --upgrade pip
& $pip install --quiet -r $reqs
if ($LASTEXITCODE -ne 0) { exit 1 }

# Proves the install rather than assuming it: an MCP client reports a server
# that will not start as a connection failure, with the real reason buried. The
# exact symbols, not just the packages - mcp 2.0 kept the package name and moved
# FastMCP out of it, which "import mcp" alone would not have noticed.
& $py -c "from mcp.server.fastmcp import FastMCP; from Crypto.Cipher import DES" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "The dependencies installed but will not import. Try again with the"
    Write-Host "output shown:"
    Write-Host "  $pip install -r $reqs"
    exit 1
}

Write-Host ""
Write-Host "Done. The MCP server is ready."
Write-Host ""
Write-Host "  interpreter : $py"
Write-Host "  server      : $server"
Write-Host ""
Write-Host 'Point your MCP client at both - the interpreter matters, because a bare'
Write-Host '"python" is a different Python without these dependencies in it.'
Write-Host ""
Write-Host "  claude mcp add rpcemu -- `"$py`" `"$server`""
# Where the data directory is, for the block printed below. RPCEmu records it
# only when the user has chosen one, and the file is absent for anyone who kept
# the default - so: the recorded answer if there is one, %USERPROFILE%\RPCEmu
# otherwise. Both are a suggestion to check rather than something verified here.
$datadir = Join-Path $env:USERPROFILE 'RPCEmu'
$store = Join-Path $env:APPDATA 'RPCEmu\datadir'
if (Test-Path $store) {
    $found = (Get-Content $store -Raw).Trim()
    if ($found) { $datadir = $found }
}

# A machine that is actually there, rather than one the reader must invent.
$machine = 'MACHINE-NAME-HERE'
$configs = Join-Path $datadir 'configs'
if (Test-Path $configs) {
    $first = Get-ChildItem -Path $configs -Filter *.cfg -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if ($first) { $machine = $first.BaseName }
}

# JSON wants its backslashes doubled, and these paths are full of them.
$j  = { param($p) $p -replace '\\', '\\' }
$jPy      = & $j $py
$jServer  = & $j $server
$jMachine = & $j (Join-Path $datadir "machines\$machine")

Write-Host ""
Write-Host "Or put this in .mcp.json in your project, which says the same thing. The"
Write-Host "interpreter and the server are known to be right; the paths below them say"
Write-Host "which machine to talk to and are worth a look - see the `"Configuration`""
Write-Host "table in $here\README.md."
Write-Host ""
Write-Host @"
{
  "mcpServers": {
    "rpcemu": {
      "command": "$jPy",
      "args": ["$jServer"],
      "env": {
        "RPCEMU_HOSTCMD_SOCKET": "$jMachine\\hostcmd.sock",
        "RPCEMU_HOSTFS_DIR": "$jMachine\\hostfs",
        "RPCEMU_VNC_HOST": "127.0.0.1",
        "RPCEMU_VNC_PORT": "5900",
        "RPCEMU_VNC_PASSWORD": "",
        "RPCEMU_DEBUG_SOCKET": "$jMachine\\rpcemu-debug.sock"
      }
    }
  }
}
"@
