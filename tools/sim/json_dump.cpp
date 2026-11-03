// Dumps the firmware's JSON payloads so the web UI contract can be checked
// automatically against what the browser code actually reads.
//
// Links the real src/state_json.cpp against the host Arduino shim.
// Usage:  tools\sim\build_json_dump.ps1  ->  tools\sim\json_dump.exe
#include <cstdio>

#include <ArduinoJson.h>

#include "Arduino.h"
#include "WiFi.h"
#include "app_state.h"
#include "state_json.h"

namespace simclock {
uint32_t nowMs = 123456;
void advance(uint32_t deltaMs) { nowMs += deltaMs; }
}
namespace simadc {
float nodeMillivolts = 1650.0f;
uint16_t rawCount = 2048;
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

int main() {
  SystemState state;
  state.mode = ControlMode::Pid;
  state.state = HeaterState::Heating;
  state.setpointC = 205.0f;
  state.temperatureC = 198.3f;
  state.temperatureRawC = 198.9f;
  state.outputPct = 62.5f;
  state.duty = 159.0f;
  state.gains = PidGains{12.0f, 0.4f, 30.0f};
  state.pTerm = 80.0f;
  state.iTerm = 12.0f;
  state.dTerm = 3.0f;

  JsonDocument stateDoc;
  stateToJson(stateDoc, state);

  JsonDocument settingsDoc;
  AppSettings settings;
  snprintf(settings.wifiSsid, sizeof(settings.wifiSsid), "lab-network");
  snprintf(settings.hostname, sizeof(settings.hostname), "fruitsnack");
  settingsToJson(settingsDoc, settings);

  JsonDocument root;
  root["state"] = stateDoc.as<JsonObject>();
  root["config"] = settingsDoc.as<JsonObject>();

  String out;
  serializeJsonPretty(root, out);
  printf("%s\n", out.c_str());
  return 0;
}
