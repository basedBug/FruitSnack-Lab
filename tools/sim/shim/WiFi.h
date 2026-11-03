// Host-side WiFi shim for the FruitSnack simulation.
// Not part of the firmware. See tools/sim/shim/Arduino.h.
#pragma once

#include <stdint.h>
#include <string>

#include "Arduino.h"

enum wl_status_t {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
};

typedef enum {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP = 1,
  WIFI_AUTH_WPA_PSK = 2,
  WIFI_AUTH_WPA2_PSK = 3,
  WIFI_AUTH_WPA_WPA2_PSK = 4,
} wifi_auth_mode_t;

class IPAddress {
 public:
  uint8_t octet[4] = {192, 168, 1, 42};
  String toString() const {
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", octet[0], octet[1], octet[2], octet[3]);
    return String(buf);
  }
};

class WiFiClass {
 public:
  void mode(int m) { mode_ = m; }
  int mode() const { return mode_; }
  void persistent(bool) {}
  void setAutoReconnect(bool) {}
  void setHostname(const char *host) { hostname_ = host ? host : ""; }
  String hostname() const { return hostname_; }

  int begin(const char *ssid, const char *pass) {
    (void)pass;
    ssid_ = ssid ? ssid : "";
    return 0;
  }
  wl_status_t status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress(); }
  String macAddress() { return "AA:BB:CC:DD:EE:FF"; }
  String softAPmacAddress() { return "AA:BB:CC:DD:EE:F0"; }
  int32_t RSSI() { return -55; }
  int32_t RSSI(int16_t i) { (void)i; return -55; }
  String SSID() { return ssid_; }
  String SSID(int16_t i) { (void)i; return "sim-network"; }
  uint8_t channel(int16_t i) { (void)i; return 6; }
  uint8_t encryptionType(int16_t i) { (void)i; return WIFI_AUTH_WPA2_PSK; }

  int scanNetworks() { return 1; }
  int scanNetworks(bool async) { (void)async; return 1; }
  bool scanComplete() { return true; }
  void scanDelete() {}
  bool disconnect(bool wifioff = false, bool eraseap = false) {
    (void)wifioff; (void)eraseap;
    return true;
  }

  bool softAP(const char *ssid, const char *pass) {    (void)pass;
    ap_ssid_ = ssid ? ssid : "";
    return true;
  }
  void softAPdisconnect(bool) { ap_ssid_ = ""; }
  void softAPsetHostname(const char *host) { hostname_ = host ? host : ""; }
  IPAddress softAPIP() {
    IPAddress ip;
    ip.octet[0] = 192; ip.octet[1] = 168; ip.octet[2] = 4; ip.octet[3] = 1;
    return ip;
  }

 private:
  int mode_ = 1;
  String ssid_;
  String ap_ssid_;
  String hostname_ = "fruitsnack";
};

extern WiFiClass WiFi;

#define WIFI_STA 1
#define WIFI_AP 2

// Scan result codes (Arduino WiFi library).
#define WIFI_SCAN_RUNNING -1
#define WIFI_SCAN_FAILED -2
#define WIFI_SCAN_COMPLETED -3
