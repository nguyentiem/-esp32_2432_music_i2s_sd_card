#pragma once
#include "config.h"
#include <AudioFileSource.h>
#include <HTTPClient.h>
#include <SD.h>
#include <atomic>
#include <freertos/stream_buffer.h>
#include <functional>
// Single producer (core 0), single consumer (decoder core 1).
class BufferedSource : public AudioFileSource {
  StreamBufferHandle_t buffer = nullptr;
  TaskHandle_t task = nullptr;
  String uri;
  std::atomic<bool> cancel{false}, done{false}, failed{false};
  uint32_t position = 0;
  static void run(void *p) { static_cast<BufferedSource *>(p)->produce(); }
  void produce() {
    File file;
    HTTPClient http;
    WiFiClient client;
    Stream *input = nullptr;
    int64_t remaining = -1;
    if (uri.startsWith("http://")) {
      http.useHTTP10(true); // Avoid chunked transfer framing in the decoder.
      http.setConnectTimeout(5000);
      http.setTimeout(3000);
      for (int attempt = 0; attempt < 4 && !cancel; ++attempt) {
        if (http.begin(client, uri)) {
          const char *headers[] = {"Transfer-Encoding"};
          http.collectHeaders(headers, 1);
          if (http.GET() == 200 && !http.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
            input = http.getStreamPtr();
            remaining = http.getSize();
            break;
          }
        }
        http.end();
        if (attempt < 3)
          for (int wait = 0; wait < (500 << attempt) && !cancel; wait += 50)
            vTaskDelay(pdMS_TO_TICKS(50));
      }
    } else {
      file = SD.open(uri);
      if (file) {
        input = &file;
        remaining = file.size();
      }
    }
    if (!input)
      failed = true;
    uint8_t chunk[1024];
    uint32_t last = millis();
    while (input && !cancel && remaining != 0) {
      int available = input->available();
      if (!available) {
        if (file || !http.connected()) {
          if (remaining > 0)
            failed = true;
          break;
        }
        if (millis() - last > 6000) {
          failed = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      size_t want = std::min(static_cast<size_t>(available), sizeof(chunk));
      if (remaining > 0)
        want = std::min(want, static_cast<size_t>(remaining));
      int n = input->readBytes(chunk, want);
      if (n <= 0) {
        failed = true;
        break;
      }
      last = millis();
      if (remaining > 0)
        remaining -= n;
      size_t sent = 0;
      while (sent < static_cast<size_t>(n) && !cancel)
        sent += xStreamBufferSend(buffer, chunk + sent, n - sent, pdMS_TO_TICKS(50));
      last = millis();
    }
    file.close();
    http.end();
    done = true;
    vTaskDelete(nullptr);
  }

public:
  std::function<void()> onWait;
  bool start(const String &path) {
    uri = path;
    buffer = xStreamBufferCreate(SOURCE_BUFFER, 1);
    if (!buffer)
      return false;
    if (xTaskCreatePinnedToCore(run, "source", 6144, this, 2, &task, 0) != pdPASS) {
      vStreamBufferDelete(buffer);
      buffer = nullptr;
      return false;
    }
    return true;
  }
  ~BufferedSource() override { close(); }
  bool close() override {
    cancel = true;
    if (task) {
      while (!done)
        vTaskDelay(pdMS_TO_TICKS(10));
      task = nullptr;
    }
    if (buffer) {
      vStreamBufferDelete(buffer);
      buffer = nullptr;
    }
    return true;
  }
  uint32_t read(void *data, uint32_t len) override {
    if (!buffer)
      return 0;
    size_t n = 0;
    while (n < len && !cancel) {
      size_t got = xStreamBufferReceive(buffer, static_cast<uint8_t *>(data) + n, len - n,
                                        pdMS_TO_TICKS(10));
      n += got;
      if (!got && !done) {
        do {
          if (onWait)
            onWait();
          vTaskDelay(pdMS_TO_TICKS(5));
        } while (!ready() && !cancel);
      }
      if (done && !xStreamBufferBytesAvailable(buffer))
        break;
    }
    position += n;
    return n;
  }
  bool seek(int32_t offset, int whence) override {
    int64_t skip = whence == SEEK_CUR   ? offset
                   : whence == SEEK_SET ? int64_t(offset) - position
                                        : -1;
    if (skip < 0)
      return false;
    uint8_t scratch[128];
    while (skip > 0) {
      uint32_t n = std::min<int64_t>(skip, sizeof(scratch));
      if (read(scratch, n) != n)
        return false;
      skip -= n;
    }
    return true;
  }
  bool isOpen() override { return buffer != nullptr; }
  uint32_t getSize() override { return 0; }
  uint32_t getPos() override { return position; }
  bool ready() const {
    return done || (buffer && xStreamBufferBytesAvailable(buffer) >= PREBUFFER);
  }
  bool error() const { return failed; }
};
