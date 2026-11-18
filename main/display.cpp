#include "config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "font.h"
#include "player.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
spi_device_handle_t lcd;
uint16_t *stripe;
int stripeY0 = 0;

constexpr uint16_t rgb565(uint32_t hex) {
  const uint16_t r = (hex >> 19) & 0x1f;
  const uint16_t g = (hex >> 10) & 0x3f;
  const uint16_t b = (hex >> 3) & 0x1f;
  return (r << 11) | (g << 5) | b;
}

constexpr uint16_t C_BG = rgb565(0x070b14);
constexpr uint16_t C_CARD = rgb565(0x0f1626);
constexpr uint16_t C_LINE = rgb565(0x1c2740);
constexpr uint16_t C_TEXT = rgb565(0xe8edf7);
constexpr uint16_t C_DIM = rgb565(0x7d889f);
constexpr uint16_t C_CYAN = rgb565(0x22d3ee);
constexpr uint16_t C_VIOLET = rgb565(0xa78bfa);
constexpr uint16_t C_AMBER = rgb565(0xe6b450);
constexpr uint16_t C_ORANGE = rgb565(0xf97316);
constexpr uint16_t C_GREEN = rgb565(0x4ade80);
constexpr uint16_t C_RED = rgb565(0xe24b4a);
constexpr uint16_t C_BLUE = rgb565(0x7aa2f7);
constexpr uint16_t C_TEAL = rgb565(0x5dcaa5);
constexpr uint16_t C_GRAY = rgb565(0x5b6475);
constexpr uint16_t C_HINT = rgb565(0x3a4560);
constexpr uint16_t C_COVER = rgb565(0x231a38);

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

inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha) {
  if (alpha == 255)
    return fg;
  if (alpha == 0)
    return bg;
  const uint32_t inv = 255 - alpha;
  const uint32_t r = (((bg >> 11) & 31) * inv + ((fg >> 11) & 31) * alpha + 127) / 255;
  const uint32_t g = (((bg >> 5) & 63) * inv + ((fg >> 5) & 63) * alpha + 127) / 255;
  const uint32_t b = ((bg & 31) * inv + (fg & 31) * alpha + 127) / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

inline void putPixel(int x, int y, uint16_t color, uint8_t alpha = 255) {
  if (x < 0 || x >= 320 || y < stripeY0 || y >= stripeY0 + 20 || alpha == 0)
    return;
  uint16_t &dst = stripe[(y - stripeY0) * 320 + x];
  dst = (alpha == 255) ? color : blend565(dst, color, alpha);
}

inline uint8_t coverageAlpha(float dist) {
  // Smooth 1px anti-aliased edge transition around dist = 0.
  if (dist <= -0.5f)
    return 255;
  if (dist >= 0.5f)
    return 0;
  return static_cast<uint8_t>((0.5f - dist) * 255.0f);
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
  const int y0 = std::max(stripeY0, y);
  const int y1 = std::min(stripeY0 + 20, y + h);
  if (y0 >= y1)
    return;
  const int x0 = std::max(0, x);
  const int x1 = std::min(320, x + w);
  for (int py = y0; py < y1; ++py) {
    uint16_t *row = &stripe[(py - stripeY0) * 320];
    for (int px = x0; px < x1; ++px)
      row[px] = color;
  }
}

void fillRoundRectAA(int x, int y, int w, int h, int radius, uint16_t color) {
  if (w <= 0 || h <= 0)
    return;
  const int y0 = std::max(stripeY0, y);
  const int y1 = std::min(stripeY0 + 20, y + h);
  if (y0 >= y1)
    return;
  const int x0 = std::max(0, x);
  const int x1 = std::min(320, x + w);
  const int r = std::clamp(radius, 0, std::min(w, h) / 2);
  if (r == 0) {
    fillRect(x, y, w, h, color);
    return;
  }
  const float rf = float(r);
  const int lx = x + r;
  const int rx = x + w - 1 - r;
  const int ty = y + r;
  const int by = y + h - 1 - r;
  for (int py = y0; py < y1; ++py) {
    const int dy = (py < ty) ? (ty - py) : (py > by ? (py - by) : 0);
    if (dy == 0) {
      uint16_t *row = &stripe[(py - stripeY0) * 320];
      for (int px = x0; px < x1; ++px)
        row[px] = color;
      continue;
    }
    for (int px = x0; px < x1; ++px) {
      const int dx = (px < lx) ? (lx - px) : (px > rx ? (px - rx) : 0);
      if (dx == 0) {
        putPixel(px, py, color);
      } else {
        const float dist = std::sqrt(float(dx * dx + dy * dy)) - rf;
        putPixel(px, py, color, coverageAlpha(dist));
      }
    }
  }
}

void drawCardAA(int x, int y, int w, int h, int radius, uint16_t fill, uint16_t border) {
  if (y + h <= stripeY0 || y >= stripeY0 + 20)
    return;
  fillRoundRectAA(x, y, w, h, radius, border);
  if (w > 2 && h > 2)
    fillRoundRectAA(x + 1, y + 1, w - 2, h - 2, std::max(0, radius - 1), fill);
}

void fillCircleAA(float cx, float cy, float r, uint16_t color) {
  const int y0 = std::max(stripeY0, int(std::floor(cy - r - 1.0f)));
  const int y1 = std::min(stripeY0 + 20, int(std::ceil(cy + r + 1.0f)) + 1);
  if (y0 >= y1)
    return;
  const int x0 = std::max(0, int(std::floor(cx - r - 1.0f)));
  const int x1 = std::min(320, int(std::ceil(cx + r + 1.0f)) + 1);
  for (int py = y0; py < y1; ++py) {
    const float dy = float(py) - cy;
    for (int px = x0; px < x1; ++px) {
      const float dx = float(px) - cx;
      const float dist = std::sqrt(dx * dx + dy * dy) - r;
      putPixel(px, py, color, coverageAlpha(dist));
    }
  }
}

void drawRingArcAA(float cx, float cy, float rOuter, float rInner, int pct, uint16_t fg, uint16_t bg) {
  const int y0 = std::max(stripeY0, int(std::floor(cy - rOuter - 1.0f)));
  const int y1 = std::min(stripeY0 + 20, int(std::ceil(cy + rOuter + 1.0f)) + 1);
  if (y0 >= y1)
    return;
  const int x0 = std::max(0, int(std::floor(cx - rOuter - 1.0f)));
  const int x1 = std::min(320, int(std::ceil(cx + rOuter + 1.0f)) + 1);
  const float maxAngle = float(std::clamp(pct, 0, 100)) * 3.6f;
  constexpr float kRadToDeg = 57.2957795f;
  for (int py = y0; py < y1; ++py) {
    const float dy = float(py) - cy;
    for (int px = x0; px < x1; ++px) {
      const float dx = float(px) - cx;
      const float d = std::sqrt(dx * dx + dy * dy);
      const uint8_t aOut = coverageAlpha(d - rOuter);
      if (aOut == 0)
        continue;
      const uint8_t aIn = coverageAlpha(rInner - d);
      const uint8_t ringAlpha = uint8_t(aOut * aIn / 255);
      if (ringAlpha == 0)
        continue;
      float deg = std::atan2(dx, -dy) * kRadToDeg;
      if (deg < 0.0f)
        deg += 360.0f;
      uint16_t c = bg;
      if (pct >= 100) {
        c = fg;
      } else if (pct > 0) {
        if (deg <= maxAngle - 1.5f)
          c = fg;
        else if (deg < maxAngle + 1.5f)
          c = blend565(bg, fg, uint8_t((maxAngle + 1.5f - deg) / 3.0f * 255.0f));
      }
      putPixel(px, py, c, ringAlpha);
    }
  }
}

void drawLineAA(float x0, float y0, float x1, float y1, uint16_t color, float halfWidth = 0.75f) {
  const int minY = std::max(stripeY0, int(std::floor(std::min(y0, y1) - halfWidth - 1.0f)));
  const int maxY = std::min(stripeY0 + 19, int(std::ceil(std::max(y0, y1) + halfWidth + 1.0f)));
  if (minY > maxY)
    return;
  const int minX = std::max(0, int(std::floor(std::min(x0, x1) - halfWidth - 1.0f)));
  const int maxX = std::min(319, int(std::ceil(std::max(x0, x1) + halfWidth + 1.0f)));
  const float vx = x1 - x0, vy = y1 - y0;
  const float len2 = vx * vx + vy * vy;
  for (int py = minY; py <= maxY; ++py) {
    for (int px = minX; px <= maxX; ++px) {
      float t = len2 > 0.0001f ? ((float(px) - x0) * vx + (float(py) - y0) * vy) / len2 : 0.0f;
      t = std::clamp(t, 0.0f, 1.0f);
      const float dx = float(px) - (x0 + t * vx);
      const float dy = float(py) - (y0 + t * vy);
      const float dist = std::sqrt(dx * dx + dy * dy) - halfWidth;
      putPixel(px, py, color, coverageAlpha(dist));
    }
  }
}

void drawArcSpanAA(float cx, float cy, float r, float halfWidth, float degStart, float degEnd, uint16_t color) {
  const float rMax = r + halfWidth + 1.0f;
  const int y0 = std::max(stripeY0, int(std::floor(cy - rMax)));
  const int y1 = std::min(stripeY0 + 20, int(std::ceil(cy + rMax)) + 1);
  if (y0 >= y1)
    return;
  const int x0 = std::max(0, int(std::floor(cx - rMax)));
  const int x1 = std::min(320, int(std::ceil(cx + rMax)) + 1);
  const bool full = (degEnd - degStart >= 359.5f);
  constexpr float kDegToRad = 0.0174532925f;
  constexpr float kRadToDeg = 57.2957795f;
  const float sx = cx + r * std::sin(degStart * kDegToRad);
  const float sy = cy - r * std::cos(degStart * kDegToRad);
  const float ex = cx + r * std::sin(degEnd * kDegToRad);
  const float ey = cy - r * std::cos(degEnd * kDegToRad);
  for (int py = y0; py < y1; ++py) {
    const float dy = float(py) - cy;
    for (int px = x0; px < x1; ++px) {
      const float dx = float(px) - cx;
      const float d = std::sqrt(dx * dx + dy * dy);
      bool inside = full;
      if (!full) {
        float deg = std::atan2(dx, -dy) * kRadToDeg;
        if (deg < 0.0f)
          deg += 360.0f;
        inside = (degStart <= degEnd) ? (deg >= degStart && deg <= degEnd) : (deg >= degStart || deg <= degEnd);
      }
      float dist;
      if (inside) {
        dist = std::abs(d - r) - halfWidth;
      } else {
        const float d1 = std::sqrt((float(px) - sx) * (float(px) - sx) + (float(py) - sy) * (float(py) - sy));
        const float d2 = std::sqrt((float(px) - ex) * (float(px) - ex) + (float(py) - ey) * (float(py) - ey));
        dist = std::min(d1, d2) - halfWidth;
      }
      putPixel(px, py, color, coverageAlpha(dist));
    }
  }
}

float bigGlyphWidth(char ch, float h) {
  if (ch == ':')
    return h * 0.24f;
  if (ch == 0xB0 || ch == '*')
    return h * 0.34f;
  return h * 0.56f;
}

void drawBigGlyph(char ch, float x, float y, float h, float stroke, uint16_t color) {
  const float w = bigGlyphWidth(ch, h);
  const float hw = stroke * 0.5f;
  const float x0 = x + hw, x1 = x + w - hw, xm = x + w * 0.5f;
  const float y0 = y + hw, y1 = y + h - hw, ym = y + h * 0.5f;
  const float rx = (w - stroke) * 0.5f;
  switch (ch) {
  case '0':
    drawArcSpanAA(xm, y0 + rx, rx, hw, 270.0f, 90.0f, color);
    drawLineAA(x0, y0 + rx, x0, y1 - rx, color, hw);
    drawLineAA(x1, y0 + rx, x1, y1 - rx, color, hw);
    drawArcSpanAA(xm, y1 - rx, rx, hw, 90.0f, 270.0f, color);
    break;
  case '1':
    drawLineAA(xm + w * 0.06f, y0, xm + w * 0.06f, y1, color, hw);
    drawLineAA(xm + w * 0.06f, y0, xm - rx * 0.6f, y0 + h * 0.16f, color, hw);
    drawLineAA(xm - rx * 0.65f, y1, xm + rx * 0.75f, y1, color, hw);
    break;
  case '2':
    drawArcSpanAA(xm, y0 + rx, rx, hw, 250.0f, 115.0f, color);
    drawLineAA(x + w * 0.88f, y0 + rx * 1.35f, x0, y1, color, hw);
    drawLineAA(x0, y1, x1, y1, color, hw);
    break;
  case '3':
    drawArcSpanAA(xm, y0 + rx * 0.92f, rx * 0.92f, hw, 255.0f, 180.0f, color);
    drawArcSpanAA(xm, y1 - rx * 0.92f, rx * 0.92f, hw, 0.0f, 285.0f, color);
    break;
  case '4':
    drawLineAA(x1 - w * 0.18f, y0, x0, ym + h * 0.08f, color, hw);
    drawLineAA(x0, ym + h * 0.08f, x1, ym + h * 0.08f, color, hw);
    drawLineAA(x1 - w * 0.18f, y0, x1 - w * 0.18f, y1, color, hw);
    break;
  case '5': {
    const float yMid = ym - h * 0.06f;
    const float r5 = (y1 - yMid) * 0.5f;
    drawLineAA(x1 - w * 0.05f, y0, x0 + w * 0.05f, y0, color, hw);
    drawLineAA(x0 + w * 0.05f, y0, x0 + w * 0.03f, yMid, color, hw);
    drawLineAA(x0 + w * 0.03f, yMid, xm, yMid, color, hw);
    drawArcSpanAA(xm, yMid + r5, r5, hw, 0.0f, 230.0f, color);
    break;
  }
  case '6':
    drawArcSpanAA(xm, y1 - rx, rx, hw, 0.0f, 360.0f, color);
    drawLineAA(x0, y0 + rx, x0, y1 - rx, color, hw);
    drawArcSpanAA(xm, y0 + rx, rx, hw, 270.0f, 45.0f, color);
    break;
  case '7':
    drawLineAA(x0, y0, x1, y0, color, hw);
    drawLineAA(x1, y0, x0 + w * 0.30f, y1, color, hw);
    break;
  case '8': {
    const float rTop = (ym - y0) * 0.52f;
    const float rBot = (y1 - ym) * 0.52f;
    drawArcSpanAA(xm, y0 + rTop, rTop, hw, 0.0f, 360.0f, color);
    drawArcSpanAA(xm, y1 - rBot, rBot, hw, 0.0f, 360.0f, color);
    break;
  }
  case '9':
    drawArcSpanAA(xm, y0 + rx, rx, hw, 0.0f, 360.0f, color);
    drawLineAA(x1, y0 + rx, x1, y1 - rx, color, hw);
    drawArcSpanAA(xm, y1 - rx, rx, hw, 90.0f, 225.0f, color);
    break;
  case ':':
    fillCircleAA(xm, y + h * 0.34f, hw * 1.15f, color);
    fillCircleAA(xm, y + h * 0.66f, hw * 1.15f, color);
    break;
  case '-':
    drawLineAA(x0, ym, x1, ym, color, hw);
    break;
  case 'C':
    drawArcSpanAA(xm + w * 0.06f, ym, rx * 1.02f, hw, 45.0f, 315.0f, color);
    break;
  default:
    drawArcSpanAA(xm, y0 + rx * 0.9f, rx * 0.8f, hw * 0.65f, 0.0f, 360.0f, color);
    break;
  }
}

void drawBigText(const char *s, float x, float y, float h, float stroke, uint16_t color) {
  const float gap = h * 0.12f;
  while (*s) {
    uint8_t c = uint8_t(*s);
    if (c == 0xc2 && uint8_t(s[1]) == 0xb0) {
      drawBigGlyph('*', x, y, h, stroke, color);
      x += bigGlyphWidth('*', h) + gap * 0.5f;
      s += 2;
      continue;
    }
    drawBigGlyph(*s, x, y, h, stroke, color);
    x += bigGlyphWidth(*s, h) + gap;
    ++s;
  }
}

float measureBigText(const char *s, float h) {
  const float gap = h * 0.12f;
  float total = 0.0f;
  while (*s) {
    uint8_t c = uint8_t(*s);
    if (c == 0xc2 && uint8_t(s[1]) == 0xb0) {
      total += bigGlyphWidth('*', h) + gap * 0.5f;
      s += 2;
      continue;
    }
    total += bigGlyphWidth(*s, h) + gap;
    ++s;
  }
  return total > gap ? total - gap : 0.0f;
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

const Glyph *findGlyph(uint32_t code) {
  auto glyph = std::lower_bound(std::begin(font), std::end(font), code,
                                [](const Glyph &g, uint32_t c) { return g.code < c; });
  if (glyph == std::end(font) || glyph->code != code)
    return &font['?' - 32];
  return glyph;
}

int measureText(const char *s, float scale = 1.0f) {
  float w = 0.0f;
  while (*s) {
    uint32_t code = nextCode(s);
    w += float(findGlyph(code)->advance) * scale;
  }
  return int(std::ceil(w));
}

inline float sampleGlyph(const Glyph *g, int x, int y) {
  if (x < 0 || x >= 16 || y < 0 || y >= 20)
    return 0.0f;
  return (g->rows[y] & (0x8000 >> x)) ? 1.0f : 0.0f;
}

void drawText(const char *s, int x, int y, int width, uint16_t color, int lines = 1, float scale = 1.0f) {
  int left = x, row = 0;
  float fxPos = float(x);
  const int lineH = int(std::round(20.0f * scale));
  while (*s && row < lines) {
    uint32_t code = nextCode(s);
    const Glyph *glyph = findGlyph(code);
    const float adv = float(glyph->advance) * scale;
    if (fxPos + adv > float(left + width) && fxPos > float(left)) {
      fxPos = float(left);
      y += lineH;
      if (++row >= lines)
        break;
    }
    const int drawX = int(std::round(fxPos));
    const int gy0 = std::max(stripeY0, y);
    const int gy1 = std::min(stripeY0 + 20, y + lineH);
    if (gy0 < gy1) {
      if (std::abs(scale - 1.0f) < 0.01f) {
        for (int py = gy0; py < gy1; ++py) {
          const uint16_t rowBits = glyph->rows[py - y];
          const int maxI = std::min(16, left + width - drawX);
          for (int i = 0; i < maxI; ++i)
            if (rowBits & (0x8000 >> i))
              putPixel(drawX + i, py, color);
        }
      } else {
        const int gw = std::min(int(std::ceil(16.0f * scale)), left + width - drawX);
        const float invScale = 1.0f / scale;
        for (int py = gy0; py < gy1; ++py) {
          const float fy = (float(py - y) + 0.5f) * invScale - 0.5f;
          const int iy = int(std::floor(fy));
          const float ty = fy - float(iy);
          for (int px = 0; px < gw; ++px) {
            const float fx = (float(px) + 0.5f) * invScale - 0.5f;
            const int ix = int(std::floor(fx));
            const float tx = fx - float(ix);
            const float c00 = sampleGlyph(glyph, ix, iy);
            const float c10 = sampleGlyph(glyph, ix + 1, iy);
            const float c01 = sampleGlyph(glyph, ix, iy + 1);
            const float c11 = sampleGlyph(glyph, ix + 1, iy + 1);
            float cov = (c00 * (1.0f - tx) + c10 * tx) * (1.0f - ty) +
                        (c01 * (1.0f - tx) + c11 * tx) * ty;
            cov = std::clamp(cov * 1.65f, 0.0f, 1.0f);
            if (cov > 0.02f)
              putPixel(drawX + px, py, color, static_cast<uint8_t>(cov * 255.0f));
          }
        }
      }
    }
    fxPos += adv;
  }
}

void drawTextCentered(const char *s, int cx, int y, int maxW, uint16_t color, float scale = 1.0f) {
  const int w = std::min(maxW, measureText(s, scale));
  drawText(s, cx - w / 2, y, maxW, color, 1, scale);
}

void drawTextRight(const char *s, int rightX, int y, uint16_t color, float scale = 1.0f) {
  const int w = measureText(s, scale);
  drawText(s, rightX - w, y, w + 2, color, 1, scale);
}

// --- Anti-Aliased Vector Icons ---
void drawWifiIconAA(float cx, float cy, uint16_t color) {
  fillCircleAA(cx, cy + 5.0f, 1.5f, color);
  const int y0 = std::max(stripeY0, int(cy - 6));
  const int y1 = std::min(stripeY0 + 20, int(cy + 7));
  if (y0 >= y1)
    return;
  for (int py = y0; py < y1; ++py) {
    const float dy = float(py) - (cy + 5.0f);
    for (int px = int(cx - 11); px <= int(cx + 11); ++px) {
      const float dx = float(px) - cx;
      if (std::abs(dx) > -dy + 1.5f)
        continue;
      const float d = std::sqrt(dx * dx + dy * dy);
      for (float r : {5.0f, 9.0f}) {
        const float dist = std::abs(d - r) - 0.75f;
        if (dist < 0.5f)
          putPixel(px, py, color, coverageAlpha(dist));
      }
    }
  }
}

void drawBackChevronAA(float x, float y, uint16_t color) {
  drawLineAA(x + 6.0f, y, x, y + 6.0f, color, 0.9f);
  drawLineAA(x, y + 6.0f, x + 6.0f, y + 12.0f, color, 0.9f);
}

void drawRightChevronAA(float x, float y, uint16_t color) {
  drawLineAA(x, y, x + 5.0f, y + 5.0f, color, 0.85f);
  drawLineAA(x + 5.0f, y + 5.0f, x, y + 10.0f, color, 0.85f);
}

void drawGearIconAA(float cx, float cy, uint16_t color) {
  for (int a = 0; a < 8; ++a) {
    const float rad = float(a) * 0.785398f;
    drawLineAA(cx + std::cos(rad) * 7.0f, cy + std::sin(rad) * 7.0f,
               cx + std::cos(rad) * 11.5f, cy + std::sin(rad) * 11.5f, color, 1.8f);
  }
  drawRingArcAA(cx, cy, 8.8f, 4.2f, 100, color, C_GRAY);
}

void drawNoteIconAA(float cx, float cy, uint16_t color, float scale = 1.0f) {
  fillCircleAA(cx - 4.5f * scale, cy + 6.0f * scale, 4.0f * scale, color);
  fillCircleAA(cx + 6.0f * scale, cy + 4.0f * scale, 4.0f * scale, color);
  drawLineAA(cx - 1.5f * scale, cy + 5.0f * scale, cx - 1.5f * scale, cy - 10.0f * scale, color, 1.1f * scale);
  drawLineAA(cx + 9.0f * scale, cy + 3.0f * scale, cx + 9.0f * scale, cy - 12.0f * scale, color, 1.1f * scale);
  drawLineAA(cx - 1.5f * scale, cy - 10.0f * scale, cx + 9.0f * scale, cy - 12.0f * scale, color, 1.6f * scale);
}

void drawGlobeIconAA(float cx, float cy, uint16_t color) {
  drawRingArcAA(cx, cy, 8.5f, 7.0f, 100, color, C_BG);
  drawLineAA(cx - 8.0f, cy, cx + 8.0f, cy, color, 0.65f);
  drawLineAA(cx, cy - 8.0f, cx, cy + 8.0f, color, 0.65f);
}

void drawMonitorIconAA(float cx, float cy, uint16_t color) {
  fillRoundRectAA(int(cx - 9), int(cy - 7), 18, 12, 2, color);
  fillRect(int(cx - 7), int(cy - 5), 14, 8, C_BG);
  fillRect(int(cx - 2), int(cy + 5), 4, 3, color);
  drawLineAA(cx - 5.0f, cy + 8.5f, cx + 5.0f, cy + 8.5f, color, 0.9f);
}

void drawWeatherIconAA(float cx, float cy, int code) {
  if (code == 0 || code == 1) {
    fillCircleAA(cx, cy, 8.5f, C_AMBER);
    for (int a = 0; a < 8; ++a) {
      const float rad = float(a) * 0.785398f;
      drawLineAA(cx + std::cos(rad) * 11.0f, cy + std::sin(rad) * 11.0f,
                 cx + std::cos(rad) * 14.0f, cy + std::sin(rad) * 14.0f, C_AMBER, 0.9f);
    }
  } else {
    if (code <= 3)
      fillCircleAA(cx + 8.0f, cy - 6.0f, 5.5f, C_AMBER);
    fillCircleAA(cx - 6.0f, cy + 2.0f, 6.5f, C_BLUE);
    fillCircleAA(cx + 2.0f, cy - 2.0f, 8.5f, C_BLUE);
    fillCircleAA(cx + 9.0f, cy + 3.0f, 5.5f, C_BLUE);
    fillRect(int(cx - 6), int(cy + 2), 16, 7, C_BLUE);
    if (code >= 51) {
      for (int rx : {-7, 0, 7})
        drawLineAA(cx + float(rx), cy + 11.0f, cx + float(rx) - 2.0f, cy + 16.0f, C_CYAN, 0.9f);
    }
  }
}

void drawPlayTriangleAA(float cx, float cy, float w, float h, uint16_t color, bool right) {
  const int y0 = std::max(stripeY0, int(std::floor(cy - h - 1.0f)));
  const int y1 = std::min(stripeY0 + 20, int(std::ceil(cy + h + 1.0f)) + 1);
  if (y0 >= y1)
    return;
  const int x0 = std::max(0, int(std::floor(cx - w - 1.0f)));
  const int x1 = std::min(320, int(std::ceil(cx + w + 1.0f)) + 1);
  for (int py = y0; py < y1; ++py) {
    const float dy = std::abs(float(py) - cy);
    for (int px = x0; px < x1; ++px) {
      const float fx = right ? (float(px) - (cx - w * 0.5f)) : ((cx + w * 0.5f) - float(px));
      if (fx < -0.5f || fx > w + 0.5f)
        continue;
      const float maxHalf = (1.0f - std::clamp(fx / w, 0.0f, 1.0f)) * h;
      const float dEdge = dy - maxHalf;
      const float dLeft = -fx;
      const float dist = std::max(dEdge, dLeft);
      putPixel(px, py, color, coverageAlpha(dist));
    }
  }
}

void drawSwipeHint() { fillRoundRectAA(132, 232, 56, 3, 2, C_HINT); }

// --- Screen Renderers ---
void drawStandbyClockWeather(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  time_t now = time(nullptr);
  struct tm tmNow {};
  localtime_r(&now, &tmNow);
  const bool timeValid = tmNow.tm_year + 1900 >= 2024;
  const int hh = timeValid ? tmNow.tm_hour : (v.uiTick / 3600) % 24;
  const int mm = timeValid ? tmNow.tm_min : (v.uiTick / 60) % 60;
  const int ss = timeValid ? tmNow.tm_sec : (v.uiTick % 60);
  const int dd = timeValid ? tmNow.tm_mday : 17;
  const int mo = timeValid ? tmNow.tm_mon + 1 : 11;
  const int yy = timeValid ? tmNow.tm_year + 1900 : 2026;
  const int wday = timeValid ? tmNow.tm_wday : 2;
  const LunarDate lunar = solarToLunar(dd, mo, yy);
  const WeatherInfo w = servicesGetWeather();

  static const char *const wdayVi[] = {"Chủ nhật", "Thứ hai", "Thứ ba", "Thứ tư",
                                       "Thứ năm", "Thứ sáu", "Thứ bảy"};
  static const char *const wdayEn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const can[] = {"Canh", "Tân", "Nhâm", "Quý", "Giáp",
                                    "Ất", "Bính", "Đinh", "Mậu", "Kỷ"};
  static const char *const chi[] = {"Thân", "Dậu", "Tuất", "Hợi", "Tý", "Sửu",
                                    "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi"};

  drawText(vi ? "Hà Nội" : "Hanoi", 12, 6, 120, C_DIM);
  drawWifiIconAA(298.0f, 11.0f, v.wifiConnected ? C_GREEN : C_DIM);

  char timeStr[8], secStr[8], dateStr[40], lunarStr[48], tempStr[12];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hh, mm);
  snprintf(secStr, sizeof(secStr), "%02d", ss);
  drawBigText(timeStr, 14.0f, 38.0f, 42.0f, 4.6f, C_TEXT);
  drawBigText(secStr, 148.0f, 60.0f, 20.0f, 2.4f, C_DIM);

  snprintf(dateStr, sizeof(dateStr), "%s, %02d/%02d/%04d",
           vi ? wdayVi[wday % 7] : wdayEn[wday % 7], dd, mo, yy);
  if (vi) {
    snprintf(lunarStr, sizeof(lunarStr), "Âm lịch %d/%d%s · %s %s", lunar.day, lunar.month,
             lunar.leap ? "*" : "", can[lunar.year % 10], chi[lunar.year % 12]);
  } else {
    snprintf(lunarStr, sizeof(lunarStr), "Lunar %d/%d%s", lunar.day, lunar.month,
             lunar.leap ? "*" : "");
  }
  drawText(dateStr, 14, 102, 178, C_TEXT, 1, 1.2f);
  drawText(lunarStr, 14, 130, 178, C_AMBER, 1, 1.15f);

  drawCardAA(196, 34, 112, 124, 8, C_CARD, C_LINE);
  drawWeatherIconAA(252.0f, 56.0f, w.code);
  snprintf(tempStr, sizeof(tempStr), "%d°C", w.tempC);
  const float tw = measureBigText(tempStr, 22.0f);
  drawBigText(tempStr, 252.0f - tw * 0.5f, 82.0f, 22.0f, 2.6f, C_TEXT);
  drawTextCentered(vi ? w.descVi : w.descEn, 252, 116, 102, C_DIM);
  drawTextCentered(w.valid ? "Open-Meteo" : (vi ? "Ngoại tuyến" : "Offline"), 252, 136, 102, C_DIM);

  drawTextCentered(vi ? "Vuốt lên để mở Menu" : "Swipe up for Menu", 160, 198, 220, C_DIM);
  drawSwipeHint();
}

uint8_t cpuHist[60] = {};
uint8_t ramHist[60] = {};
uint32_t lastHistTick = UINT32_MAX;

void drawStandbyPcStats(const View &v) {
  const uint32_t t = v.uiTick;
  const int cpu = 22 + int((t * 7 + 13) % 52);
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t totalHeap = 320 * 1024;
  const int ramUsedPct = std::clamp(int(100 - (freeHeap * 100 / totalHeap)), 10, 95);
  const int flashPct = 34;
  const int tempC = 44 + int((t * 3) % 10);

  int rssiDbm = -75;
  int rssiPct = 0;
  wifi_ap_record_t apInfo{};
  if (v.wifiConnected && esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
    rssiDbm = apInfo.rssi;
    rssiPct = std::clamp((rssiDbm + 100) * 2, 5, 100);
  }

  if (t != lastHistTick) {
    lastHistTick = t;
    memmove(cpuHist, cpuHist + 1, 59);
    memmove(ramHist, ramHist + 1, 59);
    cpuHist[59] = static_cast<uint8_t>(cpu);
    ramHist[59] = static_cast<uint8_t>(ramUsedPct);
  }

  time_t now = time(nullptr);
  struct tm tmNow {};
  localtime_r(&now, &tmNow);
  const bool timeValid = tmNow.tm_year + 1900 >= 2024;
  char clockStr[16];
  snprintf(clockStr, sizeof(clockStr), "%02d:%02d:%02d",
           timeValid ? tmNow.tm_hour : int((t / 3600) % 24),
           timeValid ? tmNow.tm_min : int((t / 60) % 60),
           timeValid ? tmNow.tm_sec : int(t % 60));

  drawText("SYS://MONITOR", 12, 4, 150, C_CYAN);
  drawTextRight(clockStr, 308, 4, C_CYAN);
  fillRect(0, 28, 320, 1, C_LINE);

  drawCardAA(8, 36, 96, 104, 8, C_CARD, C_LINE);
  drawRingArcAA(56.0f, 76.0f, 33.0f, 26.5f, cpu, C_CYAN, C_LINE);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d%%", cpu);
  drawTextCentered(buf, 56, 67, 50, C_CYAN);
  drawTextCentered("CPU 240MHz", 56, 118, 90, C_DIM);

  drawCardAA(112, 36, 96, 104, 8, C_CARD, C_LINE);
  drawRingArcAA(160.0f, 76.0f, 33.0f, 26.5f, ramUsedPct, C_VIOLET, C_LINE);
  snprintf(buf, sizeof(buf), "%d%%", ramUsedPct);
  drawTextCentered(buf, 160, 67, 50, C_VIOLET);
  snprintf(buf, sizeof(buf), "FREE %luK", (unsigned long)(freeHeap / 1024));
  drawTextCentered(buf, 160, 118, 90, C_DIM);

  drawText("FLASH", 216, 36, 50, C_DIM);
  snprintf(buf, sizeof(buf), "%d%%", flashPct);
  drawTextRight(buf, 312, 36, C_AMBER);
  fillRoundRectAA(216, 56, 96, 5, 2, C_LINE);
  fillRoundRectAA(216, 56, 96 * flashPct / 100, 5, 2, C_AMBER);

  drawText("TEMP", 216, 70, 50, C_DIM);
  snprintf(buf, sizeof(buf), "%d°C", tempC);
  drawTextRight(buf, 312, 70, C_ORANGE);
  fillRoundRectAA(216, 90, 96, 5, 2, C_LINE);
  fillRoundRectAA(216, 90, 96 * std::clamp(tempC, 0, 100) / 100, 5, 2, C_ORANGE);

  drawText("RSSI", 216, 104, 46, C_DIM);
  if (v.wifiConnected)
    snprintf(buf, sizeof(buf), "%ddBm", rssiDbm);
  else
    snprintf(buf, sizeof(buf), "OFF");
  drawTextRight(buf, 312, 104, v.wifiConnected ? C_GREEN : C_DIM);
  fillRoundRectAA(216, 124, 96, 5, 2, C_LINE);
  if (rssiPct > 0)
    fillRoundRectAA(216, 124, 96 * rssiPct / 100, 5, 2, C_GREEN);

  drawCardAA(8, 148, 304, 84, 8, C_CARD, C_LINE);
  for (int gy : {164, 180, 196})
    fillRect(14, gy, 292, 1, C_LINE);
  for (int i = 0; i < 59; ++i) {
    const float x0 = 14.0f + float(i * 292) / 59.0f;
    const float x1 = 14.0f + float((i + 1) * 292) / 59.0f;
    const float cy0 = 208.0f - float(cpuHist[i]) * 52.0f / 100.0f;
    const float cy1 = 208.0f - float(cpuHist[i + 1]) * 52.0f / 100.0f;
    const float ry0 = 208.0f - float(ramHist[i]) * 52.0f / 100.0f;
    const float ry1 = 208.0f - float(ramHist[i + 1]) * 52.0f / 100.0f;
    drawLineAA(x0, ry0, x1, ry1, C_VIOLET, 0.9f);
    drawLineAA(x0, cy0, x1, cy1, C_CYAN, 0.9f);
  }
  const uint32_t up = uint32_t(esp_timer_get_time() / 1000000ULL);
  snprintf(buf, sizeof(buf), "60s · up %02lu:%02lu:%02lu",
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
  drawTextRight(buf, 304, 211, C_DIM);
}

void drawMenu(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  time_t now = time(nullptr);
  struct tm tmNow {};
  localtime_r(&now, &tmNow);
  const bool timeValid = tmNow.tm_year + 1900 >= 2024;
  char clockStr[16];
  snprintf(clockStr, sizeof(clockStr), "%02d:%02d",
           timeValid ? tmNow.tm_hour : int((v.uiTick / 3600) % 24),
           timeValid ? tmNow.tm_min : int((v.uiTick / 60) % 60));

  drawBackChevronAA(12.0f, 10.0f, C_TEXT);
  drawText(clockStr, 28, 6, 80, C_DIM);
  drawWifiIconAA(298.0f, 11.0f, v.wifiConnected ? C_GREEN : C_DIM);

  fillRoundRectAA(30, 46, 56, 56, 14, C_GRAY);
  drawGearIconAA(58.0f, 74.0f, C_TEXT);
  drawTextCentered(vi ? "Cài đặt" : "Settings", 58, 108, 76, C_TEXT);

  fillRoundRectAA(114, 46, 56, 56, 14, C_RED);
  drawNoteIconAA(142.0f, 74.0f, C_TEXT, 1.0f);
  drawTextCentered(vi ? "Nhạc" : "Music", 142, 108, 76, C_TEXT);

  fillCircleAA(150.0f, 217.0f, 3.0f, C_TEXT);
  fillCircleAA(164.0f, 217.0f, 3.0f, C_HINT);
  drawSwipeHint();
}

void drawHeader(const char *title, const char *rightText, uint16_t rightColor = C_DIM) {
  drawBackChevronAA(12.0f, 11.0f, C_TEXT);
  drawText(title, 28, 7, 190, C_TEXT);
  if (rightText && rightText[0])
    drawTextRight(rightText, 308, 7, rightColor);
  fillRect(0, 34, 320, 1, C_LINE);
}

void drawSettings(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  time_t now = time(nullptr);
  struct tm tmNow {};
  localtime_r(&now, &tmNow);
  const bool timeValid = tmNow.tm_year + 1900 >= 2024;
  char clockStr[16];
  snprintf(clockStr, sizeof(clockStr), "%02d:%02d",
           timeValid ? tmNow.tm_hour : int((v.uiTick / 3600) % 24),
           timeValid ? tmNow.tm_min : int((v.uiTick / 60) % 60));

  drawHeader(vi ? "Cài đặt" : "Settings", clockStr);

  drawWifiIconAA(22.0f, 56.0f, C_BLUE);
  drawText("Wi-Fi", 42, 47, 130, C_TEXT);
  drawTextRight(v.wifiConnected ? v.wifiIp : (vi ? "Chưa kết nối" : "Offline"), 288, 47, C_DIM);
  drawRightChevronAA(298.0f, 52.0f, C_DIM);
  fillRect(0, 78, 320, 1, C_LINE);

  drawGlobeIconAA(22.0f, 100.0f, C_TEAL);
  drawText(vi ? "Ngôn ngữ" : "Language", 42, 91, 130, C_TEXT);
  drawTextRight(vi ? "Tiếng Việt" : "English", 288, 91, C_DIM);
  drawRightChevronAA(298.0f, 96.0f, C_DIM);
  fillRect(0, 122, 320, 1, C_LINE);

  drawMonitorIconAA(22.0f, 144.0f, C_AMBER);
  drawText(vi ? "Màn hình chờ" : "Standby Mode", 42, 135, 140, C_TEXT);
  drawTextRight(v.standbyMode == StandbyMode::ClockWeather ? (vi ? "Đồng hồ" : "Clock") : "Dashboard",
                288, 135, C_DIM);
  drawRightChevronAA(298.0f, 140.0f, C_DIM);
  fillRect(0, 166, 320, 1, C_LINE);

  drawSwipeHint();
}

char selectedSsid[33] = "";
char wifiPass[65] = "";
bool kbSymbols = false;
bool kbUpper = false;

void drawWifiSettings(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  bool scanning = false;
  auto aps = servicesGetWifiScanResults(scanning);

  drawHeader("Wi-Fi", nullptr);
  fillRoundRectAA(244, 5, 64, 24, 6, C_BLUE);
  drawTextCentered(scanning ? "..." : (vi ? "Quét" : "Scan"), 276, 7, 58, C_BG);

  char statusBuf[64];
  if (scanning)
    snprintf(statusBuf, sizeof(statusBuf), "%s", vi ? "Đang quét mạng Wi-Fi..." : "Scanning Wi-Fi...");
  else if (v.wifiConnected)
    snprintf(statusBuf, sizeof(statusBuf), "%s (%s)", vi ? "Đã kết nối" : "Connected", v.wifiIp);
  else
    snprintf(statusBuf, sizeof(statusBuf), "%s", vi ? "Chưa kết nối - Chọn mạng hoặc Quét" : "Disconnected - Select or Scan");
  drawText(statusBuf, 14, 40, 292, scanning ? C_CYAN : (v.wifiConnected ? C_GREEN : C_DIM));

  if (!scanning && aps.empty()) {
    drawTextCentered(vi ? "Không tìm thấy mạng nào" : "No networks found", 160, 120, 260, C_DIM);
    return;
  }

  const int maxRows = std::min<int>(4, aps.size());
  for (int i = 0; i < maxRows; ++i) {
    const int y = 66 + i * 42;
    drawCardAA(12, y, 296, 36, 8, C_CARD, C_LINE);
    drawText(aps[i].ssid, 22, y + 8, 200, C_TEXT);
    char rssi[16];
    snprintf(rssi, sizeof(rssi), "%ddBm", aps[i].rssi);
    drawTextRight(rssi, 296, y + 8, aps[i].rssi > -65 ? C_GREEN : C_DIM);
  }
}

void drawWifiKeyboard(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  drawBackChevronAA(12.0f, 11.0f, C_TEXT);
  drawText(selectedSsid, 32, 7, 210, C_TEXT);
  fillRoundRectAA(256, 5, 54, 24, 6, C_CYAN);
  drawTextCentered("OK", 283, 7, 48, C_BG);

  drawCardAA(12, 36, 296, 32, 8, C_CARD, C_LINE);
  drawText(wifiPass[0] ? wifiPass : (vi ? "Nhập mật khẩu..." : "Password..."), 20, 42, 280,
           wifiPass[0] ? C_TEXT : C_DIM);

  static const char *const rowsLower[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static const char *const rowsSym[3] = {"1234567890", "-/:;()$&@", ".,?!'\""};
  const char *const *rows = kbSymbols ? rowsSym : rowsLower;

  for (int i = 0; i < 10; ++i) {
    drawCardAA(i * 32 + 2, 76, 28, 34, 6, C_CARD, C_LINE);
    char s[2] = {rows[0][i], '\0'};
    if (!kbSymbols && kbUpper)
      s[0] = char(s[0] - 'a' + 'A');
    drawTextCentered(s, i * 32 + 16, 83, 24, C_TEXT);
  }
  for (int i = 0; i < 9; ++i) {
    drawCardAA(i * 32 + 18, 116, 28, 34, 6, C_CARD, C_LINE);
    char s[2] = {rows[1][i], '\0'};
    if (!kbSymbols && kbUpper)
      s[0] = char(s[0] - 'a' + 'A');
    drawTextCentered(s, i * 32 + 32, 123, 24, C_TEXT);
  }
  drawCardAA(2, 156, 28, 34, 6, kbUpper ? C_BLUE : C_CARD, C_LINE);
  drawTextCentered(kbSymbols ? "#+" : (kbUpper ? "UP" : "up"), 16, 163, 26, kbUpper ? C_BG : C_TEXT);

  const int r2len = kbSymbols ? 6 : 7;
  for (int i = 0; i < r2len; ++i) {
    drawCardAA((i + 1) * 32 + 18, 156, 28, 34, 6, C_CARD, C_LINE);
    char s[2] = {rows[2][i], '\0'};
    if (!kbSymbols && kbUpper)
      s[0] = char(s[0] - 'a' + 'A');
    drawTextCentered(s, (i + 1) * 32 + 32, 163, 24, C_TEXT);
  }
  drawCardAA(280, 156, 38, 34, 6, C_CARD, C_LINE);
  drawTextCentered("DEL", 299, 163, 34, C_RED);

  drawCardAA(4, 198, 62, 34, 8, C_CARD, C_LINE);
  drawTextCentered(kbSymbols ? "abc" : "123", 35, 205, 56, C_TEXT);
  drawCardAA(72, 198, 164, 34, 8, C_CARD, C_LINE);
  drawTextCentered("SPACE", 154, 205, 100, C_DIM);
  fillRoundRectAA(242, 198, 74, 34, 8, C_GREEN);
  drawTextCentered(vi ? "Kết nối" : "Join", 279, 205, 68, C_BG);
}

void drawLanguageSettings(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  drawHeader(vi ? "Ngôn ngữ" : "Language", nullptr);

  drawCardAA(16, 56, 288, 52, 10, vi ? C_CARD : C_BG, vi ? C_TEAL : C_LINE);
  drawText("Tiếng Việt", 32, 72, 200, C_TEXT);
  if (vi)
    fillCircleAA(280.0f, 82.0f, 6.0f, C_TEAL);

  drawCardAA(16, 120, 288, 52, 10, !vi ? C_CARD : C_BG, !vi ? C_TEAL : C_LINE);
  drawText("English", 32, 136, 200, C_TEXT);
  if (!vi)
    fillCircleAA(280.0f, 146.0f, 6.0f, C_TEAL);

  drawSwipeHint();
}

void drawStandbyModeSettings(const View &v) {
  const bool vi = v.language == Language::Vietnamese;
  const bool mode1 = v.standbyMode == StandbyMode::ClockWeather;
  drawHeader(vi ? "Màn hình chờ" : "Standby Mode", nullptr);

  drawCardAA(16, 56, 288, 52, 10, mode1 ? C_CARD : C_BG, mode1 ? C_AMBER : C_LINE);
  drawText(vi ? "1. Đồng hồ & Thời tiết" : "1. Clock & Weather", 32, 72, 230, C_TEXT);
  if (mode1)
    fillCircleAA(280.0f, 82.0f, 6.0f, C_AMBER);

  drawCardAA(16, 120, 288, 52, 10, !mode1 ? C_CARD : C_BG, !mode1 ? C_CYAN : C_LINE);
  drawText("2. SYS://MONITOR", 32, 136, 230, C_TEXT);
  if (!mode1)
    fillCircleAA(280.0f, 146.0f, 6.0f, C_CYAN);

  drawSwipeHint();
}

void drawMusicPlayer(const View &v) {
  const bool vi = v.language == Language::Vietnamese;

  drawBackChevronAA(12.0f, 10.0f, C_TEXT);
  drawText(vi ? "Nhạc" : "Music", 28, 5, 100, C_TEXT);
  char volStr[16];
  snprintf(volStr, sizeof(volStr), "Vol %d%%", std::clamp(v.volume, 0, 100));
  drawTextRight(volStr, 308, 5, C_DIM);

  drawCardAA(14, 34, 112, 112, 10, C_COVER, C_LINE);
  drawNoteIconAA(70.0f, 90.0f, C_VIOLET, 2.0f);

  const char *title = strrchr(v.title, '/');
  drawText(title ? title + 1 : v.title, 138, 34, 172, C_TEXT, 1);
  drawText(v.status, 138, 56, 172, C_DIM, 1);
  drawText(v.lyric[0] ? v.lyric : (vi ? "Chưa có lời bài hát" : "No lyrics"),
           138, 84, 172, v.lyric[0] ? C_AMBER : C_DIM, 3);

  fillRoundRectAA(14, 156, 292, 5, 2, C_LINE);
  if (v.total > 0) {
    const int fillW = int(std::min<uint64_t>(292, uint64_t(v.ms) * 292 / v.total));
    if (fillW > 0)
      fillRoundRectAA(14, 156, fillW, 5, 2, C_VIOLET);
  }
  char posStr[16], durStr[16];
  snprintf(posStr, sizeof(posStr), "%lu:%02lu", (unsigned long)(v.ms / 60000),
           (unsigned long)(v.ms / 1000 % 60));
  if (v.total)
    snprintf(durStr, sizeof(durStr), "%lu:%02lu", (unsigned long)(v.total / 60000),
             (unsigned long)(v.total / 1000 % 60));
  else
    snprintf(durStr, sizeof(durStr), "--:--");
  drawText(posStr, 14, 166, 80, C_DIM);
  drawTextRight(durStr, 306, 166, C_DIM);

  const float cy = 207.0f;
  drawTextCentered("-", 38, 197, 24, C_DIM);
  drawPlayTriangleAA(99.0f, cy, 10.0f, 7.0f, C_TEXT, false);
  fillRect(89, int(cy - 7), 2, 15, C_TEXT);

  fillCircleAA(160.0f, cy, 21.0f, C_VIOLET);
  if (v.playing) {
    fillRoundRectAA(153, int(cy - 8), 5, 16, 1, C_BG);
    fillRoundRectAA(162, int(cy - 8), 5, 16, 1, C_BG);
  } else {
    drawPlayTriangleAA(161.0f, cy, 13.0f, 8.5f, C_BG, true);
  }

  drawPlayTriangleAA(221.0f, cy, 10.0f, 7.0f, C_TEXT, true);
  fillRect(229, int(cy - 7), 2, 15, C_TEXT);
  drawTextCentered("+", 282, 197, 24, C_DIM);
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
  init(0x36, {0xa0});
  init(0x3a, {0x55});
  command(0x20);
  command(0x29);
  vTaskDelay(pdMS_TO_TICKS(20));
  displayDraw(view);
  gpio_set_level(GPIO_NUM_21, 1);
  ESP_LOGI("display", "ST7789 320x240 ready, SPI 20 MHz; XPT2046 software SPI");
}

void displayDraw(const View &v) {
  uint8_t columns[] = {0, 0, 1, 63};
  command(0x2a, columns, 4);
  for (stripeY0 = 0; stripeY0 < 240; stripeY0 += 20) {
    uint8_t rows[] = {0, static_cast<uint8_t>(stripeY0), 0, static_cast<uint8_t>(stripeY0 + 19)};
    command(0x2b, rows, 4);
    command(0x2c);

    for (int i = 0; i < 320 * 20; ++i)
      stripe[i] = C_BG;

    switch (v.screen) {
    case Screen::Standby:
      if (v.standbyMode == StandbyMode::ClockWeather)
        drawStandbyClockWeather(v);
      else
        drawStandbyPcStats(v);
      break;
    case Screen::Menu:
      drawMenu(v);
      break;
    case Screen::Settings:
      drawSettings(v);
      break;
    case Screen::WifiSettings:
      drawWifiSettings(v);
      break;
    case Screen::WifiKeyboard:
      drawWifiKeyboard(v);
      break;
    case Screen::LanguageSettings:
      drawLanguageSettings(v);
      break;
    case Screen::StandbyModeSettings:
      drawStandbyModeSettings(v);
      break;
    case Screen::Player:
      drawMusicPlayer(v);
      break;
    }

    for (int i = 0; i < 320 * 20; ++i)
      stripe[i] = __builtin_bswap16(stripe[i]);
    tx(stripe, 320 * 20 * 2, true);
  }
}

void handleTap(int x, int y) {
  Screen cur = Screen::Standby;
  if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
    cur = view.screen;
    xSemaphoreGive(viewLock);
  }

  auto setScreen = [](Screen next) {
    if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      view.screen = next;
      xSemaphoreGive(viewLock);
    }
  };

  if (cur == Screen::Menu) {
    if ((x <= 100 && y <= 38) || y >= 210)
      setScreen(Screen::Standby);
    else if (x >= 20 && x <= 96 && y >= 40 && y <= 136)
      setScreen(Screen::Settings);
    else if (x >= 104 && x <= 180 && y >= 40 && y <= 136)
      setScreen(Screen::Player);
    return;
  }

  if (cur == Screen::Settings) {
    if (x <= 120 && y <= 34)
      setScreen(Screen::Menu);
    else if (y >= 35 && y <= 78) {
      servicesStartWifiScan();
      setScreen(Screen::WifiSettings);
    } else if (y >= 79 && y <= 122)
      setScreen(Screen::LanguageSettings);
    else if (y >= 123 && y <= 166)
      setScreen(Screen::StandbyModeSettings);
    return;
  }

  if (cur == Screen::WifiSettings) {
    if (x <= 120 && y <= 34) {
      setScreen(Screen::Settings);
    } else if (x >= 240 && y <= 34) {
      servicesStartWifiScan();
    } else if (y >= 66 && y < 234) {
      int idx = (y - 66) / 42;
      bool scanning = false;
      auto aps = servicesGetWifiScanResults(scanning);
      if (idx >= 0 && idx < int(aps.size())) {
        snprintf(selectedSsid, sizeof(selectedSsid), "%s", aps[idx].ssid);
        wifiPass[0] = '\0';
        if (aps[idx].open) {
          servicesConnectWifi(selectedSsid, "");
          setScreen(Screen::Settings);
        } else {
          setScreen(Screen::WifiKeyboard);
        }
      }
    }
    return;
  }

  if (cur == Screen::WifiKeyboard) {
    if (x <= 50 && y <= 34) {
      setScreen(Screen::WifiSettings);
      return;
    }
    if ((x >= 250 && y <= 34) || (x >= 240 && y >= 198)) {
      servicesConnectWifi(selectedSsid, wifiPass);
      setScreen(Screen::WifiSettings);
      return;
    }
    static const char *const rowsLower[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const char *const rowsSym[3] = {"1234567890", "-/:;()$&@", ".,?!'\""};
    const char *const *rows = kbSymbols ? rowsSym : rowsLower;
    size_t len = strlen(wifiPass);
    auto appendChar = [&](char ch) {
      if (len + 1 < sizeof(wifiPass)) {
        wifiPass[len] = ch;
        wifiPass[len + 1] = '\0';
      }
    };
    if (y >= 76 && y < 112) {
      int col = x / 32;
      if (col >= 0 && col < 10) {
        char ch = rows[0][col];
        if (!kbSymbols && kbUpper)
          ch = char(ch - 'a' + 'A');
        appendChar(ch);
      }
    } else if (y >= 116 && y < 152 && x >= 16 && x < 304) {
      int col = (x - 16) / 32;
      if (col >= 0 && col < 9) {
        char ch = rows[1][col];
        if (!kbSymbols && kbUpper)
          ch = char(ch - 'a' + 'A');
        appendChar(ch);
      }
    } else if (y >= 156 && y < 192) {
      if (x < 34) {
        if (!kbSymbols)
          kbUpper = !kbUpper;
      } else if (x >= 280) {
        if (len > 0)
          wifiPass[len - 1] = '\0';
      } else if (x >= 48) {
        int idx = (x - 18) / 32 - 1;
        int r2len = kbSymbols ? 6 : 7;
        if (idx >= 0 && idx < r2len) {
          char ch = rows[2][idx];
          if (!kbSymbols && kbUpper)
            ch = char(ch - 'a' + 'A');
          appendChar(ch);
        }
      }
    } else if (y >= 198) {
      if (x < 68)
        kbSymbols = !kbSymbols;
      else if (x >= 72 && x < 238)
        appendChar(' ');
    }
    if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      ++view.uiDirty;
      xSemaphoreGive(viewLock);
    }
    return;
  }

  if (cur == Screen::LanguageSettings) {
    if (x <= 120 && y <= 34) {
      setScreen(Screen::Settings);
    } else if (y >= 56 && y <= 108) {
      if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
        view.language = Language::Vietnamese;
        servicesSaveSettings(view.standbyMode, view.language);
        xSemaphoreGive(viewLock);
      }
    } else if (y >= 120 && y <= 172) {
      if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
        view.language = Language::English;
        servicesSaveSettings(view.standbyMode, view.language);
        xSemaphoreGive(viewLock);
      }
    }
    return;
  }

  if (cur == Screen::StandbyModeSettings) {
    if (x <= 120 && y <= 34) {
      setScreen(Screen::Settings);
    } else if (y >= 56 && y <= 108) {
      if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
        view.standbyMode = StandbyMode::ClockWeather;
        servicesSaveSettings(view.standbyMode, view.language);
        xSemaphoreGive(viewLock);
      }
    } else if (y >= 120 && y <= 172) {
      if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
        view.standbyMode = StandbyMode::PcStats;
        servicesSaveSettings(view.standbyMode, view.language);
        xSemaphoreGive(viewLock);
      }
    }
    return;
  }

  if (cur == Screen::Player) {
    if (x <= 120 && y <= 34) {
      setScreen(Screen::Menu);
      return;
    }
    if (y >= 182 && y <= 236) {
      Command c;
      if (x < 68)
        c = Command::Quieter;
      else if (x < 130)
        c = Command::Previous;
      else if (x < 190)
        c = Command::Toggle;
      else if (x < 252)
        c = Command::Next;
      else
        c = Command::Louder;
      if (xQueueSend(commands, &c, 0) != pdTRUE)
        ESP_LOGW("touch", "Command dropped: queue full");
    }
  }
}

void pollTouch() {
  static bool held = false;
  static bool swiped = false;
  static int startX = 0, startY = 0;
  static int lastX = 0, lastY = 0;
  static int64_t releasedAt = 0;
  static int64_t lastRejectedLog = 0;
  const int64_t now = esp_timer_get_time();

  auto handleStandbySwipe = [](int dx, int dy) {
    if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (view.screen == Screen::Standby) {
        if (std::abs(dx) > std::abs(dy)) {
          view.standbyMode = (view.standbyMode == StandbyMode::ClockWeather)
                                 ? StandbyMode::PcStats
                                 : StandbyMode::ClockWeather;
          servicesSaveSettings(view.standbyMode, view.language);
        } else {
          view.screen = Screen::Menu;
        }
      }
      xSemaphoreGive(viewLock);
    }
  };

  if (gpio_get_level(GPIO_NUM_36)) {
    if (held && !releasedAt)
      releasedAt = now;
    if (held && releasedAt && now - releasedAt >= 30000) {
      if (!swiped) {
        const int dx = lastX - startX, dy = lastY - startY;
        if (std::abs(dx) >= 24 || std::abs(dy) >= 24) {
          handleStandbySwipe(dx, dy);
          ESP_LOGI("touch", "SWIPE ON RELEASE (%d,%d)->(%d,%d)", startX, startY, lastX, lastY);
        } else {
          handleTap(startX, startY);
        }
      }
      held = false;
      swiped = false;
      ESP_LOGI("touch", "RELEASE");
    }
    return;
  }
  releasedAt = 0;

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

  const int x = 319 - coordinate(rawY, CONFIG_MUSIC_TOUCH_Y_MIN, CONFIG_MUSIC_TOUCH_Y_MAX, 319);
  const int y = 239 - coordinate(rawX, CONFIG_MUSIC_TOUCH_X_MIN, CONFIG_MUSIC_TOUCH_X_MAX, 239);

  if (!held) {
    held = true;
    swiped = false;
    startX = lastX = x;
    startY = lastY = y;
    ESP_LOGI("touch", "PRESS raw=(%d,%d) point=(%d,%d)", rawX, rawY, x, y);
  } else {
    lastX = x;
    lastY = y;
    const int dx = lastX - startX, dy = lastY - startY;
    if (!swiped && (std::abs(dx) >= 24 || std::abs(dy) >= 24)) {
      swiped = true;
      handleStandbySwipe(dx, dy);
      ESP_LOGI("touch", "SWIPE (%d,%d)->(%d,%d)", startX, startY, lastX, lastY);
    }
  }
}
