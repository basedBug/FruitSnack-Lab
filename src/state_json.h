// FruitSnack — JSON serialisation of the shared state contracts.
//
// The wire format produced here is consumed by the browser UI, so the key
// names and value shapes must stay stable.
#pragma once

#include <ArduinoJson.h>

#include "app_state.h"

/// Fill `doc` with the state object (root becomes a JSON object).
void stateToJson(JsonDocument &doc, const SystemState &state);

/// Append one graph sample `{"t":..,"temp":..,"sp":..,"out":..}` to the
/// document's root array (creating the array when needed).
void sampleToJson(JsonDocument &doc, const TelemetrySample &sample);

/// Fill `doc` with the persisted settings object.
void settingsToJson(JsonDocument &doc, const AppSettings &settings);

/// Merge the keys present in `doc` into `settings`; missing keys are untouched.
void applyJsonToSettings(const JsonDocument &doc, AppSettings &settings);
