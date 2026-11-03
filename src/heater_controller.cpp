// Implementation of the hotend control core (see heater_controller.h).
//
// Owns the NTC sensor, the LEDC heater output, the PID and the telemetry
// ring. loop() is the single writer of the control state; every other entry
// point reads or mutates it under a short FreeRTOS mutex, so a stalled
// consumer can never block the control task for more than ~20 ms.
//
// Safety order inside loop(): sensor fault -> overtemp -> mode.
// A latched fault or overtemp always forces the heater off first.
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"
#include "heater.h"
#include "heater_controller.h"
#include "logger.h"
#include "ntc_sensor.h"
#include "pid.h"
#include "telemetry.h"

HeaterController gController;

namespace {

constexpr uint32_t kLockTimeoutMs = 20;
constexpr size_t kTelemetryCapacity = 900;
constexpr float kOvertempClearMarginC = 5.0f;
constexpr float kMinSetpointC = 0.0f;

inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// The control task never formats a float with %f: callers hand in fixed-point
// text instead. The lines go straight to the logger, which is what the old
// event-sink fan-out did.
void emit(const char* msg) {
  if (msg != nullptr) gLogger.info("%s", msg);
}

void emitf(const char* fmt, ...) {
  if (fmt == nullptr) return;
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  gLogger.info("%s", buf);
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

void HeaterController::fmtTemp(char* out, size_t n, float celsius) {
  const long t = lroundf(celsius * 10.0f);
  snprintf(out, n, "%ld.%ld", t / 10, labs(t % 10));
}

void HeaterController::fmtX100(char* out, size_t n, float v) {
  const long x = lroundf(v * 100.0f);
  snprintf(out, n, "%s%ld.%02ld", x < 0 ? "-" : "", labs(x / 100), labs(x % 100));
}

// ---------------------------------------------------------------- locking

bool HeaterController::lock() const {
  return mutex_ != nullptr &&
         xSemaphoreTake(mutex_, pdMS_TO_TICKS(kLockTimeoutMs)) == pdTRUE;
}

void HeaterController::unlock() const {
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
}

void HeaterController::configureSensor() {
  SensorConfig sc;
  sc.nominalResistanceOhms = NTC_NOMINAL_RESISTANCE_OHMS;
  sc.nominalTempC = NTC_NOMINAL_TEMP_C;
  sc.beta = NTC_BETA;
  sc.pullupOhms =
      (isfinite(settings_.ntcPullupOhms) && settings_.ntcPullupOhms > 0.0f)
          ? settings_.ntcPullupOhms
          : NTC_PULLUP_OHMS;
  sc.adcReferenceV = NTC_ADC_REFERENCE_V;
  sc.adcMax = NTC_ADC_MAX;
  sc.offsetC = settings_.sensorOffsetC;

  sensor_cfg_ = sc;
  sensor_.setConfig(sensor_cfg_);
}

HeaterController::HeaterController() { mutex_ = xSemaphoreCreateMutex(); }

HeaterController::~HeaterController() {
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
}

void HeaterController::begin(const AppSettings& settings) {
  if (!lock()) return;

  settings_ = settings;

  const float max_default = HEATER_MAX_TEMPERATURE_C;
  if (!isfinite(settings_.maxTemperatureC) ||
      settings_.maxTemperatureC < SENSOR_MIN_VALID_C + 20.0f ||
      settings_.maxTemperatureC > SENSOR_MAX_VALID_C) {
    settings_.maxTemperatureC = max_default;
  }
  if (!isfinite(settings_.outputLimitPct)) settings_.outputLimitPct = 100.0f;
  settings_.outputLimitPct = clampf(settings_.outputLimitPct, 0.0f, 100.0f);
  if (!isfinite(settings_.sensorOffsetC)) settings_.sensorOffsetC = 0.0f;

  configureSensor();
  sensor_.begin(PIN_NTC);

  heater_.begin(PIN_HEATER_PWM, HEATER_PWM_FREQ_HZ, HEATER_PWM_BITS);
  heater_.off();

  pid_.configure(settings_.gains.kp, settings_.gains.ki, settings_.gains.kd);
  pid_.setOutputLimits(0.0f, settings_.outputLimitPct);
  pid_.setIntegralLimit(PID_INTEGRAL_LIMIT);
  pid_.setDerivativeFilter(0.2f);
  pid_.reset();

  setpoint_c_ = isfinite(settings_.setpointC) ? settings_.setpointC : 0.0f;
  setpoint_c_ = clampf(setpoint_c_, kMinSetpointC, settings_.maxTemperatureC);
  settings_.setpointC = setpoint_c_;
  manual_duty_ = 0.0f;

  mode_ = ControlMode::Off;
  state_ = HeaterState::Idle;
  output_pct_ = 0.0f;
  sensor_fault_ = false;
  overtemp_latch_ = false;
  prev_fault_ = false;

  last_known_mode_ = mode_;
  last_known_setpoint_c_ = setpoint_c_;
  last_known_gains_ = settings_.gains;

  last_loop_ms_ = millis();
  have_last_loop_ = false;
  last_telemetry_ms_ = last_loop_ms_;
  have_telemetry_ = false;
  hz_window_ms_ = last_loop_ms_;
  hz_count_ = 0;
  loop_hz_ = 0;

  telemetry_.begin(kTelemetryCapacity);

  char t[16];
  fmtTemp(t, sizeof(t), settings_.maxTemperatureC);
  emitf("boot: ntc=GPIO%u heater=GPIO%u pwm=%luHz/%ubit max=%sC",
        (unsigned)PIN_NTC, (unsigned)PIN_HEATER_PWM,
        (unsigned long)HEATER_PWM_FREQ_HZ, (unsigned)HEATER_PWM_BITS, t);
  if (!heater_.ready()) {
    emit("heater: PWM attach failed, output disabled");
  }

  unlock();
}

void HeaterController::pushTelemetry(uint32_t nowMs) {
  if (have_telemetry_ && millisElapsed(nowMs, last_telemetry_ms_) <
                             (uint32_t)TELEMETRY_PERIOD_MS) {
    return;
  }
  telemetry_.push(nowMs, temperature_c_, setpoint_c_, output_pct_,
                  (float)heater_.dutyCount());
  last_telemetry_ms_ = nowMs;
  have_telemetry_ = true;
}

void HeaterController::loop(uint32_t nowMs) {
  if (!lock()) {
    // Fail safe. Skipping the cycle would leave the last duty applied while the
    // overtemp check, the PID step and the telemetry all go stale, so the
    // output is forced off instead and the cycle is abandoned - no retry, no
    // blocking (the mutex may be held by a starved consumer).
    heater_.setDutyPercent(0.0f);
    state_ = HeaterState::Fault;
    emit("control: mutex timeout");
    return;
  }

  // --- 0. timing ---------------------------------------------------------
  float measured_dt = (float)CONTROL_PERIOD_MS / 1000.0f;
  if (have_last_loop_) {
    const uint32_t elapsed = millisElapsed(nowMs, last_loop_ms_);
    if (elapsed > 0 && elapsed <= 5000) {
      measured_dt = (float)elapsed / 1000.0f;
    }
  }
  last_loop_ms_ = nowMs;
  have_last_loop_ = true;

  // --- 1. acquisition ----------------------------------------------------
  temperature_raw_c_ = sensor_.readRawCelsius();
  temperature_c_ = sensor_.readCelsius();
  adc_mv_ = sensor_.readMillivolts();
  adc_raw_ = sensor_.lastAdcRaw();
  sensor_fault_ = sensor_.faulted();

  ntc_resistance_ = sensor_.resistanceOhms();
  strlcpy(ntc_warn_, sensor_.warning(), sizeof(ntc_warn_));

  // --- 2. safety first ---------------------------------------------------
  if (sensor_fault_) {
    if (!prev_fault_) {
      emitf("sensor fault: mv=%ld raw=%u", lroundf(adc_mv_),
            (unsigned)adc_raw_);
      prev_fault_ = true;
    }
    heater_.off();
    output_pct_ = 0.0f;
    pid_.reset();
    state_ = HeaterState::Fault;
    pushTelemetry(nowMs);
    unlock();
    return;
  }
  if (prev_fault_) {
    prev_fault_ = false;
    emit("sensor recovered");
  }

  if (temperature_c_ > settings_.maxTemperatureC) {
    if (!overtemp_latch_) {
      char t[16];
      char m[16];
      fmtTemp(t, sizeof(t), temperature_c_);
      fmtTemp(m, sizeof(m), settings_.maxTemperatureC);
      emitf("overtemp latch: %sC > %sC", t, m);
      overtemp_latch_ = true;
    }
  } else if (overtemp_latch_ &&
             temperature_c_ < settings_.maxTemperatureC - kOvertempClearMarginC) {
    overtemp_latch_ = false;
    emit("overtemp cleared");
  }

  if (overtemp_latch_) {
    heater_.off();
    output_pct_ = 0.0f;
    pid_.reset();
    state_ = HeaterState::Overtemp;
    pushTelemetry(nowMs);
    unlock();
    return;
  }

  // --- 3..5. mode --------------------------------------------------------
  switch (mode_) {
    case ControlMode::Pid:
      // measured_dt, not the nominal period: the integral must not be wrong
      // when a cycle runs late. It is clamped to 0..5 s above.
      output_pct_ = pid_.compute(setpoint_c_, temperature_c_, measured_dt);
      break;
    case ControlMode::Manual:
      output_pct_ = clampf(manual_duty_, 0.0f, 100.0f);
      break;
    case ControlMode::Off:
    default:
      output_pct_ = 0.0f;
      pid_.reset();
      break;
  }
  if (!isfinite(output_pct_)) output_pct_ = 0.0f;
  output_pct_ = clampf(output_pct_, 0.0f, 100.0f);

  // --- 7. apply + coarse state -------------------------------------------
  heater_.setDutyPercent(output_pct_);
  if (output_pct_ > 0.01f && temperature_c_ < setpoint_c_) {
    state_ = HeaterState::Heating;
  } else if (output_pct_ <= 0.01f && temperature_c_ > setpoint_c_) {
    state_ = HeaterState::Cooling;
  } else {
    state_ = HeaterState::Idle;
  }

  // --- 8. diagnostics ----------------------------------------------------
  pushTelemetry(nowMs);

  ++hz_count_;
  const uint32_t span = millisElapsed(nowMs, hz_window_ms_);
  if (span >= 1000) {
    loop_hz_ = (uint32_t)((uint64_t)hz_count_ * 1000ull / span);
    hz_count_ = 0;
    hz_window_ms_ = nowMs;
  }

  unlock();
}

SystemState HeaterController::makeSnapshot() const {
  SystemState st;
  st.mode = mode_;
  st.state = state_;
  st.setpointC = setpoint_c_;
  st.temperatureC = temperature_c_;
  st.temperatureRawC = temperature_raw_c_;
  st.outputPct = output_pct_;
  st.duty = (float)heater_.dutyCount();

  st.overtemp = overtemp_latch_;
  st.sensorFault = sensor_fault_;
  st.pwmReady = heater_.ready();
  st.maxTemperatureC = settings_.maxTemperatureC;

  st.gains = settings_.gains;
  st.pTerm = pid_.pTerm();
  st.iTerm = pid_.iTerm();
  st.dTerm = pid_.dTerm();
  st.pidSaturated = pid_.saturated();

  st.adcMillivolts = adc_mv_;
  st.adcRaw = (float)adc_raw_;
  st.controlLoopHz = loop_hz_;

  st.ntcResistance = isfinite(ntc_resistance_) ? ntc_resistance_ : 0.0f;
  strlcpy(st.ntcWarn, ntc_warn_, sizeof(st.ntcWarn));
  return st;
}

// ---------------------------------------------------------------- control

void HeaterController::setMode(ControlMode mode) {
  if (!lock()) return;

  if (mode != ControlMode::Off) {
    if (sensor_fault_) {
      emit("mode refused: sensor fault");
      unlock();
      return;
    }
    if (overtemp_latch_) {
      emit("mode refused: overtemp latched");
      unlock();
      return;
    }
  }

  if (mode != mode_) {
    mode_ = mode;
    last_known_mode_ = mode;
    pid_.reset();
    if (mode == ControlMode::Off) {
      heater_.off();
      output_pct_ = 0.0f;
    }
  }
  unlock();
}

ControlMode HeaterController::getMode() const {
  // A lock timeout returns the last value known under the lock, never the
  // misleading "Off" default.
  if (!lock()) return last_known_mode_;
  const ControlMode m = mode_;
  unlock();
  return m;
}

void HeaterController::setSetpoint(float celsius) {
  if (!lock()) return;
  if (!isfinite(celsius)) celsius = 0.0f;
  setpoint_c_ = clampf(celsius, kMinSetpointC, settings_.maxTemperatureC);
  settings_.setpointC = setpoint_c_;
  last_known_setpoint_c_ = setpoint_c_;
  unlock();
}

float HeaterController::getSetpoint() const {
  if (!lock()) return last_known_setpoint_c_;
  const float v = setpoint_c_;
  unlock();
  return v;
}

void HeaterController::setManualDuty(float percent) {
  if (!lock()) return;
  if (!isfinite(percent)) percent = 0.0f;
  manual_duty_ = clampf(percent, 0.0f, 100.0f);
  unlock();
}

float HeaterController::getManualDuty() const {
  if (!lock()) return 0.0f;
  const float v = manual_duty_;
  unlock();
  return v;
}

void HeaterController::setGains(const PidGains& gains) {
  if (!lock()) return;
  settings_.gains = gains;
  last_known_gains_ = gains;
  pid_.configure(gains.kp, gains.ki, gains.kd);
  pid_.reset();
  unlock();
}

PidGains HeaterController::getGains() const {
  if (!lock()) return last_known_gains_;
  const PidGains g = settings_.gains;
  unlock();
  return g;
}

void HeaterController::setOutputLimit(float percent) {
  if (!lock()) return;
  if (!isfinite(percent)) percent = 100.0f;
  percent = clampf(percent, 0.0f, 100.0f);
  settings_.outputLimitPct = percent;
  pid_.setOutputLimits(0.0f, percent);
  unlock();
}

float HeaterController::getOutputLimit() const {
  if (!lock()) return 0.0f;
  const float v = settings_.outputLimitPct;
  unlock();
  return v;
}

void HeaterController::setMaxTemperature(float celsius) {
  if (!lock()) return;
  if (!isfinite(celsius)) return;
  const float lo = SENSOR_MIN_VALID_C + 20.0f;
  const float hi = SENSOR_MAX_VALID_C;
  settings_.maxTemperatureC = clampf(celsius, lo, hi);
  if (setpoint_c_ > settings_.maxTemperatureC) {
    setpoint_c_ = settings_.maxTemperatureC;
    settings_.setpointC = setpoint_c_;
    last_known_setpoint_c_ = setpoint_c_;
  }
  unlock();
}

float HeaterController::getMaxTemperature() const {
  if (!lock()) return 0.0f;
  const float v = settings_.maxTemperatureC;
  unlock();
  return v;
}

void HeaterController::setSensorOffset(float celsius) {
  if (!lock()) return;
  if (!isfinite(celsius)) celsius = 0.0f;
  settings_.sensorOffsetC = celsius;
  configureSensor();
  unlock();
}

void HeaterController::applySettings(const AppSettings& settings) {
  if (!lock()) return;

  settings_ = settings;
  if (!isfinite(settings_.maxTemperatureC) ||
      settings_.maxTemperatureC < SENSOR_MIN_VALID_C + 20.0f ||
      settings_.maxTemperatureC > SENSOR_MAX_VALID_C) {
    settings_.maxTemperatureC = HEATER_MAX_TEMPERATURE_C;
  }
  if (!isfinite(settings_.outputLimitPct)) {
    settings_.outputLimitPct = 100.0f;
  }
  settings_.outputLimitPct = clampf(settings_.outputLimitPct, 0.0f, 100.0f);
  if (!isfinite(settings_.sensorOffsetC)) {
    settings_.sensorOffsetC = 0.0f;
  }

  setpoint_c_ = isfinite(settings_.setpointC) ? settings_.setpointC : 0.0f;
  setpoint_c_ = clampf(setpoint_c_, kMinSetpointC, settings_.maxTemperatureC);
  settings_.setpointC = setpoint_c_;

  pid_.configure(settings_.gains.kp, settings_.gains.ki, settings_.gains.kd);
  pid_.setOutputLimits(0.0f, settings_.outputLimitPct);
  pid_.reset();

  last_known_setpoint_c_ = setpoint_c_;
  last_known_gains_ = settings_.gains;

  // A settings change may alter the pull-up or the offset, so remap the whole
  // SensorConfig.
  configureSensor();

  unlock();
}

AppSettings HeaterController::getSettings() const {
  AppSettings s;
  if (!lock()) return s;
  s = settings_;
  unlock();
  return s;
}

// -------------------------------------------------------------- telemetry

SystemState HeaterController::snapshot() const {
  SystemState st;

  if (lock()) {
    st = makeSnapshot();
    unlock();
  }

  st.uptimeMs = millis();
  st.freeHeap = ESP.getFreeHeap();
  st.minFreeHeap = ESP.getMinFreeHeap();

  const uint32_t heap_size = ESP.getHeapSize();
  const uint32_t max_alloc = ESP.getMaxAllocHeap();
  if (heap_size > 0) {
    const uint32_t free_pct = (uint32_t)((uint64_t)max_alloc * 100u / heap_size);
    st.heapFragmentation = (uint8_t)(free_pct >= 100 ? 0 : 100 - free_pct);
  } else {
    st.heapFragmentation = 0;
  }

  // WiFi fields are owned by the network layer; left untouched here.
  return st;
}

size_t HeaterController::copyTelemetry(TelemetrySample* out,
                                       size_t maxSamples) const {
  if (!lock()) return 0;
  const size_t n = telemetry_.copyRecent(out, maxSamples);
  unlock();
  return n;
}

void HeaterController::clearTelemetry() {
  if (!lock()) return;
  telemetry_.clear();
  unlock();
}
