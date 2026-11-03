// FruitSnack console + settings test (host build).
//
// Exercises the command interpreter that backs BOTH the hardware UART console
// and the websocket console, plus the LittleFS-backed settings store.
//
// Build + run: tools\sim\run_tests.sh (the "console" test)
#include <cstdio>
#include <cstring>

#include "Arduino.h"
#include "ESPmDNS.h"
#include "LittleFS.h"
#include "WiFi.h"

#include "app_state.h"
#include "config.h"
#include "console.h"
#include "heater_controller.h"
#include "logger.h"
#include "network_manager.h"
#include "settings.h"

// ---------------------------------------------------------------- shim state
namespace simclock {
uint32_t nowMs = 0;
void advance(uint32_t deltaMs) { nowMs += deltaMs; }
}
namespace simadc {
float nodeMillivolts = 3000.0f;  // ~25 degC with the NTC on the low side
uint16_t rawCount = 3723;
uint8_t attenuation = 3;
void (*sampleHook)(void) = nullptr;
}
namespace simgpio {
uint32_t ledcDuty[64] = {0};
uint32_t ledcFreq[64] = {0};
uint8_t ledcResolution[64] = {0};
uint8_t pinMode[64] = {0};
uint8_t pinLevel[64] = {0};
}
HardwareSerial Serial;
HardwareSerial Serial1;
EspClass ESP;
WiFiClass WiFi;
fs::LittleFSFS LittleFS;
ESPMDNSClass MDNS;

// ---------------------------------------------------------------- harness
static int gChecks = 0;
static int gFailures = 0;

static void check(bool ok, const char *what, const char *detail = nullptr) {
  gChecks++;
  if (!ok) gFailures++;
  const char *text = detail ? detail : "";
  printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, text[0] ? " -- " : "", text);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("FruitSnack console test\n");

  gLogger.begin(&Serial, &Serial1);
  gLogger.setLevel(LogLevel::Warn);  // keep the transcript readable

  // The firmware modules own their singletons: the test drives those instead of
  // constructing copies of its own.
  SettingsStore &settings = gSettings;
  check(settings.begin(), "the settings store mounts the filesystem");
  check(settings.totalBytes() > 0, "the filesystem reports its size");

  AppSettings app;
  snprintf(app.hostname, sizeof(app.hostname), "fruitsnack");
  snprintf(app.wifiSsid, sizeof(app.wifiSsid), "lab-network");
  app.gains = PidGains{12.0f, 0.4f, 30.0f};
  app.setpointC = 180.0f;
  app.maxTemperatureC = 250.0f;
  app.outputLimitPct = 100.0f;

  HeaterController &controller = gController;
  controller.begin(app);
  controller.setMode(ControlMode::Pid);

  NetworkManager &network = gNetwork;
  network.begin(app);

  Console &console = gConsole;

  // ------------------------------------------------------------- help/status
  Console::Result r = console.execute("help");
  check(r.ok && r.text.length() > 100, "help prints the command list");
  r = console.execute("status");
  check(r.ok && r.text.length() > 10, "status prints the system state");
  check(r.text.indexOf("sensor warning:") >= 0 && r.text.indexOf("adc:") >= 0,
        "status reports the sensor warning and the ADC values");

  // ------------------------------------------------------------- setpoint
  r = console.execute("set 205");
  check(r.ok, "set 205 is accepted");
  check(controller.getSetpoint() > 204.0f && controller.getSetpoint() < 206.0f,
        "the setpoint is applied to the controller");
  r = console.execute("set 9999");
  check(r.ok && controller.getSetpoint() <= 400.0f, "an absurd setpoint is clamped");
  r = console.execute("set abc");
  check(!r.ok, "a non-numeric setpoint is rejected");
  r = console.execute("set");
  check(!r.ok, "set without an argument reports the usage");

  // ------------------------------------------------------------- modes
  r = console.execute("mode manual");
  check(r.ok && controller.getMode() == ControlMode::Manual, "mode manual switches the controller");
  r = console.execute("manual 55");
  check(r.ok && controller.getManualDuty() > 54.0f && controller.getManualDuty() < 56.0f,
        "manual 55 sets the duty");
  r = console.execute("manual 500");
  check(r.ok && controller.getManualDuty() <= 100.0f, "the manual duty is clamped to 100 %");
  r = console.execute("manual abc");
  check(!r.ok, "a non-numeric duty is rejected");
  r = console.execute("mode bogus");
  check(!r.ok, "an unknown mode is rejected");
  r = console.execute("on");
  check(r.ok && controller.getMode() == ControlMode::Pid, "on selects PID mode");
  r = console.execute("off");
  check(r.ok && controller.getMode() == ControlMode::Off, "off selects off mode");

  // ------------------------------------------------------------- pid
  r = console.execute("pid set 10 0.5 20");
  PidGains gains = controller.getGains();
  check(r.ok && gains.kp == 10.0f && gains.ki == 0.5f && gains.kd == 20.0f,
        "pid set applies the three gains");
  r = console.execute("pid set 1");
  check(!r.ok, "pid set with too few arguments reports the usage");
  r = console.execute("pid set -1 0 0");
  check(!r.ok, "negative gains are rejected");
  r = console.execute("pid");
  check(r.ok && r.text.length() > 5, "pid prints the gains and terms");
  r = console.execute("pid save");
  check(r.ok, "pid save persists the gains");

  // ------------------------------------------------------------- configuration
  r = console.execute("config");
  check(r.ok && r.text.length() > 10, "config prints the stored configuration");
  r = console.execute("config set hostname testpi");
  check(r.ok, "config set hostname is accepted");
  AppSettings reloaded;
  check(settings.load(reloaded), "the configuration was written to the filesystem");
  check(strcmp(reloaded.hostname, "testpi") == 0, "the new hostname was persisted");
  r = console.execute("config set bogus 1");
  check(!r.ok, "an unknown configuration key is rejected");
  r = console.execute("config set kp 15");
  check(r.ok, "config set kp is accepted");
  check(controller.getGains().kp == 15.0f, "the new gain reached the controller");

  // ------------------------------------------------------------- thermistor
  // Run a few control iterations so the sensor diagnostics are live.
  for (int i = 0; i < 5; i++) {
    controller.loop(simclock::nowMs);
    simclock::advance(CONTROL_PERIOD_MS);
  }
  r = console.execute("sensor");
  check(r.ok && r.text.indexOf("thermistor:") >= 0 &&
            r.text.indexOf("warning:") >= 0,
        "sensor prints the electrical diagnostics");
  r = console.execute("config set ntcpullup 4700");
  check(r.ok, "config set ntcpullup is accepted");
  settings.load(reloaded);
  check(reloaded.ntcPullupOhms == 4700.0f, "the pull-up value was persisted");
  r = console.execute("config set ntcpullup 0");
  check(!r.ok, "a non-positive pull-up is rejected");
  r = console.execute("config set ntcpullup -4700");
  check(!r.ok, "a negative pull-up is rejected");

  // ------------------------------------------------------------- misc
  r = console.execute("save");
  check(r.ok, "save persists the settings");
  r = console.execute("clear");
  check(r.ok, "clear empties the graph history");
  r = console.execute("graph 10");
  check(r.ok, "graph prints the sample buffer");
  r = console.execute("info");
  check(r.ok && r.text.length() > 10, "info prints the device information");
  r = console.execute("wifi");
  check(r.ok, "wifi prints the link status");
  r = console.execute("mdns");
  check(r.ok, "mdns prints the hostname");
  r = console.execute("totally-unknown");
  check(!r.ok, "an unknown command is rejected");

  // ------------------------------------------------------------- case folding
  r = console.execute("SET 150");
  check(r.ok, "commands are case insensitive");
  r = console.execute("   status   ");
  check(r.ok, "surrounding whitespace is tolerated");

  // -------------------------------------------------------- refused mode changes
  // An open circuit trips the sensor-fault safety, and setMode() then refuses to
  // start heating. That refusal must be reported as a failure, not as "mode = pid".
  simadc::nodeMillivolts = 3295.0f;
  simadc::rawCount = 4095;
  for (int i = 0; i < 5; i++) {
    controller.loop(simclock::nowMs);
    simclock::advance(CONTROL_PERIOD_MS);
  }
  check(controller.snapshot().sensorFault, "an open sensor is reported as a fault");
  r = console.execute("on");
  check(!r.ok, "on is refused while the sensor is faulted", r.text.c_str());
  check(controller.getMode() != ControlMode::Pid,
        "a refused mode change leaves the mode untouched");
  r = console.execute("mode manual");
  check(!r.ok, "mode manual is refused while the sensor is faulted", r.text.c_str());
  simadc::nodeMillivolts = 3000.0f;
  simadc::rawCount = 3723;
  for (int i = 0; i < 5; i++) {
    controller.loop(simclock::nowMs);
    simclock::advance(CONTROL_PERIOD_MS);
  }

  // -------------------------------------------------- corrupt config protection
  // A file that is present but unreadable must never be replaced by defaults:
  // the stored WiFi credentials and hostname would be lost.
  {
    File file = LittleFS.open(CONFIG_FILE_PATH, "w");
    check(static_cast<bool>(file), "a corrupt config can be planted");
    file.print("{ this is not json");
    file.close();
  }
  r = console.execute("save");
  check(!r.ok, "save refuses to run with an unreadable config", r.text.c_str());
  r = console.execute("config set kp 20");
  check(!r.ok, "config set refuses to run with an unreadable config", r.text.c_str());
  {
    File file = LittleFS.open(CONFIG_FILE_PATH, "r");
    const String raw = file.readString();
    file.close();
    check(raw.indexOf("this is not json") >= 0,
          "the unreadable config file was left untouched", raw.c_str());
  }
  LittleFS.remove(CONFIG_FILE_PATH);
  r = console.execute("save");
  check(r.ok, "save works again once no config file exists");

  printf("\n=========================================\n");
  printf("%d checks, %d failures\n", gChecks, gFailures);
  printf("=========================================\n");
  return gFailures == 0 ? 0 : 1;
}
