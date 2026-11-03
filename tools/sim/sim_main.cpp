// FruitSnack control-core simulation (host build).
//
// Compiles the REAL firmware control sources (src/ntc_sensor.cpp, src/heater.cpp,
// src/pid.cpp, src/telemetry.cpp, src/heater_controller.cpp) against a small
// Arduino shim and exercises them with a synthetic thermal plant.
//
// It answers the questions that cannot be answered without hardware:
//   1. Does the fixed 4.7k low-side NTC100K/B3950 divider + beta maths invert
//      correctly, and are the short/open/range/below-range codes right?
//   2. Does the PID converge, stay stable and avoid integrator windup?
//   3. Does the safety logic (overtemp / sensor fault) actually cut the heater?
//
// The wiring is fixed:  3V3 -- 4.7k -- node -- NTC100K -- GND.
//
// Build + run:  tools\sim\run_sim.ps1
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "WiFi.h"
#include "app_state.h"
#include "config.h"
#include "heater.h"
#include "heater_controller.h"
#include "ntc_sensor.h"
#include "pid.h"
#include "telemetry.h"

// ---------------------------------------------------------------- shim state
namespace simclock {
uint32_t nowMs = 0;
void advance(uint32_t deltaMs) { nowMs += deltaMs; }
}  // namespace simclock

namespace simadc {
float nodeMillivolts = 1650.0f;
uint16_t rawCount = 2048;
uint8_t attenuation = 3;
void (*sampleHook)(void) = nullptr;
}  // namespace simadc

namespace simgpio {
uint32_t ledcDuty[64] = {0};
uint32_t ledcFreq[64] = {0};
uint8_t ledcResolution[64] = {0};
uint8_t pinMode[64] = {0};
uint8_t pinLevel[64] = {0};
}  // namespace simgpio

HardwareSerial Serial;
HardwareSerial Serial1;
EspClass ESP;
WiFiClass WiFi;

// ---------------------------------------------------------------- test harness
static int gChecks = 0;
static int gFailures = 0;
static const char *gSection = "";

static void section(const char *name) {
  gSection = name;
  printf("\n=== %s ===\n", name);
}

static void check(bool ok, const char *what, const char *detail = nullptr) {
  gChecks++;
  if (!ok) gFailures++;
  const char *text = detail ? detail : "";
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, text[0] ? " -- " : "", text);
}

static void checkNear(double actual, double expected, double tolerance, const char *what) {
  char detail[160];
  snprintf(detail, sizeof(detail), "got %.3f, expected %.3f +/- %.3f", actual, expected, tolerance);
  check(std::fabs(actual - expected) <= tolerance, what, detail);
}

// ---------------------------------------------------------------- plant + sensor model
struct Plant {
  double tempC = 25.0;
  double ambientC = 25.0;
  double heatRateCPerS = 5.5;  ///< °C/s at 100 % heater duty
  double lossPerS = 0.02;      ///< 1/s toward ambient

  double step(double dt, double dutyFraction) {
    tempC += (dutyFraction * heatRateCPerS - lossPerS * (tempC - ambientC)) * dt;
    return tempC;
  }
  double dutyFromShim(uint8_t pin) const {
    uint32_t maxDuty = (1u << simgpio::ledcResolution[pin]) - 1u;
    if (maxDuty == 0) return 0.0;
    uint32_t duty = simgpio::ledcDuty[pin];
    if (duty > maxDuty) duty = maxDuty;
    return (double)duty / (double)maxDuty;
  }
};

static double ntcResistance(double tempC) {
  const double t0K = 25.0 + 273.15;
  const double beta = 3950.0;
  const double r0 = 100000.0;
  const double tK = tempC + 273.15;
  return r0 * std::exp(beta * (1.0 / tK - 1.0 / t0K));
}

/// Divider node voltage for a given temperature with the FIXED wiring:
/// 3V3 -- 4.7k -- node -- NTC100K -- GND (the NTC is on the low side).
static double nodeMillivoltsFor(double tempC) {
  const double vRef = 3.3;
  const double rPull = 4700.0;
  const double rNtc = ntcResistance(tempC);
  return vRef * rNtc / (rPull + rNtc) * 1000.0;
}

/// Drive the shim ADC with an explicit node voltage and matching raw count.
static void feedNode(float millivolts, uint16_t rawCount) {
  simadc::nodeMillivolts = millivolts;
  simadc::rawCount = rawCount;
}

static void feedSensor(const Plant &plant) {
  const double mv = nodeMillivoltsFor(plant.tempC);
  simadc::nodeMillivolts = (float)mv;
  simadc::rawCount = (uint16_t)std::lround(mv / 3300.0 * 4095.0);
}

// ---------------------------------------------------------------- sensor tests
//
// Raw counts are the 12-bit code a healthy, non-railed ADC produces for the
// node voltage: round(mv / 3300 * 4095).
static void testSensorBetaCurve() {
  section("NTC100K B3950 beta curve");

  struct Point {
    float millivolts;
    uint16_t raw;
    double expectedC;
  };
  const Point points[] = {
      {1972.0f, 2447, 100.0},
      {982.0f, 1219, 150.0},
      {220.0f, 273, 250.0},
  };

  for (const Point &point : points) {
    // A fresh sensor per point: the EMA must not carry the previous (very
    // different) temperature into this one.
    NtcSensor sensor;
    SensorConfig cfg;
    sensor.begin(PIN_NTC);
    sensor.setConfig(cfg);

    feedNode(point.millivolts, point.raw);
    double measured = 0.0;
    for (int i = 0; i < 12; ++i) measured = sensor.readCelsius();  // settle the EMA

    char detail[160];
    snprintf(detail, sizeof(detail), "node %.0f mV -> %.2f C (expected %.0f C)",
             point.millivolts, measured, point.expectedC);
    checkNear(measured, point.expectedC, 1.0,
              "the beta model reproduces the NTC curve");
    check(!sensor.faulted(), "the in-range point is not faulted", detail);
  }
}

static void testSensorBelowRange() {
  section("NTC below-range upper bound");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  // 25 C / 100k puts the node at 3152 mV. The raw count has railed, so the
  // converter clamps to 3100 mV and reports the hottest temperature that node
  // could correspond to (~32.3 C). That is information, not a fault.
  feedNode(3152.0f, 4095);
  double measured = 0.0;
  for (int i = 0; i < 12; ++i) measured = sensor.readCelsius();

  char detail[160];
  snprintf(detail, sizeof(detail), "node 3152 mV / raw 4095 -> %.2f C", measured);
  checkNear(measured, 32.3, 1.5, "the clamped cold reading is the upper bound");
  check(!sensor.faulted(), "a below-range reading is not a fault", detail);
  check(strcmp(sensor.warning(), "below-range") == 0,
        "the below-range warning is published", sensor.warning());
}

static void testSensorRailedExtrapolation() {
  section("NTC railed ADC discards the extrapolated voltage");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  // The real failure: the node is 3.15 V, so the raw code pins at 4095 and the
  // curve-fitting calibration extrapolates from the pinned code to an
  // arbitrary millivolt value (985 mV here). Taking that value at face value
  // yields R = 2 kOhm -> 150 C on a cold sensor. A railed raw code means the
  // millivolt reading is meaningless, so it must be discarded.
  feedNode(985.0f, 4095);
  double measured = 0.0;
  for (int i = 0; i < 12; ++i) measured = sensor.readCelsius();

  char detail[160];
  snprintf(detail, sizeof(detail), "node 985 mV / raw 4095 -> %.2f C", measured);
  checkNear(measured, 32.3, 1.5,
            "a railed ADC reports the upper bound, not the extrapolated value");
  check(!sensor.faulted(), "a railed but plausible reading is not a fault",
        detail);
  check(strcmp(sensor.warning(), "below-range") == 0,
        "a railed ADC is reported as below-range", sensor.warning());
}

// --------------------------------------------------------- sample provenance
//
// The median filter selects a sample by temperature, but the published raw code
// must come from THAT sample: the "raw code has railed" test and the voltage it
// gates have to describe one conversion, not two. Here the three coldest samples
// are railed (985 mV of extrapolated calibration against raw 4095) and the LAST
// two are honest in-range samples (1972 mV, raw 2447), so "the last conversion"
// is a different sample from the median. The reported raw must be 4095 and the
// warning must be below-range; reading the last conversion's raw against the
// median's voltage yields raw 2447 and hides the warning.
//
// analogReadMilliVolts()/analogRead() are two independent conversions, which is
// exactly the bug shape of the 150 degC regression: the shim therefore lets the
// test return a different (mV, raw) pair per sample through simadc::sampleHook.
static const float kProvenanceMv[NTC_MEDIAN_WINDOW] = {985.0f, 985.0f, 985.0f,
                                                       1972.0f, 1972.0f};
static const uint16_t kProvenanceRaw[NTC_MEDIAN_WINDOW] = {4095, 4095, 4095, 2447, 2447};
static int gProvenanceIndex = 0;

static void provenanceSample() {
  const int i = gProvenanceIndex % NTC_MEDIAN_WINDOW;
  simadc::nodeMillivolts = kProvenanceMv[i];
  simadc::rawCount = kProvenanceRaw[i];
  ++gProvenanceIndex;
}

static void testSensorSampleProvenance() {
  section("NTC sample provenance (mV and raw of the same sample)");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  // readCelsius() always takes exactly NTC_MEDIAN_WINDOW samples and the hook
  // cycles with the same period, so every read sees the identical window: the
  // median is a railed sample and the last sample is not.
  gProvenanceIndex = 0;
  simadc::sampleHook = provenanceSample;
  double measured = 0.0;
  for (int i = 0; i < 12; ++i) measured = sensor.readCelsius();
  simadc::sampleHook = nullptr;

  char detail[160];
  snprintf(detail, sizeof(detail), "raw %u, mv %.0f, %.2f C", (unsigned)sensor.lastAdcRaw(),
           sensor.readMillivolts(), measured);
  check(sensor.lastAdcRaw() == 4095,
        "the published raw code belongs to the median sample, not the last one", detail);
  check(sensor.readMillivolts() > 984.0f && sensor.readMillivolts() < 986.0f,
        "the published millivolts belong to the median sample", detail);
  checkNear(measured, 32.3, 1.5,
            "the railed median yields the honest upper bound, not 150 C");
  check(strcmp(sensor.warning(), "below-range") == 0,
        "the below-range warning matches the railed median sample", sensor.warning());
  check(!sensor.faulted(), "a below-range median is not a fault", detail);

  // The single raw sample must publish its own raw code as well.
  feedNode(1972.0f, 2447);
  sensor.readRawCelsius();
  check(sensor.lastAdcRaw() == 2447,
        "readRawCelsius publishes the raw code of its own sample");
}

static void testSensorInRangeNoWarning() {
  section("NTC in-range reading has no warning");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  feedNode(1972.0f, 2447);  // ~100 C, inside the ADC's calibrated band
  for (int i = 0; i < 12; ++i) sensor.readCelsius();

  check(sensor.warning()[0] == '\0', "an in-range sample clears the warning",
        sensor.warning());
  check(!sensor.faulted(), "an in-range sample is not faulted");
}

static void testSensorShort() {
  section("NTC short circuit");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  feedNode(0.0f, 0);
  for (int i = 0; i < 12; ++i) sensor.readCelsius();

  check(sensor.faulted(), "a node at 0 V is a fault");
  check(strcmp(sensor.warning(), "short") == 0, "a short sets the short warning",
        sensor.warning());
}

static void testSensorOpen() {
  section("NTC open circuit");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  feedNode(3300.0f, 4095);
  for (int i = 0; i < 12; ++i) sensor.readCelsius();

  check(sensor.faulted(), "a node at the rail is a fault");
  check(strcmp(sensor.warning(), "open") == 0, "an open sets the open warning",
        sensor.warning());
}

static void testSensorOutOfRange() {
  section("NTC out-of-range temperature");
  NtcSensor sensor;
  SensorConfig cfg;
  sensor.begin(PIN_NTC);
  sensor.setConfig(cfg);

  // 60 mV on the node is a thermistor of ~87 ohm, which the beta model maps to
  // ~364 C - above SENSOR_MAX_VALID_C (350 C).
  feedNode(60.0f, 74);
  for (int i = 0; i < 12; ++i) sensor.readCelsius();

  check(sensor.faulted(), "a temperature outside the valid window is a fault");
  check(strcmp(sensor.warning(), "range") == 0, "out of range sets the range warning",
        sensor.warning());
}

// ---------------------------------------------------------------- control tests
static void testPidConvergence() {
  section("PID stability and convergence");
  PidController pid;
  pid.configure(12.0f, 0.4f, 30.0f);
  pid.setOutputLimits(0.0f, 100.0f);
  pid.setIntegralLimit(PID_INTEGRAL_LIMIT);
  pid.setDerivativeFilter(0.25f);

  Plant plant;
  const float setpoint = 180.0f;
  const float dt = CONTROL_PERIOD_MS / 1000.0f;
  const int settleSteps = (int)(60000 / CONTROL_PERIOD_MS);  // ignore the warm-up ramp
  float minTemp = 1e9f, maxTemp = -1e9f;
  float settled = 0.0f;
  bool withinBand = true;

  for (int step = 0; step < (int)(120000 / CONTROL_PERIOD_MS); step++) {
    float out = pid.compute(setpoint, (float)plant.tempC, dt);
    plant.step(dt, out / 100.0);
    if (step > settleSteps) {
      minTemp = std::min(minTemp, (float)plant.tempC);
      maxTemp = std::max(maxTemp, (float)plant.tempC);
    }
    if (step > settleSteps) {
      if (std::fabs(plant.tempC - setpoint) > 3.0f) withinBand = false;
      settled = (float)plant.tempC;
    }
  }
  check(withinBand, "settles within ±3 °C of the setpoint", nullptr);
  checkNear(settled, setpoint, 3.0, "final temperature");
  check(maxTemp - minTemp < 12.0, "no runaway oscillation after settling", nullptr);
  char detail[128];
  snprintf(detail, sizeof(detail), "post-settle band %.1f °C, min %.1f, max %.1f", maxTemp - minTemp, minTemp, maxTemp);
  printf("  info: %s\n", detail);
}

static void testAntiWindup() {
  section("Anti-windup");
  PidController pid;
  pid.configure(6.0f, 2.0f, 10.0f);
  pid.setOutputLimits(0.0f, 50.0f);  // deliberately clipped
  pid.setIntegralLimit(PID_INTEGRAL_LIMIT);

  const float dt = CONTROL_PERIOD_MS / 1000.0f;
  for (int step = 0; step < 10000; step++) pid.compute(400.0f, 25.0f, dt);  // long saturation
  float saturatedOutput = pid.output();
  check(saturatedOutput <= 50.5f, "output stays inside the configured limit", nullptr);

  // Reverse the error: with a wound-up integrator the output would stay pinned.
  int stepsToRespond = -1;
  for (int step = 0; step < 1000; step++) {
    float out = pid.compute(25.0f, 25.0f, dt);
    if (out < 25.0f) {
      stepsToRespond = step;
      break;
    }
  }
  char detail[128];
  snprintf(detail, sizeof(detail), "%d steps (%.0f ms)", stepsToRespond,
           (double)stepsToRespond * (double)CONTROL_PERIOD_MS);
  check(stepsToRespond >= 0 && stepsToRespond * CONTROL_PERIOD_MS < 3000,
        "output reacts quickly when the error reverses", detail);
}

static void testControllerSafety() {
  section("Controller safety (overtemp + fault)");
  HeaterController controller;
  AppSettings settings;
  settings.setpointC = 180.0f;
  settings.maxTemperatureC = 120.0f;
  settings.outputLimitPct = 100.0f;
  settings.gains = PidGains{12.0f, 0.4f, 30.0f};  // without gains the PID cannot drive
  controller.begin(settings);
  controller.setMode(ControlMode::Pid);

  Plant plant;
  const float dt = CONTROL_PERIOD_MS / 1000.0f;
  for (int step = 0; step < 400; step++) {
    feedSensor(plant);
    controller.loop(simclock::nowMs);
    plant.step(dt, plant.dutyFromShim(PIN_HEATER_PWM));
    simclock::advance(CONTROL_PERIOD_MS);
  }
  SystemState state = controller.snapshot();
  check(state.temperatureC > 25.0f, "the controller sees a rising temperature", nullptr);
  check(state.outputPct > 0.0f || state.state == HeaterState::Heating, "the heater is driven while below the setpoint", nullptr);

  // Force the plant above the safety limit.
  plant.tempC = 200.0;
  for (int step = 0; step < 100; step++) {
    feedSensor(plant);
    controller.loop(simclock::nowMs);
    plant.step(dt, plant.dutyFromShim(PIN_HEATER_PWM));
    simclock::advance(CONTROL_PERIOD_MS);
  }
  state = controller.snapshot();
  check(state.overtemp, "overtemperature is latched", nullptr);
  check(state.outputPct == 0.0f, "the heater output is forced to zero on overtemp", nullptr);
  check(simgpio::ledcDuty[PIN_HEATER_PWM] == 0, "the PWM duty is actually zero on overtemp", nullptr);

  // Cool back down: the latch must clear on its own.
  plant.tempC = 100.0;
  for (int step = 0; step < 100; step++) {
    feedSensor(plant);
    controller.loop(simclock::nowMs);
    plant.step(dt, plant.dutyFromShim(PIN_HEATER_PWM));
    simclock::advance(CONTROL_PERIOD_MS);
  }
  state = controller.snapshot();
  check(!state.overtemp, "the overtemp latch clears after cooling", nullptr);

  // Sensor fault must also kill the heater.
  simadc::nodeMillivolts = 3295.0f;  // open circuit
  for (int step = 0; step < 50; step++) {
    controller.loop(simclock::nowMs);
    simclock::advance(CONTROL_PERIOD_MS);
  }
  state = controller.snapshot();
  check(state.sensorFault, "a sensor fault is reported", nullptr);
  check(simgpio::ledcDuty[PIN_HEATER_PWM] == 0, "a sensor fault forces the PWM duty to zero", nullptr);
  simadc::nodeMillivolts = 1650.0f;
}

static void testTelemetry() {
  section("Telemetry ring buffer");
  Telemetry telemetry;
  telemetry.begin(10);
  for (int i = 0; i < 25; i++) telemetry.push(i * 100, 20.0f + i, 100.0f, 50.0f, 128);
  check(telemetry.size() == 10, "the ring buffer is bounded by its capacity", nullptr);
  TelemetrySample samples[16];
  size_t count = telemetry.copyRecent(samples, 16);
  check(count == 10, "copyRecent returns the buffered samples", nullptr);
  check(samples[0].tMs == 1500 && samples[count - 1].tMs == 2400,
        "samples are returned oldest -> newest", nullptr);
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string only = (argc > 1) ? argv[1] : "";
  auto want = [&](const char *name) { return only.empty() || only == name; };

  printf("FruitSnack control-core simulation\n");
  printf("virtual clock starts at %u ms\n", simclock::nowMs);

  if (want("sensor")) {
    testSensorBetaCurve();
    testSensorBelowRange();
    testSensorRailedExtrapolation();
    testSensorSampleProvenance();
    testSensorInRangeNoWarning();
    testSensorShort();
    testSensorOpen();
    testSensorOutOfRange();
  }
  if (want("pid")) testPidConvergence();
  if (want("windup")) testAntiWindup();
  if (want("safety")) testControllerSafety();
  if (want("telemetry")) testTelemetry();

  printf("\n=========================================\n");
  printf("%d checks, %d failures\n", gChecks, gFailures);
  printf("=========================================\n");
  return gFailures == 0 ? 0 : 1;
}
