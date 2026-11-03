// Host-side ESPmDNS shim for the FruitSnack simulation.
// Not part of the firmware. See tools/sim/shim/Arduino.h.
#pragma once

#include <string>

#include "Arduino.h"

class ESPMDNSClass {
 public:
  bool begin(const char *hostName) {
    hostname_ = hostName ? hostName : "";
    return true;
  }
  bool begin(const String &hostName) { return begin(hostName.c_str()); }
  void end() { started_ = false; }
  bool addService(const char *service, const char *proto, uint16_t port) {
    (void)service; (void)proto; (void)port;
    return true;
  }
  bool addServiceTxt(const char *service, const char *proto, const char *key, const char *value) {
    (void)service; (void)proto; (void)key; (void)value;
    return true;
  }
  void setInstanceName(const String &name) { instance_ = name; }
  String hostname() const { return hostname_; }

 private:
  bool started_ = true;
  String hostname_;
  String instance_;
};

extern ESPMDNSClass MDNS;
