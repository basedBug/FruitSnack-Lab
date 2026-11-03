// FruitSnack — logger implementation.  See logger.h for the contract.
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <utility>

Logger gLogger;

const char *logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

namespace {

const char *levelTag(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "INFO";
}

}  // namespace

Logger::Logger() {
  mutex_ = xSemaphoreCreateMutex();
}

void Logger::begin(Stream *serial, Stream *uart) {
  serial_ = serial;
  uart_ = uart;
}

void Logger::addSink(Sink sink) {
  if (!sink || sinkCount_ >= kMaxSinks) {
    return;
  }
  sinks_[sinkCount_++] = std::move(sink);
}

void Logger::setLevel(LogLevel level) {
  level_ = level;
}

LogLevel Logger::level() const {
  return level_;
}

void Logger::setUartEnabled(bool enabled) {
  uartEnabled_ = enabled;
}

void Logger::writeStream(Stream *stream, const char *line, size_t len) {
  if (stream == nullptr || line == nullptr || len == 0) {
    return;
  }
  // Never block the caller: only write when the whole line fits, otherwise
  // drop it for this stream.
  if (stream->availableForWrite() >= static_cast<int>(len)) {
    stream->write(reinterpret_cast<const uint8_t *>(line), len);
  }
}

void Logger::log(LogLevel level, const char *fmt, ...) {
  if (static_cast<uint8_t>(level) < static_cast<uint8_t>(level_)) {
    return;
  }

  char message[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  message[sizeof(message) - 1] = '\0';

  const uint32_t now = millis();
  char line[224];
  const int len = snprintf(line, sizeof(line), "[%lu.%03lu] [%s] %s\r\n",
                           static_cast<unsigned long>(now / 1000U),
                           static_cast<unsigned long>(now % 1000U), levelTag(level), message);
  if (len <= 0) {
    return;
  }
  const size_t lineLen = (static_cast<size_t>(len) < sizeof(line)) ? static_cast<size_t>(len) : sizeof(line) - 1;

  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  history_[head_].tsMs = now;
  history_[head_].level = level;
  strlcpy(history_[head_].text, message, sizeof(history_[head_].text));
  head_ = (head_ + 1U) % kHistorySize;
  if (count_ < kHistorySize) {
    ++count_;
  }
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }

  writeStream(serial_, line, lineLen);
  if (uartEnabled_) {
    writeStream(uart_, line, lineLen);
  }

  // Sinks are registered once during setup, so iterating without the lock is
  // safe and keeps a slow sink from holding up the history ring.
  for (size_t i = 0; i < sinkCount_; ++i) {
    if (sinks_[i]) {
      sinks_[i](level, message);
    }
  }
}

void Logger::info(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char message[192];
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  log(LogLevel::Info, "%s", message);
}

void Logger::warn(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char message[192];
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  log(LogLevel::Warn, "%s", message);
}

void Logger::error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char message[192];
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  log(LogLevel::Error, "%s", message);
}

size_t Logger::recent(size_t maxLines, LogLine *out) const {
  if (out == nullptr || maxLines == 0) {
    return 0;
  }
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  size_t n = (maxLines < count_) ? maxLines : count_;
  const size_t start = (head_ + kHistorySize - n) % kHistorySize;
  for (size_t i = 0; i < n; ++i) {
    out[i] = history_[(start + i) % kHistorySize];
  }
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return n;
}

void Logger::clearHistory() {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  head_ = 0;
  count_ = 0;
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
}
