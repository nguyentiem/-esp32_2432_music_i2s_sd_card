#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
inline uint32_t little32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint16_t little16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
struct WavInfo {
  uint32_t rate = 0, bytes = 0;
  uint16_t channels = 0, bits = 0;
  uint32_t duration() const { return uint64_t(bytes) * 1000 / (rate * channels * (bits / 8)); }
};
// read must return exactly n bytes; skip must fail on truncated input.
template <class Read, class Skip> bool parseWav(Read read, Skip skip, WavInfo &out) {
  out = {};
  uint8_t h[16];
  if (!read(h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
    return false;
  uint32_t riff = little32(h + 4);
  if (riff < 4)
    return false;
  uint64_t left = riff - 4;
  bool format = false;
  while (left >= 8) {
    if (!read(h, 8))
      return false;
    left -= 8;
    uint32_t n = little32(h + 4);
    uint64_t padded = uint64_t(n) + (n & 1);
    if (padded > left)
      return false;
    if (!memcmp(h, "data", 4)) {
      if (!format || !n || n % (out.channels * (out.bits / 8)))
        return false;
      out.bytes = n;
      return true;
    }
    if (!memcmp(h, "fmt ", 4)) {
      if (n < 16 || !read(h, 16))
        return false;
      out.channels = little16(h + 2);
      out.rate = little32(h + 4);
      out.bits = little16(h + 14);
      if (little16(h) != 1 || (out.channels != 1 && out.channels != 2) ||
          (out.bits != 8 && out.bits != 16) || out.rate < 8000 || out.rate > 48000 ||
          little16(h + 12) != out.channels * (out.bits / 8) ||
          little32(h + 8) != out.rate * out.channels * (out.bits / 8))
        return false;
      if (!skip(padded - 16))
        return false;
      format = true;
    } else if (!skip(padded))
      return false;
    left -= padded;
  }
  return false;
}
