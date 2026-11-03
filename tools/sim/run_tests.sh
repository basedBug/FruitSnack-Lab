#!/usr/bin/env bash
# Runs each simulation test in its own process so a crash in one test does not
# hide the results of the others.
set -uo pipefail
cd "$(dirname "$0")/../.."
export PATH=/ucrt64/bin:/usr/bin:$PATH

OUT="tools/sim/fruitsnack_sim.exe"
g++ -std=c++17 -g -O0 -Wall -Wextra -Wno-unused-parameter \
  -I tools/sim/shim -I src \
  tools/sim/sim_main.cpp \
  src/ntc_sensor.cpp src/heater.cpp src/pid.cpp \
  src/telemetry.cpp src/heater_controller.cpp src/logger.cpp \
  -o "$OUT" || exit 2

CONSOLE_OUT="tools/sim/fruitsnack_console.exe"
g++ -std=c++17 -g -O0 -Wall -Wextra -Wno-unused-parameter \
  -I tools/sim/shim -I src \
  -I .pio/libdeps/esp32s3_custom/ArduinoJson/src \
  tools/sim/console_test.cpp \
  src/ntc_sensor.cpp src/heater.cpp src/pid.cpp \
  src/telemetry.cpp src/heater_controller.cpp \
  src/logger.cpp src/settings.cpp src/network_manager.cpp src/console.cpp \
  src/state_json.cpp \
  -o "$CONSOLE_OUT" || exit 2

status=0
for t in sensor pid windup safety telemetry; do
  echo "================ $t ================"
  "./$OUT" "$t"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "TEST '$t' exited with $rc"
    status=1
  fi
done

echo "================ console ================"
"./$CONSOLE_OUT"
rc=$?
if [ $rc -ne 0 ]; then
  echo "TEST 'console' exited with $rc"
  status=1
fi

echo "overall_status=$status"
exit $status
