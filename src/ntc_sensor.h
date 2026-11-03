// NTC thermistor acquisition for the FruitSnack hotend.
//
// Fixed wiring: 3V3 -- pull-up -- ADC node -- NTC -- GND (the thermistor is on
// the low side). The node voltage is inverted into the thermistor resistance
// and converted with the beta equation, exactly as Klipper does for an
// NTC100K B3950 (klippy/extras/thermistor.py, setup_coefficients_beta).
//
// The ESP32-S3's calibrated ADC range ends at 2.9 V; above that the reading is
// undefined, which with a 4.7k pull-up happens below roughly 50 degC. That is
// reported as the "below-range" warning and NOT as a fault: a cold hot end
// must still be able to heat.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "app_state.h"
#include "config.h"

class NtcSensor {
 public:
  /// Configure the ADC pin (attenuation first, then one throw-away read).
  void begin(uint8_t pin);

  void setConfig(const SensorConfig& cfg);
  const SensorConfig& config() const;

  /// Last calibrated node voltage in millivolts (cached from the last read).
  float readMillivolts() const;

  /// Last raw ADC count (cached from the last read).
  uint16_t lastAdcRaw() const;

  /// One conversion on a single unfiltered sample (no offset applied).
  float readRawCelsius();

  /// Median(N) + EMA filtered temperature with offsetC applied.
  float readCelsius();

  /// True when the last read was open/short/out of range.
  bool faulted() const;

  /// Last computed NTC resistance in ohms (NaN before the first read).
  float resistanceOhms() const;

  /// "" when healthy, otherwise "short", "open", "range" or "below-range".
  const char* warning() const;

 private:
  /// One (millivolt, raw code) pair from two back-to-back conversions. The
  /// values describe the same sample and are published together by the read
  /// entry points, never one without the other.
  float sampleNodeMv(uint16_t& rawOut);

  /// Divider + beta equation. Updates the diagnostics and the fault flag; the
  /// returned temperature is only meaningful when the function returns true.
  /// `raw` is the raw code of the same sample as `mv`: the "railed" test and
  /// the voltage it gates must describe one conversion, not two.
  bool convertMv(float mv, uint16_t raw, float& celsiusOut);

  uint8_t pin_ = PIN_NTC;
  SensorConfig cfg_{};

  float last_mv_ = 0.0f;
  uint16_t last_raw_ = 0;
  float last_resistance_ = NAN;
  float last_filtered_ = 0.0f;
  float last_valid_ = 0.0f;
  bool have_filter_ = false;
  bool faulted_ = false;

  char warning_[32] = {0};
};
