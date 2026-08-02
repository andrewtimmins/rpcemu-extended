#!/usr/bin/env bash
#
# Run the unit tests for a configured build directory, and refuse to pass quietly.
#
# The three build scripts used to do this themselves, each with
#
#     if [ -f "$BUILD_DIR/CTestTestfile.cmake" ]; then ctest ...; fi
#
# which reads as caution but is the one shape that cannot fail: a build whose
# tests did not configure has no CTestTestfile.cmake, so the tests were skipped,
# nothing was printed, and the job went green. That is the failure this script
# exists to remove. Everything here is a hard error, because by the time it is
# called the caller has already decided that this build's tests can run on this
# machine - the "cannot run here" judgement (cross-compiled, wrong architecture)
# stays with the caller, where the information is.
#
# Shared between Linux, macOS and Windows deliberately: the point of the exercise
# is that the three platforms are tested identically, and three copies of this
# would drift. It runs under MSYS2 bash on Windows.
#
# Usage: tests/run-ctest.sh <build-dir> [label]
#
# See docs/testing.md.

set -euo pipefail

BUILD_DIR="${1:-}"
LABEL="${2:-}"

if [ -z "$BUILD_DIR" ]; then
	echo "usage: $0 <build-dir> [label]" >&2
	exit 2
fi

say() {
	if [ -n "$LABEL" ]; then
		echo "==> [$LABEL] $*"
	else
		echo "==> $*"
	fi
}

fail() {
	echo "error: $*" >&2
	exit 1
}

if [ ! -d "$BUILD_DIR" ]; then
	fail "$BUILD_DIR is not a directory - nothing has been configured there"
fi

# Tests were expected here. Their absence is the silent skip.
if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
	fail "$BUILD_DIR has no CTestTestfile.cmake, so no tests were configured.
  Either the tests failed to configure, or this build was made with
  -DRPCEMU_BUILD_TESTS=OFF. Neither should reach this script: a build that
  cannot be tested must not be treated as a build that passed."
fi

# Written by tests/CMakeLists.txt, which also checks it at configure time.
count_file="$BUILD_DIR/rpcemu-expected-tests.txt"
if [ ! -f "$count_file" ]; then
	fail "$count_file is missing. It is written by tests/CMakeLists.txt at
  configure time, so either this build directory predates that (re-run cmake) or
  the tests directory was not processed at all."
fi
expected="$(tr -d ' \t\r\n' < "$count_file")"

# What ctest can actually see. Compared against the expectation before running
# anything, so a suite that has lost tests is reported as that rather than as a
# short but successful run.
listed="$(cd "$BUILD_DIR" && ctest -N 2>/dev/null | awk '/^Total Tests:/ { print $3 }')"
if [ -z "$listed" ]; then
	fail "could not read a test count out of 'ctest -N' in $BUILD_DIR"
fi

if [ "$listed" != "$expected" ]; then
	fail "$BUILD_DIR has $listed tests, $expected expected.
  Both numbers come from the same configure step, so a disagreement means this
  build directory is stale. Re-run cmake, or delete it and build again."
fi

say "ctest ($listed tests)"
(
	cd "$BUILD_DIR"
	# --output-on-failure: the failing test's own output is what diagnoses it.
	ctest --output-on-failure
)

say "all $listed tests passed"
