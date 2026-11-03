// Host-side FS shim for the FruitSnack simulation.
// Not part of the firmware. See tools/sim/shim/Arduino.h.
//
// An in-memory filesystem with just enough of the Arduino FS API for the
// settings store: files live in a std::map<std::string, std::string>.
#pragma once

#include <map>
#include <string>

#include "Arduino.h"

#ifndef FILE_READ
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
#endif

class File : public Stream {
 public:
  File() = default;
  File(std::map<std::string, std::string> *fs, const std::string &path, const char *mode)
      : fs_(fs), path_(path), mode_(mode) {
    if (mode_ != nullptr && (mode_[0] == 'w' || mode_[0] == 'a')) {
      if (fs_ != nullptr && mode_[0] == 'w') (*fs_)[path_].clear();
      open_ = true;
      writing_ = true;
    } else if (fs_ != nullptr) {
      auto it = fs_->find(path_);
      if (it != fs_->end()) {
        data_ = it->second;
        pos_ = 0;
        open_ = true;
      }
    }
  }

  operator bool() const { return open_; }
  bool isOpen() const { return open_; }
  size_t size() const { return writing_ ? buffer_.size() : data_.size(); }
  const char *name() const { return path_.c_str(); }

  void close() {
    flush();
    open_ = false;
  }

  int available() { return (int)(size() - pos_); }
  int read() {
    if (pos_ >= data_.size()) return -1;
    return (uint8_t)data_[pos_++];
  }
  int peek() {
    if (pos_ >= data_.size()) return -1;
    return (uint8_t)data_[pos_];
  }
  size_t readBytes(uint8_t *buf, size_t len) {
    size_t n = std::min(len, data_.size() - pos_);
    memcpy(buf, data_.data() + pos_, n);
    pos_ += n;
    return n;
  }
  String readString() {
    String out;
    while (pos_ < data_.size()) out += data_[pos_++];
    return out;
  }
  String readStringUntil(char terminator) {
    String out;
    while (pos_ < data_.size() && data_[pos_] != terminator) out += data_[pos_++];
    if (pos_ < data_.size()) ++pos_;
    return out;
  }

  size_t write(uint8_t c) override {
    buffer_ += (char)c;
    return 1;
  }
  size_t write(const uint8_t *data, size_t len) override {
    buffer_.append((const char *)data, len);
    return len;
  }
  int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) buffer_.append(buf, (size_t)n);
    return n;
  }

  /// Push the write buffer into the in-memory filesystem.
  void flush() {
    if (!writing_ || fs_ == nullptr) return;
    (*fs_)[path_] = buffer_;
  }

 private:
  std::map<std::string, std::string> *fs_ = nullptr;
  std::string path_;
  std::string data_;
  std::string buffer_;
  const char *mode_ = nullptr;
  size_t pos_ = 0;
  bool open_ = false;
  bool writing_ = false;
};

class FS {
 public:
  virtual ~FS() = default;
  virtual File open(const char *path, const char *mode = FILE_READ, const bool create = false) {
    (void)create;
    return File(&files_, path, mode);
  }
  File open(const String &path, const char *mode = FILE_READ, const bool create = false) {
    return open(path.c_str(), mode, create);
  }
  bool exists(const char *path) { return files_.count(path) > 0; }
  bool exists(const String &path) { return exists(path.c_str()); }
  bool remove(const char *path) { return files_.erase(path) > 0; }
  bool remove(const String &path) { return remove(path.c_str()); }
  bool rename(const char *from, const char *to) {
    auto it = files_.find(from);
    if (it == files_.end()) return false;
    files_[to] = it->second;
    files_.erase(it);
    return true;
  }
  bool rename(const String &from, const String &to) { return rename(from.c_str(), to.c_str()); }
  const char *mountpoint() { return "/littlefs"; }

 protected:
  std::map<std::string, std::string> files_;
};
