#!/usr/bin/env bash
#
# Check the command-line interface behaves the same on every platform.
#
# The option set, the exit codes and the messages are meant to be identical on
# Linux, macOS and Windows, so a script written against one works on the others.
# That is only true if it is checked the same way on each, which is what this
# does - one set of assertions, run by all the build jobs.
#
# Windows needs one extra thing: the emulator is a GUI-subsystem binary there
# (no console window when double-clicked), so it has no terminal to write to and
# reports these messages in a dialog instead. RPCEMU_NO_GUI_MESSAGES=1 sends
# them to stdout/stderr, which is both what a script would set and what makes
# this testable. It is harmless elsewhere.
#
# Usage: cli_smoke.sh <path-to-emulator>
set -u

BIN="${1:?usage: cli_smoke.sh <binary>}"
export RPCEMU_NO_GUI_MESSAGES=1

failures=0
hung=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The checks below need a machine to name, and RPCEmu no longer ships one: it
# invents nothing at startup, so a fresh install has an empty list until the
# user creates a machine. So make one here rather than depending on whatever the
# release happens to contain. Only configs/ is needed for these paths, and
# pointing both directories at it keeps the test off the real installation.
export RPCEMU_DATADIR="$tmp/data"
export RPCEMU_RESOURCE_DIR="$tmp/data"
mkdir -p "$RPCEMU_DATADIR/configs"
cat > "$RPCEMU_DATADIR/configs/Smoke.cfg" <<'CFG'
[General]
name=Smoke
model=RPCSA
mem_size=256
vram_size=8
CFG

# run <expected-status> <description> -- <args...>
# Captures stdout+stderr into $tmp/out and checks the exit status.
# Every one of these options is meant to print and exit without ever reaching
# wxEntry(), so none should take more than an instant. A run that does not
# finish means the binary started the GUI instead - a failure in its own right,
# and one that would otherwise hang the job until its timeout. macOS ships no
# timeout(1), so the watchdog is hand-rolled and works the same everywhere.
TIMEOUT="${CLI_SMOKE_TIMEOUT:-20}"

run() {
	local want="$1" desc="$2"; shift 3   # drop the "--"
	local rc=0 pid i

	# Once one invocation has hung there is nothing left to learn from the rest.
	if [ "$hung" -ne 0 ]; then
		echo "SKIP: $desc (an earlier check did not return)"
		return 1
	fi

	"$BIN" "$@" > "$tmp/out" 2>&1 &
	pid=$!
	for ((i = 0; i < TIMEOUT; i++)); do
		kill -0 "$pid" 2>/dev/null || break
		sleep 1
	done
	if kill -0 "$pid" 2>/dev/null; then
		kill -9 "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
		echo "FAIL: $desc: did not exit within ${TIMEOUT}s (it should print and return;"
		echo "      taking this long means it started the GUI instead)"
		sed 's/^/      /' "$tmp/out"
		failures=$((failures + 1))
		# One hang means the binary does not return from these paths at all, so
		# the remaining checks would each burn the same wait for the same
		# answer. Stop here rather than spending ten timeouts to learn it once.
		hung=1
		return 1
	fi
	wait "$pid" || rc=$?

	if [ "$rc" != "$want" ]; then
		echo "FAIL: $desc: expected exit $want, got $rc"
		sed 's/^/      /' "$tmp/out"
		failures=$((failures + 1))
		return 1
	fi
	return 0
}

expect_output() { # <pattern> <description>
	if ! grep -qiE -- "$1" "$tmp/out"; then
		echo "FAIL: $2: output did not match /$1/"
		sed 's/^/      /' "$tmp/out"
		failures=$((failures + 1))
		return 1
	fi
	return 0
}

echo "== $BIN"

# --help prints the usage and exits cleanly. Checking the text as well as the
# status matters: a binary that printed nothing would still exit 0.
if run 0 "--help" -- --help; then
	expect_output '^Usage:' "--help"
	expect_output -- '--machine' "--help lists --machine"
	expect_output -- '--resume' "--help lists --resume"
fi

# --list-machines names the machines it can find.
if run 0 "--list-machines" -- --list-machines; then
	expect_output 'Smoke' "--list-machines"
fi

# A usage error exits 2 and says why, rather than silently starting the GUI -
# which on a machine with no display would hang.
if run 2 "--machine with an unknown name" -- --machine no-such-machine; then
	expect_output 'not found' "unknown machine"
fi

if run 2 "an unknown option" -- --no-such-option; then
	expect_output 'unknown option' "unknown option"
fi

if run 2 "a stray argument" -- stray-argument; then
	expect_output 'unexpected argument' "stray argument"
fi

# --resume and --state select a state for a specific machine, so both need one.
if run 2 "--resume without --machine" -- --resume; then
	expect_output 'requires --machine' "--resume without --machine"
fi

if run 2 "--state without --machine" -- --state /nonexistent.state; then
	expect_output 'requires --machine' "--state without --machine"
fi

# They name two different snapshots, so asking for both is ambiguous.
if run 2 "--resume with --state" -- --machine Smoke --resume --state /nonexistent.state; then
	expect_output 'mutually exclusive' "--resume with --state"
fi

# A named state file that does not exist is an error, not a silent plain boot.
if run 2 "--state naming a missing file" -- --machine Smoke --state /nonexistent.state; then
	expect_output 'does not exist' "missing state file"
fi

if [ "$failures" -eq 0 ]; then
	echo "== all command-line checks passed"
	exit 0
fi
echo "== $failures command-line check(s) failed"
exit 1
