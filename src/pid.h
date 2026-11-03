// Positional PID controller for the hotend heater, output in percent.
//
// Derivative acts on the measurement (no setpoint kick), the derivative term is
// low-pass filtered, and the integrator is clamped and frozen while the output
// is saturated so a cold start does not wind up.
#pragma once

#include <Arduino.h>

class PidController {
 public:
  void configure(float kp, float ki, float kd);
  void setOutputLimits(float minPct, float maxPct);

  /// Symmetric clamp for the integral term, in output percent.
  void setIntegralLimit(float limitPct);

  /// EMA weight of the new derivative sample (0..1, 1 = no filtering).
  void setDerivativeFilter(float alpha);

  /// Clear integrator + derivative history.
  void reset();

  /// One step. dtSeconds <= 0 keeps the previous state untouched.
  float compute(float setpoint, float measurement, float dtSeconds);

  float pTerm() const { return p_; }
  float iTerm() const { return i_; }
  float dTerm() const { return d_; }
  float kp() const { return kp_; }
  float ki() const { return ki_; }
  float kd() const { return kd_; }
  float output() const { return output_; }
  bool saturated() const { return saturated_; }

 private:
  float kp_ = 0.0f;
  float ki_ = 0.0f;
  float kd_ = 0.0f;

  float out_min_ = 0.0f;
  float out_max_ = 100.0f;
  float integral_limit_ = 100.0f;
  float deriv_alpha_ = 0.2f;

  float integral_ = 0.0f;
  float p_ = 0.0f;
  float i_ = 0.0f;
  float d_ = 0.0f;
  float d_filtered_ = 0.0f;
  float prev_measurement_ = 0.0f;
  float output_ = 0.0f;
  bool have_prev_ = false;
  bool saturated_ = false;
};
