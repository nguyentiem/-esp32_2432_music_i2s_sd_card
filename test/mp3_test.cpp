#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include <cassert>
#include <cstdio>
#include <vector>
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  assert(f);
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  rewind(f);
  assert(length > 0);
  std::vector<uint8_t> data(length);
  assert(fread(data.data(), 1, length, f) == size_t(length));
  fclose(f);
  mp3dec_t decoder;
  mp3dec_init(&decoder);
  int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  size_t pos = 0, frames = 0;
  int peak = 0;
  while (pos < data.size()) {
    mp3dec_frame_info_t info = {};
    int n = mp3dec_decode_frame(&decoder, data.data() + pos, data.size() - pos, pcm, &info);
    if (!info.frame_bytes)
      break;
    pos += info.frame_bytes;
    if (n) {
      assert(info.hz == 44100 && info.channels == 2);
      frames += n;
      for (int i = 0; i < n * info.channels; ++i)
        if (pcm[i] > peak)
          peak = pcm[i];
    }
  }
  assert(frames >= 44100 && frames < 48000 && peak > 100);
  puts("MP3 decode test passed");
}
