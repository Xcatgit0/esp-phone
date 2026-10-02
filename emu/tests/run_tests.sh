#!/bin/sh
# Headless regression tests. Usage: tests/run_tests.sh [path/to/phone_emu]
EMU=${1:-$(dirname "$0")/../build/phone_emu}
T=$(dirname "$0")
set -e
echo "== smoke ==";            "$EMU" --headless --run "$T/smoke.lua" | tee /tmp/emu_smoke.log; grep -q "ALL PASSED" /tmp/emu_smoke.log
echo "== cursor.poll ==";     "$EMU" --headless --run "$T/poll.lua" | tee /tmp/emu_poll.log; grep -q "ALL PASSED" /tmp/emu_poll.log
echo "== touch roundtrip =="; "$EMU" --headless --run "$T/touch_roundtrip.lua" | tee /tmp/emu_touch.log
awk '/worst error px/ { if ($NF > 3) exit 1 }' /tmp/emu_touch.log
echo "== push latency ==";    "$EMU" --headless --run "$T/push_latency.lua" | grep -E "sporadic|burst"
echo "OK"
