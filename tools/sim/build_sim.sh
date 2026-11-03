#!/usr/bin/env bash
# Build and run the FruitSnack control-core simulation (host build).
# Run from an MSYS2 ucrt64 shell, e.g.
#   C:\msys64\ucrt64\bin\bash.exe tools/sim/build_sim.sh
set -euo pipefail

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

CXX="${CXX:-g++}"
OUT="tools/sim/fruitsnack_sim.exe"

echo "Compiling the control-core simulation..."
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
  -I tools/sim/shim -I src \
  tools/sim/sim_main.cpp \
  src/ntc_sensor.cpp \
  src/heater.cpp \
  src/pid.cpp \
  src/telemetry.cpp \
  src/heater_controller.cpp \
  src/logger.cpp \
  -o "$OUT"

echo "Running the simulation..."
"$OUT"
