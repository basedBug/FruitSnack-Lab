// FruitSnack — plain text command console.
//
// The same command language is used by the USB-CDC console, the hardware UART
// and the websocket `cmd` message, so the parser lives in one place and never
// blocks the caller.
//
// The module owns its singleton (gConsole) and talks to the other modules
// through their singletons: there is nothing to inject.
#pragma once

#include <Arduino.h>

#include "app_state.h"

class Console {
 public:
  struct Result {
    bool ok = true;
    String text;
  };

  Console();
  ~Console();

  Console(const Console &) = delete;
  Console &operator=(const Console &) = delete;

  /// Run one command line.  Never throws, never blocks for long.
  Result execute(const String &line);

  String helpText() const;

 private:
  /// Persist the control-side settings.  Returns false and, when `error` is
  /// non-null, sets it to the reason.
  bool persistControlSettings(String *error);

  /// Load the stored configuration for a read-modify-write cycle.  False only
  /// when a config file exists but cannot be read.
  bool loadForWrite(AppSettings &out) const;

  /// Apply a mode change and verify it took effect; a refused change (sensor
  /// fault / overtemp latch) is reported as a failure.
  Result setModeChecked(ControlMode mode, const String &ok_text);

  bool scheduleRestart(uint32_t delay_ms);  ///< false when the task cannot be created
  static constexpr size_t kGraphCapacity = 600;

  bool reboot_pending_ = false;
  TelemetrySample *graph_buf_ = nullptr;
};

/// The console used by the UART/USB prompt.  The web server keeps its own
/// instance: two interleaved `graph` dumps must not share one buffer.
extern Console gConsole;
