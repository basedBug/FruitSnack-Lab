# Host-side simulation of the FruitSnack control core.
#
# Compiles the real firmware control sources against the Arduino shim in
# tools/sim/shim and runs the tests in tools/sim/sim_main.cpp.
# This is a development/verification aid; it is NOT part of the firmware.
#
# Usage:  tools\sim\run_sim.ps1

$ErrorActionPreference = 'Stop'
# $PSScriptRoot is tools\sim, so the project root is two directories up.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$out = Join-Path $root 'tools\sim\fruitsnack_sim.exe'
$sources = @(
  'tools\sim\sim_main.cpp',
  'src\ntc_sensor.cpp',
  'src\heater.cpp',
  'src\pid.cpp',
  'src\telemetry.cpp',
  'src\heater_controller.cpp',
  'src\logger.cpp'
)

$missing = $sources | Where-Object { -not (Test-Path $_) }
if ($missing) {
  Write-Host "Missing source files: $($missing -join ', ')" -ForegroundColor Red
  exit 2
}

Write-Host "Compiling the control-core simulation..." -ForegroundColor Cyan
& g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter `
  -I 'tools\sim\shim' -I 'src' `
  $sources -o $out
if ($LASTEXITCODE -ne 0) {
  Write-Host "Compilation failed." -ForegroundColor Red
  exit $LASTEXITCODE
}

Write-Host "Running the simulation..." -ForegroundColor Cyan
& $out
exit $LASTEXITCODE
