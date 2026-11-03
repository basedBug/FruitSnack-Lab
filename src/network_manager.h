// FruitSnack — WiFi station / mDNS manager.
//
// The manager owns the connection state machine: it joins the configured
// network, falls back to a rescue access point when no SSID is configured,
// reconnects with a bounded exponential backoff and publishes the web UI via
// mDNS once the link is up.
//
// NOTE: this header intentionally does not include <WiFi.h>; the ESP32 Arduino
// core already declares a global class named `NetworkManager` in Network.h.
// The implementation renames that core symbol for its own translation unit.
// Do not include <WiFi.h> in a file that also includes this header.
#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

#include "app_state.h"

struct WifiScanEntry {
  String ssid;
  int32_t rssi = 0;
  uint8_t channel = 0;
  bool secure = false;
};

class NetworkManager {
 public:
  using ScanCallback = std::function<void(const std::vector<WifiScanEntry> &)>;

  void begin(const AppSettings &settings);

  /// Call frequently (at least every 50 ms); drives connect/reconnect, mDNS
  /// restarts and the asynchronous scan poll.
  void loop();

  bool isConnected() const;
  String ipAddress() const;
  String macAddress() const;
  int32_t rssi() const;
  String hostname() const;
  String ssid() const;

  /// Validate + apply a new mDNS host name.  Invalid input is rejected and
  /// logged; the previous name stays in effect.
  void setHostname(const String &hostname);

  /// Apply changed credentials / host name (safe to call from any task).
  void applySettings(const AppSettings &settings);

  void reconnect();

  /// Start an asynchronous scan.  The callback runs exactly once from loop().
  void startScan(ScanCallback cb);
  bool isScanning() const;

 private:
  void startSta();
  void startAp();
  void startMdns();
  void stopMdns();
  void scheduleRetry(uint32_t now);
  void handleScan();
  static bool validHostname(const String &candidate);

  String ssid_;
  String password_;
  String hostname_ = "fruitsnack";

  bool started_ = false;
  bool ap_mode_ = false;
  bool connecting_ = false;
  bool link_up_ = false;
  bool mdns_started_ = false;
  bool reconfigure_ = false;
  bool hostname_dirty_ = false;

  uint32_t attempt_ms_ = 0;
  uint32_t retry_at_ms_ = 0;
  uint32_t backoff_ms_ = 5000;

  bool scanning_ = false;
  bool scan_empty_pending_ = false;
  uint32_t scan_started_ms_ = 0;
  ScanCallback scan_cb_;
};

/// The one WiFi/mDNS manager.
extern NetworkManager gNetwork;
