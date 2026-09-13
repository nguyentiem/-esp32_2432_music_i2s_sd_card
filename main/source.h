#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
// One producer owns the file/HTTP handle, one player consumes the stream.
class Source {
public:
  explicit Source(std::string uri) : uri_(std::move(uri)) {}
  ~Source();
  Source(const Source &) = delete;
  Source &operator=(const Source &) = delete;
  bool start();
  size_t read(void *dst, size_t bytes, const std::function<bool()> &service);
  bool skip(size_t bytes, const std::function<bool()> &service);
  bool error() const { return failed_; }
  bool buffering() const { return prebuffer_ && !done_; }
  bool eof() const { return done_ && (!buffer_ || !xStreamBufferBytesAvailable(buffer_)); }
  uint64_t size() const { return size_; }

private:
  static void run(void *arg);
  void produce();
  std::string uri_;
  StreamBufferHandle_t buffer_ = nullptr;
  SemaphoreHandle_t exited_ = nullptr;
  bool started_ = false, prebuffer_ = true;
  std::atomic<bool> cancel_{false}, done_{false}, failed_{false};
  std::atomic<uint32_t> size_{0};
  uint32_t rebufferCount_ = 0;
};
