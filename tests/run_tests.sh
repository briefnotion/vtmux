#!/bin/sh
# Build every test program (tests/*_tests.cpp) with the strict flags and the sanitizers, and run them.
# A test names the sources it needs on a line like:   // sources: ../src/vtparser.cpp ../src/emulator.cpp
# Usage: run_tests.sh [name ...]      (names without _tests.cpp; no names: all of them)
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/vtmux_tests.$$"
mkdir -p "$OUT"
FLAGS="-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined"
if [ $# -gt 0 ]; then TESTS="$*"; else TESTS=$(ls *_tests.cpp | sed 's/_tests.cpp//'); fi
status=0
for name in $TESTS; do
    t="${name}_tests"
    SOURCES=$(sed -n 's|^// sources:||p' "$t.cpp" | head -1)
    echo "== $t"
    if g++ $FLAGS -I.. -o "$OUT/$t" "$t.cpp" $SOURCES && "$OUT/$t"; then :; else status=1; echo "FAILED: $t"; fi
done
rm -rf "$OUT"
exit $status
