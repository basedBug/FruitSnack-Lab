// NTC thermistor acquisition for the FruitSnack hotend.
//
// Divider inversion + beta equation + median/EMA filtering.
//
//   3V3 -- Rpull -- node -- NTC -- GND        (the NTC is on the low side)
//   node = vref * Rntc / (Rpull + Rntc)
//   Rntc = Rpull * a / (1 - a),  a = node / vref
//   1/T  = 1/T1 + (1/beta) * ln(Rntc / R1)
//
// The maths is Klipper's (klippy/extras/thermistor.py) with the beta model,
// which is what Klipper uses for an NTC100K B3950.
#include "ntc_sensor.h"

#include <math.h>
#include <string.h>

#include "logger.h"

namespace {

constexpr float kKelvinOffset = 273.15f;
constexpr int kMedianWindow = NTC_MEDIAN_WINDOW > 0 ? NTC_MEDIAN_WINDOW : 1;

inline float clampf(float value, float low, float high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

}  // namespace

void NtcSensor::begin(uint8_t pin) {
  pin_ = pin;

  if (pin < 1 || pin > 10) {
    gLogger.info("ntc: GPIO%u is not an ADC1 pin (use GPIO1..10)",
                 (unsigned)pin);
  }

  // Attenuation MUST be set before the first analogReadMilliVolts(), otherwise
  // the core keeps a calibration handle built for the previous attenuation and
  // the millivolt values are wrong. On the ESP32-S3 the Arduino HAL keeps one
  // GLOBAL attenuation (`__analogAttenuation`) and builds the millivolt
  // calibration handle from it; the per-pin path does not touch the handle.
  // analogSetAttenuation() sets the hardware attenuation and rebuilds the
  // handle to match, so use that instead of analogSetPinAttenuation().
  analogSetAttenuation(ADC_11db);
  delayMicroseconds(200);
  uint16_t primed_raw = 0;
  last_mv_ = sampleNodeMv(primed_raw);  // one throw-away read to prime the ADC
  last_raw_ = primed_raw;
}

void NtcSensor::setConfig(const SensorConfig& cfg) {
  cfg_ = cfg;
  // A different pull-up changes the scale of the whole curve, so the smoother
  // must not blend readings from the old configuration into the new one.
  last_filtered_ = 0.0f;
  last_valid_ = 0.0f;
  have_filter_ = false;
}

const SensorConfig& NtcSensor::config() const { return cfg_; }

float NtcSensor::readMillivolts() const { return last_mv_; }

uint16_t NtcSensor::lastAdcRaw() const { return last_raw_; }

float NtcSensor::resistanceOhms() const { return last_resistance_; }

const char* NtcSensor::warning() const { return warning_; }

float NtcSensor::sampleNodeMv(uint16_t& rawOut) {
  const uint32_t mv = analogReadMilliVolts(pin_);
  rawOut = analogRead(pin_);
  return (float)mv;
}

bool NtcSensor::convertMv(float mv, uint16_t raw, float& celsiusOut) {
  const float vref = cfg_.adcReferenceV > 0.0f ? cfg_.adcReferenceV : 3.3f;
  const float rpull = cfg_.pullupOhms > 0.0f ? cfg_.pullupOhms : 4700.0f;
  const float beta = cfg_.beta > 0.0f ? cfg_.beta : 3950.0f;
  const float r0 = cfg_.nominalResistanceOhms > 0.0f
                       ? cfg_.nominalResistanceOhms
                       : 100000.0f;
  const float t1k = cfg_.nominalTempC + kKelvinOffset;

  warning_[0] = '\0';

  float vnode = mv / 1000.0f;
  if (!isfinite(vnode)) {
    strlcpy(warning_, "range", sizeof(warning_));
    last_resistance_ = NAN;
    return false;
  }

  // Hard wiring faults: the node sits against a rail.
  //
  // The thresholds must stay tight. With the NTC on the low side the node
  // voltage collapses as the thermistor heats up, so a wide "shorted"
  // threshold would report a perfectly healthy hot end as a broken sensor and
  // trip the controller's fault safety. A real short sits at 0 V and a real
  // open sits at the rail, so 0.5 % / 99.5 % separates them without touching
  // the legitimate operating range.
  if (vnode <= 0.005f * vref) {
    strlcpy(warning_, "short", sizeof(warning_));
    last_resistance_ = NAN;
    return false;
  }
  if (vnode >= 0.995f * vref) {
    strlcpy(warning_, "open", sizeof(warning_));
    last_resistance_ = NAN;
    return false;
  }

  // The calibrated value is only meaningful inside the ADC's specified range.
  // Above it the raw code has railed and the reported voltage is an
  // extrapolation of a pinned code (the curve-fitting calibration has no range
  // guard and can return anything), so the millivolt reading has to be thrown
  // away and the node clamped to the saturation point instead. The conversion
  // then yields the hottest temperature the node could possibly correspond to,
  // which is the honest upper bound for a cold hot end.
  const bool railed = raw >= NTC_ADC_RAIL_RAW;
  if (railed || vnode * 1000.0f > NTC_ADC_SATURATION_MV) {
    vnode = NTC_ADC_SATURATION_MV / 1000.0f;
  }

  // Divider inversion (NTC on the low side).
  const float fraction = clampf(vnode / vref, 1.0e-5f, 0.99999f);
  const float r_ntc = rpull * fraction / (1.0f - fraction);
  if (!(r_ntc > 0.0f) || !isfinite(r_ntc)) {
    strlcpy(warning_, "range", sizeof(warning_));
    last_resistance_ = NAN;
    return false;
  }
  last_resistance_ = r_ntc;

  // Klipper's beta model: 1/T = 1/T1 + (1/beta) * ln(R/R1).
  const float inv_t = 1.0f / t1k + (1.0f / beta) * logf(r_ntc / r0);
  if (!isfinite(inv_t) || inv_t <= 0.0f) {
    strlcpy(warning_, "range", sizeof(warning_));
    return false;
  }
  const float celsius = 1.0f / inv_t - kKelvinOffset;
  if (!isfinite(celsius) || celsius < SENSOR_MIN_VALID_C ||
      celsius > SENSOR_MAX_VALID_C) {
    strlcpy(warning_, "range", sizeof(warning_));
    return false;
  }
  celsiusOut = celsius;

  // Outside the ADC's calibrated range the reading is an upper bound only.
  // This is information, not a fault: with a 4.7k pull-up the node is above
  // 2.9 V below roughly 50 degC, and the hot end must still be able to heat.
  if (railed || vnode * 1000.0f > NTC_ADC_MAX_VALID_MV) {
    strlcpy(warning_, "below-range", sizeof(warning_));
  }
  return true;
}

float NtcSensor::readRawCelsius() {
  uint16_t raw = 0;
  const float mv = sampleNodeMv(raw);
  float celsius = 0.0f;
  const bool ok = convertMv(mv, raw, celsius);
  // Publish the pair together: the raw code must belong to the voltage read.
  last_mv_ = mv;
  last_raw_ = raw;
  faulted_ = !ok;
  return ok ? celsius : last_valid_;
}

float NtcSensor::readCelsius() {
  float mv[kMedianWindow];
  uint16_t raw[kMedianWindow];
  float temps[kMedianWindow];
  bool valid[kMedianWindow];
  int count = 0;

  for (int i = 0; i < kMedianWindow; ++i) {
    mv[i] = sampleNodeMv(raw[i]);
    float t = 0.0f;
    valid[i] = convertMv(mv[i], raw[i], t);
    temps[i] = t;
    if (valid[i]) ++count;
  }

  if (count == 0) {
    faulted_ = true;
    return last_valid_ + cfg_.offsetC;
  }

  // Median of the accepted samples (insertion sort; the window is tiny).
  float sorted[kMedianWindow];
  int n = 0;
  for (int i = 0; i < kMedianWindow; ++i) {
    if (!valid[i]) continue;
    const float key = temps[i];
    int j = n - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      --j;
    }
    sorted[j + 1] = key;
    ++n;
  }
  const float median = sorted[n / 2];

  // Cache the mV/raw pair closest to the median so the published diagnostics
  // (resistance, warning) match the temperature we report.
  int best = 0;
  float best_delta = 1.0e30f;
  for (int i = 0; i < kMedianWindow; ++i) {
    if (!valid[i]) continue;
    const float delta = fabsf(temps[i] - median);
    if (delta < best_delta) {
      best_delta = delta;
      best = i;
    }
  }
  last_mv_ = mv[best];
  last_raw_ = raw[best];
  // Re-run the conversion for the selected sample so the published diagnostics
  // match the median.
  float chosen = 0.0f;
  const bool diag_ok = convertMv(last_mv_, last_raw_, chosen);
  faulted_ = !diag_ok;

  const float alpha = (NTC_EMA_ALPHA > 0.0f && NTC_EMA_ALPHA <= 1.0f)
                          ? NTC_EMA_ALPHA
                          : 0.25f;
  if (!have_filter_) {
    last_filtered_ = median;
    have_filter_ = true;
  } else {
    last_filtered_ += alpha * (median - last_filtered_);
  }

  last_valid_ = last_filtered_;
  return last_filtered_ + cfg_.offsetC;
}

bool NtcSensor::faulted() const { return faulted_; }
