// LEDC PWM heater output for the FruitSnack hotend.
//
// Wraps ledcAttach/ledcWrite and works in raw timer counts (0 .. (1<<bits)-1).
// If the channel cannot be attached the heater is left permanently de-energised
// so a PWM failure can never leave the MOSFET stuck on.
#pragma once

#include <Arduino.h>
#include <stdint.h>

class Heater {
 public:
  void begin(uint8_t pin, uint32_t freqHz, uint8_t resolutionBits);

  /// Duty in percent, clamped to 0..100.
  void setDutyPercent(float percent);
  float dutyPercent() const;

  /// Raw LEDC compare value currently written.
  uint32_t dutyCount() const;

  /// Largest count for the configured resolution: (1<<bits)-1.
  uint32_t maxDuty() const;

  void off();

  /// True when a non-zero duty is applied to the pin.
  bool active() const;

  /// False when ledcAttach() failed; the output is forced to zero in that case.
  bool ready() const;

 private:
  uint32_t toCount(float percent) const;

  uint8_t pin_ = 0;
  uint32_t freq_hz_ = 0;
  uint8_t bits_ = 0;
  uint32_t max_count_ = 0;
  uint32_t count_ = 0;
  float percent_ = 0.0f;
  bool ready_ = false;
};
