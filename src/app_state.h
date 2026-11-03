// Shared data contracts used by the control core, the web UI, the UART console
// and the telemetry graph. Everything here is POD / trivially copyable so it
// can be safely memcpy'd out of a task under a lock.
#pragma once

#include <Arduino.h>
#include <stdint.h>

/// Operating mode of the hotend controller.
enum class ControlMode : uint8_t {
  Off = 0,    ///< heater forced off
  Pid = 1,    ///< closed-loop PID control
  Manual = 2, ///< open-loop duty command
};

/// Coarse heater state, useful for UI badges and log lines.
enum class HeaterState : uint8_t {
  Idle = 0,
  Heating = 1,
  Cooling = 2,
  Overtemp = 3,
  Fault = 4,
};

/// PID gains.
struct PidGains {
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
};

/// Sensor electrical configuration.
///
/// The wiring is fixed: 3V3 -- pull-up -- ADC node -- NTC -- GND (the NTC is on
/// the low side). Only the pull-up value and the calibration offset are
/// adjustable, because they are the only two things that change the scale of
/// the temperature curve.
struct SensorConfig {
  float nominalResistanceOhms = 100000.0f; ///< R at nominalTempC
  float nominalTempC = 25.0f;
  float beta = 3950.0f;                   ///< NTC100K B3950
  float pullupOhms = 4700.0f;             ///< 3V3 -> node resistor
  float adcReferenceV = 3.3f;
  float adcMax = 4095.0f;
  float offsetC = 0.0f; ///< calibration trim applied to the reading
};

/// Persisted user configuration.
struct AppSettings {
  char wifiSsid[33] = {0};
  char wifiPassword[65] = {0};
  char hostname[32] = "fruitsnack";
  PidGains gains{0.0f, 0.0f, 0.0f};
  float setpointC = 0.0f;
  float maxTemperatureC = 250.0f;
  float outputLimitPct = 100.0f;
  uint32_t controlPeriodMs = 50;
  bool pidEnabled = true;
  float sensorOffsetC = 0.0f;
  float ntcPullupOhms = 4700.0f;
};

/// One graph sample.
struct TelemetrySample {
  uint32_t tMs = 0;
  float temperatureC = 0.0f;
  float setpointC = 0.0f;
  float outputPct = 0.0f;
  float duty = 0.0f;
};

/// Full snapshot of the running system, safe to serialise at any time.
struct SystemState {
  // control
  ControlMode mode = ControlMode::Off;
  HeaterState state = HeaterState::Idle;
  float setpointC = 0.0f;
  float temperatureC = 0.0f;
  float temperatureRawC = 0.0f;
  float outputPct = 0.0f;
  float duty = 0.0f;

  // safety
  bool overtemp = false;
  bool sensorFault = false;
  bool pwmReady = false;  ///< false when the LEDC channel could not be attached
  float maxTemperatureC = 0.0f;

  // pid
  PidGains gains{0, 0, 0};
  float pTerm = 0.0f;
  float iTerm = 0.0f;
  float dTerm = 0.0f;
  bool pidSaturated = false;

  // diagnostics
  uint32_t uptimeMs = 0;
  uint32_t freeHeap = 0;
  uint32_t minFreeHeap = 0;
  uint8_t heapFragmentation = 0;
  uint32_t wifiRssi = 0;
  char ipAddress[16] = {0};
  char macAddress[18] = {0};
  float adcMillivolts = 0.0f;
  float adcRaw = 0.0f;
  uint32_t controlLoopHz = 0;
  float ntcResistance = 0.0f;  ///< ohms, computed from the divider
  char ntcWarn[32] = {0};      ///< "" when healthy, else a short code
};

/// Helper: human readable name for a control mode.
inline const char *controlModeName(ControlMode mode) {
  switch (mode) {
    case ControlMode::Off: return "off";
    case ControlMode::Pid: return "pid";
    case ControlMode::Manual: return "manual";
  }
  return "unknown";
}

/// Helper: human readable name for a heater state.
inline const char *heaterStateName(HeaterState state) {
  switch (state) {
    case HeaterState::Idle: return "idle";
    case HeaterState::Heating: return "heating";
    case HeaterState::Cooling: return "cooling";
    case HeaterState::Overtemp: return "overtemp";
    case HeaterState::Fault: return "fault";
  }
  return "unknown";
}
