#include "wav.h"
#include <cassert>
#include <cstdio>
#include <vector>
static void put32(std::vector<uint8_t> &v, size_t i, uint32_t n) {
  for (int j = 0; j < 4; ++j)
    v[i + j] = n >> (j * 8);
}
static std::vector<uint8_t> wav(int bits = 16, int channels = 2, uint32_t rate = 44100) {
  std::vector<uint8_t> v(52, 0);
  memcpy(v.data(), "RIFF", 4);
  put32(v, 4, v.size() - 8);
  memcpy(v.data() + 8, "WAVEfmt ", 8);
  put32(v, 16, 16);
  v[20] = 1;
  v[22] = channels;
  put32(v, 24, rate);
  put32(v, 28, rate * channels * bits / 8);
  v[32] = channels * bits / 8;
  v[34] = bits;
  memcpy(v.data() + 36, "data", 4);
  put32(v, 40, 8);
  return v;
}
static bool parse(const std::vector<uint8_t> &v, WavInfo &info) {
  size_t pos = 0;
  return parseWav(
      [&](uint8_t *p, size_t n) {
        if (n > v.size() - pos)
          return false;
        memcpy(p, v.data() + pos, n);
        pos += n;
        return true;
      },
      [&](size_t n) {
        if (n > v.size() - pos)
          return false;
        pos += n;
        return true;
      },
      info);
}
int main() {
  WavInfo info;
  for (int bits : {8, 16})
    for (int channels : {1, 2})
      for (int rate : {8000, 22050, 44100, 48000}) {
        assert(parse(wav(bits, channels, rate), info));
        assert(info.bits == bits && info.channels == channels && info.rate == unsigned(rate) &&
               info.bytes == 8);
      }
  assert(!parse(wav(24), info));
  assert(!parse(wav(16, 3), info));
  assert(!parse(wav(16, 2, 96000), info));
  auto v = wav();
  v[20] = 3;
  assert(!parse(v, info));
  v = wav();
  v[32] = 1;
  assert(!parse(v, info));
  v = wav();
  put32(v, 28, 1);
  assert(!parse(v, info));
  v = wav();
  put32(v, 40, 7);
  assert(!parse(v, info));
  v = wav();
  put32(v, 40, 0xffffffff);
  assert(!parse(v, info));
  v = wav();
  put32(v, 16, 0xffffffff);
  assert(!parse(v, info));
  v = wav();
  put32(v, 4, 2);
  assert(!parse(v, info));
  v = wav();
  v.resize(30);
  assert(!parse(v, info));
  v = wav();
  std::vector<uint8_t> junk = {'J', 'U', 'N', 'K', 1, 0, 0, 0, 42, 0};
  v.insert(v.begin() + 36, junk.begin(), junk.end());
  put32(v, 4, v.size() - 8);
  assert(parse(v, info));
  v = wav(8, 1, 8000);
  v.resize(44 + 64000);
  put32(v, 4, v.size() - 8);
  put32(v, 40, 64000);
  assert(parse(v, info) && info.duration() == 8000);
  puts("WAV parser tests passed");
}
