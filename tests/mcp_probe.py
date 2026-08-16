#!/usr/bin/env python3
"""Load the MCP server and report how many tools it registered.

Separate from mcp_smoke.sh so Windows can run the same check: PowerShell drives
it directly, rather than the shell script it has no shell for.

Installing the dependencies and importing them are different questions. A
version range can resolve to a release that installs perfectly well and has
moved the module the server asks for - which is what mcp 2.0 did with FastMCP.
Registering the tools is the part that needs them to be the right ones, because
the decorator runs at import: a FastMCP that has moved fails here rather than at
the first call, by which time it is a client reporting "cannot connect".

    python mcp_probe.py path/to/rpcemu_mcp.py
"""
import importlib.util
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} path/to/rpcemu_mcp.py", file=sys.stderr)
        return 2

    path = sys.argv[1]
    spec = importlib.util.spec_from_file_location("rpcemu_mcp", path)
    if spec is None or spec.loader is None:
        print(f"cannot load {path}", file=sys.stderr)
        return 1

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    tools = [name for name in dir(module) if name.startswith("riscos_")]
    print(f"{len(tools)} tools registered")

    if not tools:
        print("the server imported but registered no tools", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
