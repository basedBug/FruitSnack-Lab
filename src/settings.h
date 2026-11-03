// FruitSnack — persisted settings on LittleFS.
//
// The configuration is stored as /config.json.  Saving writes /config.tmp
// first and then renames it over the real file so a power cut cannot leave a
// half written configuration behind.
#pragma once

#include <Arduino.h>

#include "app_state.h"

class SettingsStore {
 public:
  SettingsStore();

  /// Mount the "littlefs" partition WITHOUT formatting it.  Returns false when
  /// the filesystem is unavailable (the caller can still run on defaults, and
  /// `mounted()` stays false); stored data is never erased by a failed mount.
  bool begin();
  bool mounted() const;
  bool exists() const;

  /// Read /config.json.  A missing OR unreadable file yields the defaults and
  /// returns false; check exists() to tell the two apart.  A corrupt file must
  /// never be overwritten with defaults - the caller can still read it back.
  bool load(AppSettings &out) const;

  /// Atomically write /config.json.
  bool save(const AppSettings &in);

  /// Delete the configuration file.
  bool clear();

  size_t totalBytes() const;
  size_t usedBytes() const;

 private:
  bool mounted_ = false;
  SemaphoreHandle_t mutex_ = nullptr;
};

/// The one settings store: the filesystem is mounted once during setup.
extern SettingsStore gSettings;
