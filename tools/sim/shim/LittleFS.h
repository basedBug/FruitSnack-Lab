// Host-side LittleFS shim for the FruitSnack simulation.
// Not part of the firmware. See tools/sim/shim/Arduino.h.
#pragma once

#include "FS.h"

namespace fs {

class LittleFSFS : public FS {
 public:
  bool begin(bool formatOnFail = false, const char *basePath = "/littlefs",
             uint8_t maxOpenFiles = 10, const char *partitionLabel = "spiffs") {
    (void)formatOnFail;
    (void)basePath;
    (void)maxOpenFiles;
    (void)partitionLabel;
    mounted_ = true;
    return true;
  }
  bool format() {
    files_.clear();
    return true;
  }
  size_t totalBytes() { return 2031616; }
  size_t usedBytes() {
    size_t used = 0;
    for (const auto &entry : files_) used += entry.second.size();
    return used;
  }
  void end() { mounted_ = false; }

 private:
  bool mounted_ = false;
};

}  // namespace fs

extern fs::LittleFSFS LittleFS;
