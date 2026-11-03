// FruitSnack — LittleFS backed settings store.
#include "settings.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"
#include "logger.h"
#include "state_json.h"

namespace {
constexpr const char *kTmpPath = "/config.tmp";
}  // namespace

SettingsStore gSettings;

SettingsStore::SettingsStore() {
  mutex_ = xSemaphoreCreateMutex();
}

bool SettingsStore::begin() {
  // Mount WITHOUT format-on-fail.  Formatting here would erase a valid
  // config.json (WiFi credentials, hostname) on a transient mount failure, so a
  // failure is reported instead and the data is left untouched.
  mounted_ = LittleFS.begin(false, LITTLEFS_BASE_PATH, 10, LITTLEFS_PARTITION_LABEL);
  if (!mounted_) {
    // Some partition tables use the historical "spiffs" label even though the
    // filesystem is littlefs.
    mounted_ = LittleFS.begin(false, LITTLEFS_BASE_PATH, 10, "spiffs");
  }
  if (!mounted_) {
    LOG_ERROR("littlefs mount failed (the partition is NOT formatted; stored data is kept)");
    return false;
  }
  LOG_INFO("littlefs mounted: %u / %u bytes used", static_cast<unsigned>(LittleFS.usedBytes()),
           static_cast<unsigned>(LittleFS.totalBytes()));
  return true;
}

bool SettingsStore::mounted() const {
  return mounted_;
}

bool SettingsStore::exists() const {
  return mounted_ && LittleFS.exists(CONFIG_FILE_PATH);
}

bool SettingsStore::load(AppSettings &out) const {
  out = AppSettings();
  if (!mounted_) {
    return false;
  }

  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  File file = LittleFS.open(CONFIG_FILE_PATH, "r");
  if (!file) {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }

  if (error) {
    LOG_WARN("config.json is corrupt (%s), using defaults", error.c_str());
    return false;
  }
  if (!doc.is<JsonObject>()) {
    LOG_WARN("config.json does not contain an object, using defaults");
    return false;
  }

  applyJsonToSettings(doc, out);
  return true;
}

bool SettingsStore::save(const AppSettings &in) {
  if (!mounted_) {
    return false;
  }

  JsonDocument doc;
  settingsToJson(doc, in);
  String body;
  body.reserve(1024);
  const size_t serialized = serializeJson(doc, body);
  if (serialized == 0 || serialized != body.length()) {
    LOG_ERROR("failed to serialise the configuration (%u of %u bytes)",
              static_cast<unsigned>(serialized), static_cast<unsigned>(body.length()));
    return false;
  }

  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }

  bool ok = false;
  File file = LittleFS.open(kTmpPath, "w");
  if (file) {
    const size_t written = file.print(body);
    file.close();
    ok = written == body.length();
  }
  if (!ok) {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
    LittleFS.remove(kTmpPath);
    LOG_ERROR("failed to write %s", kTmpPath);
    return false;
  }

  // LittleFS rename() replaces an existing destination atomically, so the old
  // config.json stays intact until the new one is in place.  Never remove the
  // destination first: a failure in between would leave the device with no
  // configuration at all.
  if (!LittleFS.rename(kTmpPath, CONFIG_FILE_PATH)) {
    LittleFS.remove(kTmpPath);
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
    LOG_ERROR("failed to move config.tmp into place, the previous configuration is kept");
    return false;
  }

  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  LOG_INFO("configuration saved (%u bytes)", static_cast<unsigned>(body.length()));
  return true;
}

bool SettingsStore::clear() {
  if (!mounted_) {
    return false;
  }
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  const bool ok = LittleFS.remove(CONFIG_FILE_PATH);
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  if (ok) {
    LOG_INFO("configuration cleared");
  }
  return ok;
}

size_t SettingsStore::totalBytes() const {
  return mounted_ ? LittleFS.totalBytes() : 0;
}

size_t SettingsStore::usedBytes() const {
  return mounted_ ? LittleFS.usedBytes() : 0;
}
