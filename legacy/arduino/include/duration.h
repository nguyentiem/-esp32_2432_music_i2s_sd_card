#pragma once
#include <SD.h>
// WAV: exact PCM duration. MP3: CBR estimate from first valid MPEG Layer III frame.
inline uint32_t durationMs(const String &path) {
  File f = SD.open(path);
  if (!f)
    return 0;
  uint8_t h[12];
  if (f.read(h, 12) != 12)
    return 0;
  auto le = [](const uint8_t *p) -> uint32_t {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  };
  if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
    uint32_t rate = 0;
    while (f.available()) {
      uint8_t c[8];
      if (f.read(c, 8) != 8)
        break;
      uint32_t n = le(c + 4);
      uint32_t start = f.position();
      if (n > f.size() - start)
        break;
      if (!memcmp(c, "fmt ", 4) && n >= 16) {
        uint8_t fmt[16];
        if (f.read(fmt, 16) != 16)
          break;
        if (fmt[0] != 1 || fmt[1] != 0)
          return 0;
        rate = le(fmt + 8);
      }
      if (!memcmp(c, "data", 4) && rate)
        return uint64_t(n) * 1000 / rate;
      if (!f.seek(start + n + (n & 1)))
        break;
    }
    return 0;
  }
  uint32_t start = 0;
  if (!memcmp(h, "ID3", 3))
    start = 10 + (uint32_t(h[6] & 127) << 21) + (uint32_t(h[7] & 127) << 14) +
            (uint32_t(h[8] & 127) << 7) + (h[9] & 127);
  if (start >= f.size() || !f.seek(start))
    return 0;
  uint32_t word = 0;
  const int high[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  const int low[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  for (int i = 0; i < 8192 && f.available(); ++i) {
    word = (word << 8) | f.read();
    unsigned version = (word >> 19) & 3, layer = (word >> 17) & 3, bitrate = (word >> 12) & 15,
             sample = (word >> 10) & 3;
    if ((word & 0xffe00000) == 0xffe00000 && version != 1 && layer == 1 && bitrate > 0 &&
        bitrate < 15 && sample < 3)
      return uint64_t(f.size() - start) * 8 / (version == 3 ? high[bitrate] : low[bitrate]);
  }
  return 0;
}
