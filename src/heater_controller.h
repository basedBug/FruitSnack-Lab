// Public control interface for the hotend.
//
// This header is the contract between the control core (sensor, heater, PID)
// and every consumer (web server, UART console, main task).
// Implementation lives in heater_controller.cpp.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "app_state.h"
#include "config.h"
#include "heater.h"
#include "ntc_sensor.h"
#include "pid.h"
#include "telemetry.h"

/// Controller for the NTC + heater + PID subsystem.
///
/// Thread-safety: every public method may be called from any task; the
/// control loop owns the mutable control state and swaps it under a short
/// critical section, so snapshots are always internally consistent.
class HeaterController {
 public:
  HeaterController();
  ~HeaterController();

  // The state lives in the object now, mutex handle included: a copy would
  // share the semaphore and free it twice.
  HeaterController(const HeaterController&) = delete;
  HeaterController& operator=(const HeaterController&) = delete;

  /// Bring up the sensor and heater outputs and apply the initial settings.
  void begin(const AppSettings& settings);

  /// Run one iteration of the control loop. Call every CONTROL_PERIOD_MS.
  void loop(uint32_t nowMs);

  // ------------------------------------------------------------- control
  void setMode(ControlMode mode);
  ControlMode getMode() const;

  void setSetpoint(float celsius);
  float getSetpoint() const;

  /// Open-loop duty command in percent (0..100), clamped.
  void setManualDuty(float percent);
  float getManualDuty() const;

  void setGains(const PidGains& gains);
  PidGains getGains() const;

  /// Clamp applied to the PID output (percent).
  void setOutputLimit(float percent);
  float getOutputLimit() const;

  /// Safety cut-off temperature; the heater is forced off above it.
  void setMaxTemperature(float celsius);
  float getMaxTemperature() const;

  /// Calibration trim added to every sensor reading.
  void setSensorOffset(float celsius);

  /// Bulk-apply persisted settings (gains, setpoint, limits, sensor offset).
  void applySettings(const AppSettings& settings);
  AppSettings getSettings() const;

  // ------------------------------------------------------------- telemetry
  /// Consistent snapshot of the running system.
  SystemState snapshot() const;

  /// Copy up to `maxSamples` graph samples, oldest first.
  /// Returns the number of samples written.
  size_t copyTelemetry(TelemetrySample* out, size_t maxSamples) const;
  void clearTelemetry();

 private:
  // ------------------------------------------------------------- locking
  /// Take the control mutex; false when it could not be taken in time.
  bool lock() const;
  void unlock() const;

  // ------------------------------------------------------------- helpers
  SystemState makeSnapshot() const;
  void pushTelemetry(uint32_t nowMs);
  void configureSensor();

  static void fmtTemp(char* out, size_t n, float celsius);
  static void fmtX100(char* out, size_t n, float v);

  // --------------------------------------------------------------- state
  NtcSensor sensor_;
  Heater heater_;
  PidController pid_;
  Telemetry telemetry_;

  AppSettings settings_{};
  SensorConfig sensor_cfg_{};
  SemaphoreHandle_t mutex_ = nullptr;

  ControlMode mode_ = ControlMode::Off;
  HeaterState state_ = HeaterState::Idle;
  float setpoint_c_ = 0.0f;
  float manual_duty_ = 0.0f;

  // Last values known under the lock. The getters return these when the mutex
  // times out: a timeout must not look like "mode off" or "gains zero". They
  // are written wherever their source field changes.
  ControlMode last_known_mode_ = ControlMode::Off;
  float last_known_setpoint_c_ = 0.0f;
  PidGains last_known_gains_{};

  float temperature_c_ = 0.0f;
  float temperature_raw_c_ = 0.0f;
  float output_pct_ = 0.0f;
  float adc_mv_ = 0.0f;
  uint16_t adc_raw_ = 0;
  float ntc_resistance_ = 0.0f;
  char ntc_warn_[32] = {0};

  bool sensor_fault_ = false;
  bool overtemp_latch_ = false;
  bool prev_fault_ = false;

  uint32_t last_loop_ms_ = 0;
  bool have_last_loop_ = false;
  uint32_t last_telemetry_ms_ = 0;
  bool have_telemetry_ = false;
  uint32_t hz_window_ms_ = 0;
  uint32_t hz_count_ = 0;
  uint32_t loop_hz_ = 0;
};

/// The one controller.  Every module talks to it through this handle.
extern HeaterController gController;
