// Host-side Arduino shim for the FruitSnack control-core simulation.
//
// This file is NOT part of the firmware. It provides the small slice of the
// Arduino / ESP-IDF API that the control core touches, so that the real
// src/pid.cpp, src/ntc_sensor.cpp, src/heater.cpp and
// src/heater_controller.cpp can be compiled and exercised natively on the
// development machine (tools/sim/sim_main.cpp).
//
// Everything is deterministic and driven by a virtual clock, so a simulation
// run is reproducible.
#pragma once

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ------------------------------------------------------------------ GPIO numbers
// ESP32-S3 GPIO identifiers (the real core exposes these from the SoC headers).
#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#define GPIO_NUM_9 9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_21 21
#define GPIO_NUM_22 22
#define GPIO_NUM_23 23
#define GPIO_NUM_24 24
#define GPIO_NUM_25 25
#define GPIO_NUM_26 26
#define GPIO_NUM_27 27
#define GPIO_NUM_28 28
#define GPIO_NUM_29 29
#define GPIO_NUM_30 30
#define GPIO_NUM_31 31
#define GPIO_NUM_32 32
#define GPIO_NUM_33 33
#define GPIO_NUM_34 34
#define GPIO_NUM_35 35
#define GPIO_NUM_36 36
#define GPIO_NUM_37 37
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_43 43
#define GPIO_NUM_44 44
#define GPIO_NUM_45 45
#define GPIO_NUM_46 46
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48

// ------------------------------------------------------------------ clock
namespace simclock {
extern uint32_t nowMs;
void advance(uint32_t deltaMs);
}  // namespace simclock

inline uint32_t millis() { return simclock::nowMs; }
inline uint32_t micros() { return simclock::nowMs * 1000u; }
inline void delay(uint32_t ms) { simclock::advance(ms); }
inline void delayMicroseconds(uint32_t us) { (void)us; }
inline void yield() {}

// ------------------------------------------------------------------ types
typedef bool boolean;
typedef uint8_t byte;
typedef unsigned int word;

#ifndef HIGH
#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#endif

// ------------------------------------------------------------------ ADC
// Only ADC_11db exists in Arduino core 3.x (ADC_ATTENDB_MAX == ADC_11db).
typedef enum {
  ADC_0db = 0,
  ADC_2_5db = 1,
  ADC_6db = 2,
  ADC_11db = 3,
  ADC_ATTENDB_MAX = 3
} adc_attenuation_t;

namespace simadc {
/// The simulated divider node voltage in millivolts that the next
/// analogReadMilliVolts() call will return.
extern float nodeMillivolts;
extern uint16_t rawCount;
extern uint8_t attenuation;
/// Optional per-sample hook, called by analogReadMilliVolts() before every
/// conversion so a test can drive a different (mV, raw) pair per sample.
/// analogRead() then returns the raw count belonging to that same pair.
/// nullptr (the default) means one constant reading.
extern void (*sampleHook)(void);
}  // namespace simadc

inline uint32_t analogReadMilliVolts(uint8_t pin) {
  (void)pin;
  if (simadc::sampleHook != nullptr) simadc::sampleHook();
  return (uint32_t)lroundf(simadc::nodeMillivolts);
}

inline uint16_t analogRead(uint8_t pin) {
  (void)pin;
  return simadc::rawCount;
}

inline void analogReadResolution(uint8_t bits) { (void)bits; }
inline void analogSetAttenuation(adc_attenuation_t atten) { simadc::attenuation = (uint8_t)atten; }
inline void analogSetPinAttenuation(uint8_t pin, adc_attenuation_t atten) {
  (void)pin;
  simadc::attenuation = (uint8_t)atten;
}

// ------------------------------------------------------------------ GPIO / LEDC
namespace simgpio {
/// Duty count last written to each pin (indexed by pin number).
extern uint32_t ledcDuty[64];
extern uint32_t ledcFreq[64];
extern uint8_t ledcResolution[64];
extern uint8_t pinMode[64];
extern uint8_t pinLevel[64];
}  // namespace simgpio

inline void pinMode(uint8_t pin, uint8_t mode) { simgpio::pinMode[pin & 63] = mode; }
inline void digitalWrite(uint8_t pin, uint8_t value) { simgpio::pinLevel[pin & 63] = value; }
inline int digitalRead(uint8_t pin) { return simgpio::pinLevel[pin & 63]; }

inline bool ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolution) {
  simgpio::ledcFreq[pin & 63] = freq;
  simgpio::ledcResolution[pin & 63] = resolution;
  return true;
}

inline bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, uint8_t channel) {
  (void)channel;
  return ledcAttach(pin, freq, resolution);
}

inline bool ledcWrite(uint8_t pin, uint32_t duty) {
  simgpio::ledcDuty[pin & 63] = duty;
  return true;
}

inline bool ledcWriteChannel(uint8_t channel, uint32_t duty) {
  simgpio::ledcDuty[channel & 63] = duty;
  return true;
}

// ------------------------------------------------------------------ String
// A small Arduino-String-compatible class for the host build.
class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String(int v) { char b[24]; snprintf(b, sizeof(b), "%d", v); s_ = b; }
  String(unsigned v) { char b[24]; snprintf(b, sizeof(b), "%u", v); s_ = b; }
  String(long v) { char b[32]; snprintf(b, sizeof(b), "%ld", v); s_ = b; }
  String(unsigned long v) { char b[32]; snprintf(b, sizeof(b), "%lu", v); s_ = b; }
  String(float v, unsigned char decimals = 2) {
    char b[48];
    snprintf(b, sizeof(b), "%.*f", (int)decimals, (double)v);
    s_ = b;
  }
  String(double v, unsigned char decimals = 2) {
    char b[48];
    snprintf(b, sizeof(b), "%.*f", (int)decimals, v);
    s_ = b;
  }
  const char *c_str() const { return s_.c_str(); }
  unsigned int length() const { return (unsigned int)s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  String &operator+=(char c) { s_ += c; return *this; }
  String &operator+=(const char *p) { s_ += p; return *this; }
  String &operator+=(const String &o) { s_ += o.s_; return *this; }
  float toFloat() const { return (float)atof(s_.c_str()); }
  double toDouble() const { return atof(s_.c_str()); }
  int toInt() const { return atoi(s_.c_str()); }
  bool startsWith(const char *p) const { return s_.rfind(p, 0) == 0; }
  bool endsWith(const char *p) const {
    size_t n = strlen(p);
    return s_.size() >= n && s_.compare(s_.size() - n, n, p) == 0;
  }
  bool equals(const char *p) const { return s_ == p; }
  int indexOf(const char *p) const {
    size_t pos = s_.find(p);
    return pos == std::string::npos ? -1 : (int)pos;
  }
  char charAt(unsigned index) const { return index < s_.size() ? s_[index] : '\0'; }
  void toCharArray(char *buffer, size_t size, bool terminate = true) const {
    if (size == 0) return;
    size_t n = s_.size() < size - 1 ? s_.size() : size - 1;
    memcpy(buffer, s_.data(), n);
    if (terminate && size > 0) buffer[n] = '\0';
  }
  void reserve(size_t n) { s_.reserve(n); }
  String substring(unsigned from) const { return String(s_.substr(from)); }
  String substring(unsigned from, unsigned to) const { return String(s_.substr(from, to - from)); }
  String trim() const {
    size_t a = s_.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return String();
    size_t b = s_.find_last_not_of(" \t\r\n");
    return String(s_.substr(a, b - a + 1));
  }
  void toLowerCase() {
    for (auto &c : s_) c = (char)tolower((unsigned char)c);
  }
  void toUpperCase() {
    for (auto &c : s_) c = (char)toupper((unsigned char)c);
  }
  const std::string &str() const { return s_; }
  // ArduinoJson writes into a String through these two methods.
  size_t write(uint8_t c) {
    s_ += static_cast<char>(c);
    return 1;
  }
  size_t write(const uint8_t *data, size_t len) {
    s_.append(reinterpret_cast<const char *>(data), len);
    return len;
  }
  bool operator==(const char *p) const { return s_ == p; }
  bool operator!=(const char *p) const { return s_ != p; }
  bool operator==(const String &o) const { return s_ == o.s_; }
  bool operator!=(const String &o) const { return s_ != o.s_; }

  friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
  friend String operator+(const String &a, const char *b) { return String(a.s_ + (b ? b : "")); }
  friend String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s_); }
  friend String operator+(const String &a, char b) { return String(a.s_ + b); }

 private:
  std::string s_;
};

// ------------------------------------------------------------------ Stream
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) {
    fputc(c, stdout);
    return 1;
  }
  virtual size_t write(const uint8_t *buf, size_t len) {
    fwrite(buf, 1, len, stdout);
    return len;
  }
  size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t print(const String &s) { return write((const uint8_t *)s.c_str(), s.length()); }
  size_t print(int v) { return printf("%d", v); }
  size_t print(unsigned v) { return printf("%u", v); }
  size_t print(float v) { return printf("%f", v); }
  size_t println() { return print("\n"); }
  size_t println(const char *s) { return print(s) + println(); }
  int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
  }
};

class Stream : public Print {
 public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  size_t availableForWrite() { return 1024; }
  void flush() {}
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long baud) { (void)baud; }
  void begin(unsigned long baud, uint32_t config, int8_t rx, int8_t tx) {
    (void)baud; (void)config; (void)rx; (void)tx;
  }
  void setRxBufferSize(size_t n) { (void)n; }
  void setTxBufferSize(size_t n) { (void)n; }
  void flush() {}
  operator bool() const { return true; }
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;

// ------------------------------------------------------------------ ESP info
class EspClass {
 public:
  uint32_t getHeapSize() { return 320000; }
  uint32_t getFreeHeap() { return 210000; }
  uint32_t getMinFreeHeap() { return 198000; }
  uint32_t getMaxAllocHeap() { return 180000; }
  uint32_t getPsramSize() { return 2097152; }
  uint32_t getFreePsram() { return 1800000; }
  uint32_t getCpuFreqMHz() { return 240; }
  const char *getChipModel() { return "ESP32-S3"; }
  uint8_t getChipCores() { return 2; }
  uint8_t getChipRevision() { return 0; }
  const char *getSdkVersion() { return "v5.5.5-dirty"; }
  const char *getCoreVersion() { return "3.3.12"; }
  uint32_t getFlashChipSize() { return 8388608; }
  uint32_t getSketchSize() { return 0; }
  void restart() { fprintf(stderr, "[sim] ESP.restart() called\n"); }
};

extern EspClass ESP;

// ------------------------------------------------------------------ misc core API
// strlcpy is provided by ESP-IDF/newlib but not by the host toolchain.
#ifndef HAVE_STRLCPY
static inline size_t strlcpy(char *dst, const char *src, size_t size) {
  if (size == 0) return strlen(src);
  size_t len = strlen(src);
  size_t copy = len >= size ? size - 1 : len;
  memcpy(dst, src, copy);
  dst[copy] = '\0';
  return len;
}
#endif

inline uint32_t random(uint32_t bound) { return bound ? (rand() % bound) : 0; }
inline void randomSeed(unsigned long seed) { srand((unsigned)seed); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
inline void attachInterrupt(uint8_t, void (*)(), int) {}
inline void detachInterrupt(uint8_t) {}
