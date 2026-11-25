#!/bin/sh
# The whole conformance comparison: rebuild vtdump, run the streams through vtmux, tmux and pyte, print the summary.
#
#   tests/conformance/run.sh [options of conform.py]
#   VTCONF_COUNT=1500 VTCONF_SEED=7 tests/conformance/run.sh        more / other random streams
#   VTCONF_CAPTURE=full tests/conformance/run.sh                    capture real programs at 3 sizes and 3 TERMs (slow, once)
#   VTCONF_FUZZ=300 tests/conformance/run.sh                        also feed random bytes to a sanitizer build
#
# Needs: g++, python3 with pyte and wcwidth, tmux.  Everything it writes goes to $VTCONF_WORK (default /tmp/vtconf);
# its tmux server is on the private socket "vtmux_conf" and is killed at the end.
# Exit status: 0 when nothing but known tmux/pyte quirks differ, 1 when there are real differences (see conform.py).
cd "$(dirname "$0")" || exit 2
WORK="${VTCONF_WORK:-/tmp/vtconf}"
export VTCONF_WORK="$WORK"
mkdir -p "$WORK"
status=0

# 1. real programs in a pty of a known size (captured once, reused: delete $WORK/captured to capture again)
if [ ! -d "$WORK/captured" ] || [ -z "$(ls "$WORK/captured" 2>/dev/null)" ] || [ -n "$VTCONF_CAPTURE" ]; then
    if [ "$VTCONF_CAPTURE" = "full" ]; then python3 capture.py; else python3 capture.py --quick; fi
fi

# 2. the comparison: random streams (many sizes), the hand-written ones, the captured ones; cell colours and attributes too
python3 conform.py --count "${VTCONF_COUNT:-700}" --seed "${VTCONF_SEED:-1}" --attrs --minimize "${VTCONF_MINIMIZE:-12}" \
    --json "$WORK/last_run.json" "$@" || status=1

# 3. optional: random bytes into a build with AddressSanitizer and UBSan
if [ -n "$VTCONF_FUZZ" ]; then
    python3 fuzz.py "$VTCONF_FUZZ" || status=1
fi

tmux -L vtmux_conf kill-server 2>/dev/null
exit $status
