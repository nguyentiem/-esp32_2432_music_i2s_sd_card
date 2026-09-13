#include "source.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

bool Source::start() {
  buffer_ = xStreamBufferCreate(SOURCE_BUFFER, 1);
  exited_ = xSemaphoreCreateBinary();
  if (!buffer_ || !exited_)
    return false;
  started_ = xTaskCreatePinnedToCore(run, "audio_source", 6144, this, 3, nullptr, 0) == pdPASS;
  return started_;
}
Source::~Source() {
  cancel_ = true;
  if (started_)
    xSemaphoreTake(exited_, portMAX_DELAY);
  if (started_)
    ESP_LOGI("source", "Ended: rebuffer events=%lu, error=%d",
             (unsigned long)rebufferCount_, int(failed_.load()));
  if (buffer_)
    vStreamBufferDelete(buffer_);
  if (exited_)
    vSemaphoreDelete(exited_);
}
void Source::run(void *arg) {
  auto *self = static_cast<Source *>(arg);
  self->produce();
  // No object access after signaling: the player may now destroy it.
  auto finished = self->exited_;
  xSemaphoreGive(finished);
  vTaskDelete(nullptr);
}
void Source::produce() {
  FILE *file = nullptr;
  esp_http_client_handle_t http = nullptr;
  int64_t remaining = -1;
  if (uri_.compare(0, 7, "http://") == 0) {
    wifi_ap_record_t ap = {};
    int64_t deadline = esp_timer_get_time() + 12000000;
    while (strlen(WIFI_SSID) && !cancel_ && esp_wifi_sta_get_ap_info(&ap) != ESP_OK &&
           esp_timer_get_time() < deadline)
      vTaskDelay(pdMS_TO_TICKS(20));
    if (cancel_ || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
      failed_ = true;
      done_ = true;
      return;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = uri_.c_str();
    cfg.timeout_ms = 1500;
    cfg.buffer_size = 2048;
    cfg.disable_auto_redirect = true;
    http = esp_http_client_init(&cfg);
    bool opened = false;
    for (int attempt = 0; http && attempt < 4 && !cancel_; ++attempt) {
      if (esp_http_client_open(http, 0) == ESP_OK) {
        auto length = esp_http_client_fetch_headers(http);
        if (length >= 0 && esp_http_client_get_status_code(http) == 200) {
          remaining = esp_http_client_is_chunked_response(http)
                          ? -1
                          : esp_http_client_get_content_length(http);
          if (remaining > 0 && remaining <= UINT32_MAX)
            size_ = remaining;
          opened = true;
          break;
        }
      }
      esp_http_client_close(http);
      if (attempt < 3)
        for (int t = 0; t < (500 << attempt) && !cancel_; t += 20)
          vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!opened)
      failed_ = true;
  } else {
    file = fopen(uri_.c_str(), "rb");
    struct stat st = {};
    if (!file || fstat(fileno(file), &st) != 0)
      failed_ = true;
    else {
      remaining = st.st_size;
      size_ = st.st_size;
    }
  }
  uint8_t chunk[1024];
  while (!failed_ && !cancel_ && remaining != 0) {
    size_t want = remaining > 0 ? std::min<int64_t>(remaining, sizeof(chunk)) : sizeof(chunk);
    int n = file ? static_cast<int>(fread(chunk, 1, want, file))
                 : esp_http_client_read(http, reinterpret_cast<char *>(chunk), want);
    if (n <= 0) {
      if (n < 0 || remaining > 0 || (file && ferror(file)) ||
          (http && esp_http_client_is_chunked_response(http) &&
           !esp_http_client_is_complete_data_received(http)))
        failed_ = true;
      break;
    }
    if (remaining > 0)
      remaining -= n;
    size_t sent = 0;
    while (sent < static_cast<size_t>(n) && !cancel_)
      sent += xStreamBufferSend(buffer_, chunk + sent, n - sent, pdMS_TO_TICKS(20));
  }
  if (file)
    fclose(file);
  if (http) {
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
  }
  done_ = true;
}
size_t Source::read(void *dst, size_t bytes, const std::function<bool()> &service) {
  size_t count = 0;
  while (count < bytes && !cancel_) {
    if (!service())
      break;
    if (prebuffer_) {
      if (!done_ && xStreamBufferBytesAvailable(buffer_) < PREBUFFER) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      prebuffer_ = false;
    }
    size_t got = xStreamBufferReceive(buffer_, static_cast<uint8_t *>(dst) + count, bytes - count,
                                      pdMS_TO_TICKS(10));
    count += got;
    if (eof())
      break;
    if (!got) {
      prebuffer_ = true;
      ++rebufferCount_;
    }
  }
  return count;
}
bool Source::skip(size_t bytes, const std::function<bool()> &service) {
  uint8_t scratch[256];
  while (bytes) {
    size_t n = std::min(bytes, sizeof(scratch));
    if (read(scratch, n, service) != n)
      return false;
    bytes -= n;
  }
  return true;
}
