// FruitSnack — JSON serialisation of SystemState / settings.
#include "state_json.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

namespace {

bool getNumber(const JsonDocument &doc, const char *key, float &out) {
  const JsonVariantConst v = doc[key];
  if (v.is<const char *>()) {
    // A browser may send a number as a string.
    const char *text = v.as<const char *>();
    if (text == nullptr) {
      return false;
    }
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (end == text) {
      return false;
    }
    out = value;
    return true;
  }
  if (!v.is<double>()) {
    return false;
  }
  out = static_cast<float>(v.as<double>());
  return true;
}

bool getBool(const JsonDocument &doc, const char *key, bool &out) {
  const JsonVariantConst v = doc[key];
  if (!v.is<bool>()) {
    return false;
  }
  out = v.as<bool>();
  return true;
}

bool getString(const JsonDocument &doc, const char *key, char *dst, size_t capacity) {
  const JsonVariantConst v = doc[key];
  if (!v.is<const char *>()) {
    return false;
  }
  const char *value = v.as<const char *>();
  if (value == nullptr) {
    return false;
  }
  strlcpy(dst, value, capacity);
  return true;
}

float clampf(float value, float low, float high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

}  // namespace

void stateToJson(JsonDocument &doc, const SystemState &state) {
  doc["mode"] = controlModeName(state.mode);
  doc["state"] = heaterStateName(state.state);

  doc["setpoint"] = state.setpointC;
  doc["temperature"] = state.temperatureC;
  doc["temperatureRaw"] = state.temperatureRawC;
  doc["output"] = state.outputPct;
  doc["duty"] = static_cast<long>(lroundf(state.duty));
  // The header badge follows the real LEDC compare value, not the requested
  // duty: with a failed ledcAttach the heater is dead however hard the PID asks.
  doc["heaterOn"] = state.duty > 0.0f;
  doc["pwmReady"] = state.pwmReady;

  doc["overtemp"] = state.overtemp;
  doc["sensorFault"] = state.sensorFault;
  doc["maxTemperature"] = state.maxTemperatureC;

  JsonObject pid = doc["pid"].to<JsonObject>();
  pid["kp"] = state.gains.kp;
  pid["ki"] = state.gains.ki;
  pid["kd"] = state.gains.kd;
  pid["p"] = state.pTerm;
  pid["i"] = state.iTerm;
  pid["d"] = state.dTerm;
  pid["saturated"] = state.pidSaturated;

  // Heap fragmentation: share of the heap that cannot be served as one block.
  uint8_t fragmentation = state.heapFragmentation;
  const uint32_t heapSize = ESP.getHeapSize();
  if (heapSize > 0) {
    const uint32_t maxBlock = ESP.getMaxAllocHeap();
    const uint32_t usedPct = (100U * maxBlock) / heapSize;
    fragmentation = static_cast<uint8_t>(usedPct >= 100U ? 0U : 100U - usedPct);
  }

  JsonObject diag = doc["diag"].to<JsonObject>();
  diag["uptime"] = state.uptimeMs;
  diag["heap"] = state.freeHeap;
  diag["minHeap"] = state.minFreeHeap;
  diag["fragmentation"] = fragmentation;
  diag["rssi"] = static_cast<int32_t>(state.wifiRssi);
  diag["ip"] = state.ipAddress;
  diag["mac"] = state.macAddress;
  diag["adcMv"] = state.adcMillivolts;
  diag["adcRaw"] = state.adcRaw;
  diag["loopHz"] = state.controlLoopHz;
  diag["ntcResistance"] = state.ntcResistance;
  diag["ntcWarn"] = state.ntcWarn;
}

void sampleToJson(JsonDocument &doc, const TelemetrySample &sample) {
  JsonArray array = doc.as<JsonArray>();
  if (array.isNull()) {
    array = doc.to<JsonArray>();
  }
  JsonObject entry = array.add<JsonObject>();
  if (entry.isNull()) {
    return;
  }
  entry["t"] = sample.tMs;
  entry["temp"] = sample.temperatureC;
  entry["sp"] = sample.setpointC;
  entry["out"] = sample.outputPct;
}

void settingsToJson(JsonDocument &doc, const AppSettings &settings) {
  doc["wifiSsid"] = settings.wifiSsid;
  doc["wifiPassword"] = settings.wifiPassword;
  doc["hostname"] = settings.hostname;
  doc["kp"] = settings.gains.kp;
  doc["ki"] = settings.gains.ki;
  doc["kd"] = settings.gains.kd;
  doc["setpoint"] = settings.setpointC;
  doc["maxTemperature"] = settings.maxTemperatureC;
  doc["outputLimit"] = settings.outputLimitPct;
  doc["sensorOffset"] = settings.sensorOffsetC;
  doc["ntcPullupOhms"] = settings.ntcPullupOhms;

  doc["controlPeriodMs"] = settings.controlPeriodMs;
  doc["pidEnabled"] = settings.pidEnabled;
}

void applyJsonToSettings(const JsonDocument &doc, AppSettings &settings) {
  getString(doc, "wifiSsid", settings.wifiSsid, sizeof(settings.wifiSsid));
  // The web UI receives a masked password; never persist the mask over the
  // real secret when the settings round-trip.
  if (doc["wifiPassword"].is<const char *>()) {
    const char *password = doc["wifiPassword"].as<const char *>();
    if (password != nullptr && strcmp(password, "********") != 0) {
      strlcpy(settings.wifiPassword, password, sizeof(settings.wifiPassword));
    }
  }
  getString(doc, "hostname", settings.hostname, sizeof(settings.hostname));

  float value = 0.0f;
  if (getNumber(doc, "kp", value) && value >= 0.0f) settings.gains.kp = value;
  if (getNumber(doc, "ki", value) && value >= 0.0f) settings.gains.ki = value;
  if (getNumber(doc, "kd", value) && value >= 0.0f) settings.gains.kd = value;
  if (getNumber(doc, "setpoint", value)) settings.setpointC = clampf(value, 0.0f, TEMPERATURE_CEILING_C);
  if (getNumber(doc, "maxTemperature", value)) settings.maxTemperatureC = clampf(value, 0.0f, TEMPERATURE_CEILING_C);
  if (getNumber(doc, "outputLimit", value)) settings.outputLimitPct = clampf(value, 0.0f, 100.0f);
  if (getNumber(doc, "sensorOffset", value)) settings.sensorOffsetC = clampf(value, -50.0f, 50.0f);
  if (getNumber(doc, "ntcPullupOhms", value) && value > 0.0f) {
    settings.ntcPullupOhms = value;
  }
  if (getNumber(doc, "controlPeriodMs", value)) {
    settings.controlPeriodMs = static_cast<uint32_t>(clampf(value, 10.0f, 1000.0f));
  }

  bool flag = false;
  if (getBool(doc, "pidEnabled", flag)) settings.pidEnabled = flag;
}
