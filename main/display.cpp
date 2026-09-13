#include "config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "font.h"
#include "player.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
spi_device_handle_t lcd;
uint8_t bitmap[240][40];
uint16_t *stripe;
void tx(const void *data, size_t bytes, bool isData) {
  if (!bytes)
    return;
  gpio_set_level(GPIO_NUM_2, isData);
  spi_transaction_t t = {};
  t.length = bytes * 8;
  t.tx_buffer = data;
  ESP_ERROR_CHECK(spi_device_polling_transmit(lcd, &t));
}
void command(uint8_t c, const uint8_t *data = nullptr, size_t n = 0) {
  tx(&c, 1, false);
  tx(data, n, true);
}
void pixel(int x, int y) {
  if (x >= 0 && x < 320 && y >= 0 && y < 240)
    bitmap[y][x / 8] |= 0x80 >> (x % 8);
}
void rect(int x, int y, int w, int h, bool fill = false) {
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i)
      if (fill || i == 0 || j == 0 || i == w - 1 || j == h - 1)
        pixel(x + i, y + j);
}
uint32_t nextCode(const char *&s) {
  uint8_t c = *s++;
  if (c < 128)
    return c;
  int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  uint32_t value = c & ((1 << (6 - n)) - 1);
  if (!n)
    return '?';
  for (int i = 0; i < n; ++i) {
    if (!*s || (uint8_t(*s) & 0xc0) != 0x80)
      return '?';
    value = (value << 6) | (uint8_t(*s++) & 63);
  }
  return value;
}
void text(const char *s, int x, int y, int width, int lines = 1) {
  int left = x, row = 0;
  while (*s && row < lines) {
    uint32_t code = nextCode(s);
    auto glyph = std::lower_bound(std::begin(font), std::end(font), code,
                                  [](const Glyph &g, uint32_t c) { return g.code < c; });
    if (glyph == std::end(font) || glyph->code != code)
      glyph = &font['?' - 32];
    if (x + glyph->advance > left + width) {
      x = left;
      y += 21;
      if (++row >= lines)
        break;
    }
    for (int j = 0; j < 20; ++j)
      for (int i = 0; i < 16 && x + i < left + width; ++i)
        if (glyph->rows[j] & (0x8000 >> i))
          pixel(x + i, y + j);
    x += glyph->advance;
  }
}
uint16_t touchRead(uint8_t c) {
  gpio_set_level(GPIO_NUM_33, 0);
  for (int i = 7; i >= 0; --i) {
    gpio_set_level(GPIO_NUM_32, (c >> i) & 1);
    gpio_set_level(GPIO_NUM_25, 1);
    esp_rom_delay_us(2);
    gpio_set_level(GPIO_NUM_25, 0);
    esp_rom_delay_us(2);
  }
  uint16_t result = 0;
  for (int i = 0; i < 16; ++i) {
    gpio_set_level(GPIO_NUM_25, 1);
    esp_rom_delay_us(2);
    result = (result << 1) | gpio_get_level(GPIO_NUM_39);
    gpio_set_level(GPIO_NUM_25, 0);
    esp_rom_delay_us(2);
  }
  gpio_set_level(GPIO_NUM_33, 1);
  return (result >> 3) & 4095;
}
int coordinate(int value, int low, int high, int extent) {
  return high == low ? 0 : std::clamp((value - low) * extent / (high - low), 0, extent);
}
} // namespace
void displayInit() {
  gpio_config_t pins = {};
  pins.pin_bit_mask = (1ULL << 2) | (1ULL << 21) | (1ULL << 25) | (1ULL << 32) | (1ULL << 33);
  pins.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&pins));
  gpio_set_level(GPIO_NUM_21, 0);
  gpio_set_level(GPIO_NUM_33, 1);
  gpio_set_level(GPIO_NUM_25, 0);
  pins.pin_bit_mask = (1ULL << 36) | (1ULL << 39);
  pins.mode = GPIO_MODE_INPUT;
  ESP_ERROR_CHECK(gpio_config(&pins));
  spi_bus_config_t bus = {};
  bus.mosi_io_num = 13;
  bus.miso_io_num = 12;
  bus.sclk_io_num = 14;
  bus.quadwp_io_num = bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 320 * 20 * 2;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 20000000;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  dev.spics_io_num = 15;
  dev.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &lcd));
  stripe = static_cast<uint16_t *>(heap_caps_malloc(320 * 20 * 2, MALLOC_CAP_DMA));
  ESP_ERROR_CHECK(stripe ? ESP_OK : ESP_ERR_NO_MEM);
  command(0x01);
  vTaskDelay(pdMS_TO_TICKS(150));
  command(0x11);
  vTaskDelay(pdMS_TO_TICKS(120));
  // ST7789 values from esp32_2432_clock_touch/main/driver/st7789.c.
  const auto init = [](uint8_t cmd, std::initializer_list<uint8_t> data) {
    command(cmd, data.begin(), data.size());
  };
  init(0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33});
  init(0xb7, {0x35});
  init(0xbb, {0x19});
  init(0xc0, {0x2c});
  init(0xc2, {0x01});
  init(0xc3, {0x12});
  init(0xc4, {0x20});
  init(0xc6, {0x0f});
  init(0xd0, {0xa4, 0xa1});
  init(0xe0, {0xd0, 0x08, 0x11, 0x08, 0x0c, 0x15, 0x39, 0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2d});
  init(0xe1, {0xd0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44, 0x51, 0x0b, 0x16, 0x14, 0x2f, 0x31});
  init(0x36, {0x60}); // MX | MV: landscape rotation without mirrored text, RGB order.
  init(0x3a, {0x55}); // RGB565
  command(0x20);      // Inversion off, matching the reference panel.
  command(0x29);
  vTaskDelay(pdMS_TO_TICKS(20));
  displayDraw(view);
  gpio_set_level(GPIO_NUM_21, 1);
  ESP_LOGI("display", "ST7789 320x240 ready, SPI 20 MHz; XPT2046 software SPI");
}
void displayDraw(const View &v) {
  memset(bitmap, 0, sizeof(bitmap));
  text("CYD MUSIC / ESP-IDF", 12, 5, 296);
  const char *title = strrchr(v.title, '/');
  text(title ? title + 1 : v.title, 12, 31, 296, 2);
  text(v.status, 12, 76, 296);
  text(v.lyric, 12, 100, 296, 3);
  char status[64];
  if (v.total)
    snprintf(status, sizeof(status), "%02lu:%02lu / %02lu:%02lu   Vol %d%%",
             (unsigned long)(v.ms / 60000), (unsigned long)(v.ms / 1000 % 60),
             (unsigned long)(v.total / 60000), (unsigned long)(v.total / 1000 % 60), v.volume);
  else
    snprintf(status, sizeof(status), "%02lu:%02lu   Vol %d%%", (unsigned long)(v.ms / 60000),
             (unsigned long)(v.ms / 1000 % 60), v.volume);
  text(status, 12, 164, 296);
  rect(12, 187, 296, 6);
  if (v.total)
    rect(13, 188, std::min<uint64_t>(294, uint64_t(v.ms) * 294 / v.total), 4, true);
  // Draw geometric icons directly: no dependency on font symbol coverage.
  const auto triangle = [](int cx, int cy, bool right) {
    for (int dx = 0; dx < 15; ++dx) {
      const int half = (14 - dx) * 10 / 14;
      rect(cx + (right ? dx - 7 : 7 - dx), cy - half, 1, half * 2 + 1, true);
    }
  };
  for (int i = 0; i < 5; ++i) {
    const int cx = i * 64 + 32, cy = 219;
    rect(i * 64 + 2, 200, 60, 38);
    if (i == 0 || i == 2) {
      const bool next = i == 2;
      triangle(cx, cy, next);
      rect(cx + (next ? 10 : -13), cy - 10, 3, 21, true);
    } else if (i == 1) {
      if (v.playing) {
        rect(cx - 9, cy - 10, 6, 21, true);
        rect(cx + 3, cy - 10, 6, 21, true);
      } else {
        triangle(cx, cy, true);
      }
    } else {
      // Speaker cone with a separate minus/plus sign.
      rect(cx - 17, cy - 4, 6, 9, true);
      for (int dx = 0; dx < 8; ++dx)
        rect(cx - 11 + dx, cy - 4 - dx, 1, 9 + 2 * dx, true);
      rect(cx + 3, cy - 1, 14, 3, true);
      if (i == 4)
        rect(cx + 8, cy - 6, 3, 13, true);
    }
  }
  uint8_t columns[] = {0, 0, 1, 63};
  command(0x2a, columns, 4);
  for (int y = 0; y < 240; y += 20) {
    uint8_t rows[] = {0, static_cast<uint8_t>(y), 0, static_cast<uint8_t>(y + 19)};
    command(0x2b, rows, 4);
    command(0x2c);
    for (int j = 0; j < 20; ++j)
      for (int x = 0; x < 320; ++x) {
        uint16_t color = (bitmap[y + j][x / 8] & (0x80 >> (x % 8)))
                             ? ((y + j < 26 || y + j >= 187) ? 0x07ff : 0xffff)
                             : 0;
        stripe[j * 320 + x] = __builtin_bswap16(color);
      }
    tx(stripe, 320 * 20 * 2, true);
  }
}
void pollTouch() {
  static bool held = false;
  static int64_t releasedAt = 0;
  static int64_t lastRejectedLog = 0;
  const int64_t now = esp_timer_get_time();
  if (gpio_get_level(GPIO_NUM_36)) {
    if (held && !releasedAt)
      releasedAt = now;
    if (held && releasedAt && now - releasedAt >= 30000) {
      held = false;
      ESP_LOGI("touch", "RELEASE");
    }
    return;
  }
  releasedAt = 0;
  if (held)
    return;
  // Same five-sample median and calibration as the reference XPT2046 driver.
  uint16_t xs[5], ys[5];
  for (int i = 0; i < 5; ++i) {
    xs[i] = touchRead(0xd0);
    ys[i] = touchRead(0x90);
  }
  std::sort(std::begin(xs), std::end(xs));
  std::sort(std::begin(ys), std::end(ys));
  const int rawX = xs[2], rawY = ys[2];
  const bool released = gpio_get_level(GPIO_NUM_36);
  if (released ||
      rawX < std::min(CONFIG_MUSIC_TOUCH_X_MIN, CONFIG_MUSIC_TOUCH_X_MAX) ||
      rawX > std::max(CONFIG_MUSIC_TOUCH_X_MIN, CONFIG_MUSIC_TOUCH_X_MAX) ||
      rawY < std::min(CONFIG_MUSIC_TOUCH_Y_MIN, CONFIG_MUSIC_TOUCH_Y_MAX) ||
      rawY > std::max(CONFIG_MUSIC_TOUCH_Y_MIN, CONFIG_MUSIC_TOUCH_Y_MAX)) {
    // Keep bad calibration visible without flooding Serial while a finger is held.
    if (!lastRejectedLog || now - lastRejectedLog >= 500000) {
      ESP_LOGW("touch", "REJECT raw=(%d,%d) spread=(%d,%d) reason=%s calX=(%d,%d) calY=(%d,%d)",
               rawX, rawY, xs[4] - xs[0], ys[4] - ys[0],
               released ? "released-during-read" : "outside-calibration",
               CONFIG_MUSIC_TOUCH_X_MIN, CONFIG_MUSIC_TOUCH_X_MAX,
               CONFIG_MUSIC_TOUCH_Y_MIN, CONFIG_MUSIC_TOUCH_Y_MAX);
      lastRejectedLog = now;
    }
    return;
  }
  // Board measurements: raw Y increases left-to-right, raw X top-to-bottom.
  const int x = coordinate(rawY, CONFIG_MUSIC_TOUCH_Y_MIN, CONFIG_MUSIC_TOUCH_Y_MAX, 319);
  const int y = coordinate(rawX, CONFIG_MUSIC_TOUCH_X_MIN, CONFIG_MUSIC_TOUCH_X_MAX, 239);
  held = true; // One command per press; holding PLAY must not toggle repeatedly.
  const bool hit = y >= 200 && y < 238 && x % 64 >= 2 && x % 64 < 62;
  static const char *const names[] = {"PRE", "PAUSE/START", "NEXT", "VOL-", "VOL+"};
  ESP_LOGI("touch", "PRESS raw=(%d,%d) point=(%d,%d) spread=(%d,%d) button=%s",
           rawX, rawY, x, y, xs[4] - xs[0], ys[4] - ys[0], hit ? names[x / 64] : "NONE");
  if (hit) {
    Command c = static_cast<Command>(x / 64);
    if (xQueueSend(commands, &c, 0) != pdTRUE)
      ESP_LOGW("touch", "Command %s dropped: queue full", names[x / 64]);
  }
}
