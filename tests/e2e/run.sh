#!/bin/sh
# The end-to-end tests: the real vtmux with real programs in a pseudo-terminal. Builds first.
set -e
cd "$(dirname "$0")/../.."
cmake -B build >/dev/null && cmake --build build -j4 --target vtmux >/dev/null
exec python3 tests/e2e/e2e.py "$@"
