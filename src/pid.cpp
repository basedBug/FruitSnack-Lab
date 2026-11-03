// Positional PID controller for the hotend heater, output in percent.
//
// error   = setpoint - measurement
// P       = kp * error
// I       = sum(ki * error * dt), clamped to +/- integral limit
// D       = EMA(-kd * d(measurement)/dt)   (no derivative kick)
// output  = clamp(P + I + D, [min, max])
#include "pid.h"

#include <math.h>

namespace {
inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace

void PidController::configure(float kp, float ki, float kd) {
  kp_ = isfinite(kp) ? kp : 0.0f;
  ki_ = isfinite(ki) ? ki : 0.0f;
  kd_ = isfinite(kd) ? kd : 0.0f;
}

void PidController::setOutputLimits(float minPct, float maxPct) {
  if (!isfinite(minPct)) minPct = 0.0f;
  if (!isfinite(maxPct)) maxPct = 100.0f;
  if (maxPct < minPct) {
    const float t = minPct;
    minPct = maxPct;
    maxPct = t;
  }
  out_min_ = minPct;
  out_max_ = maxPct;
  output_ = clampf(output_, out_min_, out_max_);
}

void PidController::setIntegralLimit(float limitPct) {
  integral_limit_ = fabsf(isfinite(limitPct) ? limitPct : 0.0f);
  integral_ = clampf(integral_, -integral_limit_, integral_limit_);
}

void PidController::setDerivativeFilter(float alpha) {
  if (!isfinite(alpha)) alpha = 0.2f;
  deriv_alpha_ = clampf(alpha, 0.0f, 1.0f);
}

void PidController::reset() {
  integral_ = 0.0f;
  p_ = 0.0f;
  i_ = 0.0f;
  d_ = 0.0f;
  d_filtered_ = 0.0f;
  prev_measurement_ = 0.0f;
  output_ = 0.0f;
  have_prev_ = false;
  saturated_ = false;
}

float PidController::compute(float setpoint, float measurement, float dtSeconds) {
  // A stalled or bogus time step must not corrupt the integrator or the
  // derivative: hold the previous output instead.
  if (!(dtSeconds > 0.0f) || !isfinite(dtSeconds) || !isfinite(measurement) ||
      !isfinite(setpoint)) {
    return output_;
  }

  const float error = setpoint - measurement;
  p_ = kp_ * error;

  // Derivative on the measurement, so a setpoint step produces no kick.
  float d_raw = 0.0f;
  if (have_prev_) {
    d_raw = -kd_ * (measurement - prev_measurement_) / dtSeconds;
  }
  d_filtered_ = deriv_alpha_ * d_raw + (1.0f - deriv_alpha_) * d_filtered_;
  d_ = d_filtered_;
  prev_measurement_ = measurement;
  have_prev_ = true;

  // Anti-windup: only integrate when the output is not pinned in the direction
  // the error is pushing it.
  const float pre = p_ + integral_ + d_;
  const bool block_up = (pre >= out_max_ && error > 0.0f);
  const bool block_down = (pre <= out_min_ && error < 0.0f);
  if (!block_up && !block_down) {
    integral_ += ki_ * error * dtSeconds;
    integral_ = clampf(integral_, -integral_limit_, integral_limit_);
  }
  i_ = integral_;

  const float raw = p_ + integral_ + d_;
  saturated_ = (raw > out_max_ || raw < out_min_);
  output_ = clampf(raw, out_min_, out_max_);
  return output_;
}
