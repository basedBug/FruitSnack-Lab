// FruitSnack — tiny thread-safe logger.
//
// Every record is written to the USB-CDC stream, the hardware UART and every
// registered sink (the websocket broadcaster).  A 200 line ring buffer is kept
// so the web UI can fetch recent history from /api/logs.
//
// Writes to the serial streams are best-effort: a line is only written when the
// stream has room for it, otherwise it is dropped for that stream.  The control
// loop must never stall because a debug console is full / disconnected.
#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <functional>

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/// One record in the recent-history ring buffer.
struct LogLine {
  uint32_t tsMs;
  LogLevel level;
  char text[192];
};

/// Human readable level name ("debug", "info", "warn", "error").
const char *logLevelName(LogLevel level);

class Logger {
 public:
  using Sink = std::function<void(LogLevel, const char *)>;
  static constexpr size_t kHistorySize = 200;

  Logger();

  /// Attach the output streams.  Either may be nullptr.
  void begin(Stream *serial, Stream *uart);

  /// Register a sink.  Sinks are only added during setup, never from a sink.
  void addSink(Sink sink);

  void setLevel(LogLevel level);
  LogLevel level() const;

  /// Enable or disable log output on the hardware UART.  The native USB
  /// stream and websocket sinks are unaffected.  Used while the UART carries
  /// binary telemetry so log text cannot corrupt the packet stream.
  void setUartEnabled(bool enabled);

  /// printf-style record.  Lines below the configured level are discarded.
  void log(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
  void info(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void warn(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void error(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

  /// Copy up to `maxLines` history records, oldest first.  Returns the count.
  size_t recent(size_t maxLines, LogLine *out) const;

  void clearHistory();

 private:
  void writeStream(Stream *stream, const char *line, size_t len);

  Stream *serial_ = nullptr;
  Stream *uart_ = nullptr;
  LogLevel level_ = LogLevel::Info;
  bool uartEnabled_ = true;

  static constexpr size_t kMaxSinks = 4;
  Sink sinks_[kMaxSinks];
  size_t sinkCount_ = 0;

  LogLine history_[kHistorySize];
  size_t head_ = 0;  ///< next write slot
  size_t count_ = 0; ///< valid records (<= kHistorySize)
  SemaphoreHandle_t mutex_ = nullptr;
};

extern Logger gLogger;

#ifndef LOG_DEBUG
#define LOG_DEBUG(...) ::gLogger.log(::LogLevel::Debug, __VA_ARGS__)
#endif
#ifndef LOG_INFO
#define LOG_INFO(...) ::gLogger.log(::LogLevel::Info, __VA_ARGS__)
#endif
#ifndef LOG_WARN
#define LOG_WARN(...) ::gLogger.log(::LogLevel::Warn, __VA_ARGS__)
#endif
#ifndef LOG_ERROR
#define LOG_ERROR(...) ::gLogger.log(::LogLevel::Error, __VA_ARGS__)
#endif
