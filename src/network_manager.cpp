// FruitSnack — WiFi + mDNS manager implementation.
//
// The ESP32 Arduino core declares its own global `NetworkManager` class in
// Network.h (included by WiFi.h).  We rename that symbol inside this
// translation unit only, so our class can keep the name from the module
// contract.  This is why <WiFi.h> must not be included by files that also
// include network_manager.h.
#include <Arduino.h>

#define NetworkManager Esp32CoreNetworkManager
#include <ESPmDNS.h>
#include <WiFi.h>
#undef NetworkManager

#include <utility>

#include "config.h"
#include "logger.h"
#include "network_manager.h"

NetworkManager gNetwork;

bool NetworkManager::validHostname(const String &candidate) {
  const size_t len = candidate.length();
  if (len == 0 || len > 63) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = candidate.charAt(i);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok) {
      return false;
    }
  }
  return candidate.charAt(0) != '-' && candidate.charAt(0) != '.';
}

void NetworkManager::begin(const AppSettings &settings) {
  ssid_ = settings.wifiSsid;
  password_ = settings.wifiPassword;
  hostname_ = settings.hostname[0] != '\0' ? String(settings.hostname) : String(MDNS_HOSTNAME);
  hostname_.toLowerCase();
  if (!validHostname(hostname_)) {
    gLogger.info("hostname \"%s\" is invalid, falling back to \"%s\"", hostname_.c_str(), MDNS_HOSTNAME);
    hostname_ = MDNS_HOSTNAME;
  }

  started_ = true;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname_.c_str());
  WiFi.setAutoReconnect(false);  // we run our own bounded backoff

  if (ssid_.isEmpty()) {
    gLogger.info("no WiFi SSID configured, starting rescue AP \"%s\"", WIFI_FALLBACK_AP_SSID);
    startAp();
  } else {
    startSta();
  }
}

void NetworkManager::startSta() {
  if (ap_mode_) {
    WiFi.softAPdisconnect(true);
    ap_mode_ = false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname_.c_str());
  WiFi.begin(ssid_.c_str(), password_.c_str());
  connecting_ = true;
  link_up_ = false;
  attempt_ms_ = millis();
  retry_at_ms_ = attempt_ms_ + WIFI_CONNECT_TIMEOUT_MS;
  gLogger.info("connecting to WiFi \"%s\"", ssid_.c_str());
}

void NetworkManager::startAp() {
  stopMdns();
  link_up_ = false;
  connecting_ = false;
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(hostname_.c_str());
  ap_mode_ = WiFi.softAP(WIFI_FALLBACK_AP_SSID, WIFI_FALLBACK_AP_PASSWORD);
  if (ap_mode_) {
    WiFi.softAPsetHostname(hostname_.c_str());
    gLogger.info("setup AP \"%s\" is up at http://%s/ - configure your WiFi network",
         WIFI_FALLBACK_AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    gLogger.info("failed to start the setup AP");
  }
}

void NetworkManager::startMdns() {
  if (mdns_started_) {
    return;
  }
  if (MDNS.begin(hostname_.c_str())) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addServiceTxt("http", "tcp", "fw", FW_NAME);
    MDNS.addServiceTxt("http", "tcp", "version", FW_VERSION);
    mdns_started_ = true;
    gLogger.info("mDNS: http://%s.local", hostname_.c_str());
  } else {
    gLogger.info("mDNS start failed for \"%s\"", hostname_.c_str());
  }
}

void NetworkManager::stopMdns() {
  if (mdns_started_) {
    MDNS.end();
    mdns_started_ = false;
  }
}

void NetworkManager::scheduleRetry(uint32_t now) {
  retry_at_ms_ = now + backoff_ms_;
  uint32_t next = backoff_ms_ * 2U;
  if (next < 2000U) {
    next = 2000U;
  }
  if (next > 60000U) {
    next = 60000U;
  }
  backoff_ms_ = next;
}

void NetworkManager::loop() {
  if (!started_) {
    return;
  }
  const uint32_t now = millis();

  handleScan();

  if (hostname_dirty_) {
    hostname_dirty_ = false;
    WiFi.setHostname(hostname_.c_str());
    if (!ap_mode_ && link_up_) {
      stopMdns();
      startMdns();
    }
  }

  if (reconfigure_) {
    reconfigure_ = false;
    link_up_ = false;
    connecting_ = false;
    stopMdns();
    backoff_ms_ = WIFI_RETRY_INTERVAL_MS;
    if (ssid_.isEmpty()) {
      gLogger.info("WiFi credentials cleared, starting rescue AP \"%s\"", WIFI_FALLBACK_AP_SSID);
      startAp();
    } else {
      WiFi.disconnect(false, false);
      startSta();
    }
    return;
  }

  if (ap_mode_) {
    return;
  }

  if (ssid_.isEmpty()) {
    // No STA credentials: only the rescue AP is possible.  If softAP() failed
    // earlier, retry it occasionally instead of spinning on WiFi.begin().
    if (static_cast<int32_t>(now - retry_at_ms_) >= 0) {
      retry_at_ms_ = now + 10000U;
      gLogger.info("rescue AP is not running, retrying");
      startAp();
    }
    return;
  }

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    if (!link_up_) {
      link_up_ = true;
      connecting_ = false;
      backoff_ms_ = WIFI_RETRY_INTERVAL_MS;
      gLogger.info("WiFi connected: ssid=\"%s\" ip=%s rssi=%d", WiFi.SSID().c_str(),
           WiFi.localIP().toString().c_str(), static_cast<int>(WiFi.RSSI()));
      startMdns();
    }
    return;
  }

  if (link_up_) {
    link_up_ = false;
    connecting_ = false;
    stopMdns();
    gLogger.info("WiFi link lost (status %d), scheduling a reconnect", static_cast<int>(status));
    scheduleRetry(now);
    return;
  }

  if (connecting_) {
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      connecting_ = false;
      gLogger.info("WiFi connection failed (status %d), retrying later", static_cast<int>(status));
      WiFi.disconnect(false, false);
      scheduleRetry(now);
      return;
    }
    if (static_cast<int32_t>(now - attempt_ms_) >= static_cast<int32_t>(WIFI_CONNECT_TIMEOUT_MS)) {
      connecting_ = false;
      gLogger.info("WiFi connect timed out after %u ms", static_cast<unsigned>(WIFI_CONNECT_TIMEOUT_MS));
      WiFi.disconnect(false, false);
      scheduleRetry(now);
    }
    return;
  }

  if (static_cast<int32_t>(now - retry_at_ms_) >= 0) {
    gLogger.info("retrying WiFi (next backoff %u ms)", static_cast<unsigned>(backoff_ms_));
    startSta();
  }
}

bool NetworkManager::isConnected() const {
  return link_up_ || ap_mode_;
}

String NetworkManager::ipAddress() const {
  if (ap_mode_) {
    return WiFi.softAPIP().toString();
  }
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return String();
}

String NetworkManager::macAddress() const {
  return ap_mode_ ? WiFi.softAPmacAddress() : WiFi.macAddress();
}

int32_t NetworkManager::rssi() const {
  return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
}

String NetworkManager::hostname() const {
  return hostname_;
}

String NetworkManager::ssid() const {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.SSID();
  }
  return ssid_;
}

void NetworkManager::setHostname(const String &hostname) {
  String candidate = hostname;
  candidate.trim();
  candidate.toLowerCase();
  if (!validHostname(candidate)) {
    gLogger.info("rejected hostname \"%s\": use 1..63 chars of a-z, 0-9, '-' or '.'", hostname.c_str());
    return;
  }
  if (candidate == hostname_) {
    return;
  }
  hostname_ = candidate;
  if (started_) {
    hostname_dirty_ = true;
  }
  gLogger.info("hostname set to %s.local", hostname_.c_str());
}

void NetworkManager::applySettings(const AppSettings &settings) {
  const bool credentialsChanged =
      (ssid_ != String(settings.wifiSsid)) || (password_ != String(settings.wifiPassword));
  ssid_ = settings.wifiSsid;
  password_ = settings.wifiPassword;

  const String requestedHostname = settings.hostname[0] != '\0' ? String(settings.hostname) : String(MDNS_HOSTNAME);
  setHostname(requestedHostname);

  if (credentialsChanged) {
    reconfigure_ = true;
    gLogger.info("WiFi credentials changed, reconnecting");
  }
}

void NetworkManager::reconnect() {
  if (!started_ || ap_mode_ || ssid_.isEmpty()) {
    return;
  }
  gLogger.info("manual WiFi reconnect requested");
  link_up_ = false;
  connecting_ = false;
  stopMdns();
  backoff_ms_ = WIFI_RETRY_INTERVAL_MS;
  WiFi.disconnect(false, false);
  startSta();
}

void NetworkManager::startScan(ScanCallback cb) {
  scan_cb_ = std::move(cb);

  if (scanning_) {
    // The in-flight scan will deliver its result to the newest callback once,
    // from loop().  Never start a second concurrent scan.
    return;
  }

  if (ap_mode_) {
    scanning_ = true;
    scan_empty_pending_ = true;
    scan_started_ms_ = millis();
    return;
  }

  scanning_ = true;
  scan_empty_pending_ = false;
  scan_started_ms_ = millis();
  const int16_t rc = WiFi.scanNetworks(true);
  if (rc == WIFI_SCAN_RUNNING || rc >= 0) {
    return;
  }

  // The driver refused the scan; deliver an empty result on the next tick so
  // the callback still fires exactly once.
  scan_empty_pending_ = true;
}

bool NetworkManager::isScanning() const {
  return scanning_;
}

void NetworkManager::handleScan() {
  if (!scanning_) {
    return;
  }

  const uint32_t now = millis();
  if (scan_empty_pending_) {
    scanning_ = false;
    scan_empty_pending_ = false;
    if (scan_cb_) {
      ScanCallback cb = scan_cb_;
      scan_cb_ = nullptr;
      const std::vector<WifiScanEntry> empty;
      cb(empty);
    }
    return;
  }

  const int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    if (static_cast<int32_t>(now - scan_started_ms_) > 30000) {
      gLogger.info("WiFi scan timed out");
      WiFi.scanDelete();
      scanning_ = false;
      if (scan_cb_) {
        ScanCallback cb = scan_cb_;
        scan_cb_ = nullptr;
        const std::vector<WifiScanEntry> empty;
        cb(empty);
      }
    }
    return;
  }

  std::vector<WifiScanEntry> entries;
  if (result > 0) {
    entries.reserve(static_cast<size_t>(result));
    for (int16_t i = 0; i < result; ++i) {
      WifiScanEntry entry;
      entry.ssid = WiFi.SSID(i);
      entry.rssi = WiFi.RSSI(i);
      entry.channel = static_cast<uint8_t>(WiFi.channel(i));
      entry.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      entries.push_back(entry);
    }
  }
  WiFi.scanDelete();
  scanning_ = false;

  if (scan_cb_) {
    ScanCallback cb = scan_cb_;
    scan_cb_ = nullptr;
    cb(entries);
  }
}
