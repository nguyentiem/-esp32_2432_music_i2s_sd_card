#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lyrics.h"
#include "player.h"
#include "sdkconfig.h"
#include "source.h"
#include "wav.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

namespace {
constexpr uint32_t OUTPUT_RATE = 48000;
constexpr size_t OUTPUT_FRAMES = 512;
constexpr size_t FRAME_BYTES = 2 * sizeof(int16_t);
constexpr bool audioPin(int pin) {
  // Other CYD pins belong to flash, LCD, SD, touch, UART, or are input-only.
  return pin == 4 || pin == 16 || pin == 17 || pin == 22 || pin == 26 || pin == 27;
}
static_assert(audioPin(CONFIG_MUSIC_I2S_BCLK) && audioPin(CONFIG_MUSIC_I2S_WS) &&
                  audioPin(CONFIG_MUSIC_I2S_DOUT),
              "I2S GPIO conflicts with CYD peripherals");
static_assert(CONFIG_MUSIC_I2S_BCLK != CONFIG_MUSIC_I2S_WS &&
                  CONFIG_MUSIC_I2S_BCLK != CONFIG_MUSIC_I2S_DOUT &&
                  CONFIG_MUSIC_I2S_WS != CONFIG_MUSIC_I2S_DOUT,
              "I2S GPIOs must be distinct");
class Player {
  i2s_chan_handle_t tx = nullptr;
  int index = 0, volume = 35;
  bool paused = false, interrupted = false, playing = false;
  int nextIndex = 0;
  uint64_t samples = 0;
  uint32_t total = 0, phase = 0;
  int64_t lastPublish = 0;
  const char *status = "Ready";
  Lyrics lyrics;
  int16_t output[OUTPUT_FRAMES * 2];
  size_t used = 0;
  uint8_t waveBins[18] = {};
  uint32_t wavePeak = 0;
  uint32_t waveSamples = 0;
  std::unique_ptr<mp3dec_t> mp3;
  std::unique_ptr<int16_t[]> pcm;
  std::unique_ptr<uint8_t[]> input;
  void publish(bool force = false) {
    int64_t now = esp_timer_get_time();
    if (!force && now - lastPublish < 100000)
      return;
    if (!playing || paused) {
      for (uint8_t &b : waveBins)
        b = (b * 3) / 5;
    }
    View copy;
    snprintf(copy.title, sizeof(copy.title), "%s",
             tracks.empty() ? "SD /music or HTTP URL" : tracks[index].c_str());
    snprintf(copy.status, sizeof(copy.status), "%s", paused ? "Paused" : status);
    copy.ms = samples * 1000 / OUTPUT_RATE;
    copy.total = total;
    copy.volume = volume;
    copy.playing = playing && !paused;
    int line = lyrics.index(copy.ms);
    if (line >= 0) {
      snprintf(copy.lyric, sizeof(copy.lyric), "%s", lyrics.lines[line].text.c_str());
      if (line > 0)
        snprintf(copy.lyricPrev, sizeof(copy.lyricPrev), "%s", lyrics.lines[line - 1].text.c_str());
      if (line + 1 < static_cast<int>(lyrics.lines.size()))
        snprintf(copy.lyricNext, sizeof(copy.lyricNext), "%s", lyrics.lines[line + 1].text.c_str());
    } else if (!lyrics.lines.empty()) {
      snprintf(copy.lyricNext, sizeof(copy.lyricNext), "%s", lyrics.lines[0].text.c_str());
    }
    memcpy(copy.waveBins, waveBins, sizeof(copy.waveBins));
    xSemaphoreTake(viewLock, portMAX_DELAY);
    memcpy(view.title, copy.title, sizeof(view.title));
    memcpy(view.lyricPrev, copy.lyricPrev, sizeof(view.lyricPrev));
    memcpy(view.lyric, copy.lyric, sizeof(view.lyric));
    memcpy(view.lyricNext, copy.lyricNext, sizeof(view.lyricNext));
    memcpy(view.status, copy.status, sizeof(view.status));
    memcpy(view.waveBins, copy.waveBins, sizeof(view.waveBins));
    view.ms = copy.ms;
    view.total = copy.total;
    view.volume = copy.volume;
    view.playing = copy.playing;
    xSemaphoreGive(viewLock);
    lastPublish = now;
  }
  bool write(const int16_t *data, size_t frames, bool music) {
    const size_t n = frames * FRAME_BYTES;
    size_t loaded = 0;
    esp_err_t result = i2s_channel_write(tx, data, n, &loaded, 100);
    if (music)
      samples += loaded / FRAME_BYTES;
    if (result != ESP_OK || loaded != n) {
      ESP_LOGE("player", "I2S write: %s, %u/%u bytes", esp_err_to_name(result),
               unsigned(loaded), unsigned(n));
      status = "I2S write error";
      return false;
    }
    return true;
  }
  bool service() {
    do {
      Command cmd;
      while (xQueueReceive(commands, &cmd, 0) == pdTRUE) {
        switch (cmd) {
        case Command::Previous:
        case Command::Next:
          if (!tracks.empty()) {
            nextIndex =
                (nextIndex + static_cast<int>(tracks.size()) + (cmd == Command::Next ? 1 : -1)) %
                static_cast<int>(tracks.size());
            interrupted = true;
          }
          break;
        case Command::Toggle:
          if (playing)
            paused = !paused;
          else if (!tracks.empty()) {
            nextIndex = index;
            interrupted = true;
          }
          break;
        case Command::Quieter:
          volume = std::max(0, volume - 5);
          break;
        case Command::Louder:
          volume = std::min(100, volume + 5);
          break;
        case Command::Pause:
          if (playing)
            paused = true;
          break;
        case Command::Resume:
          if (playing)
            paused = false;
          else if (!tracks.empty()) {
            nextIndex = index;
            interrupted = true;
          }
          break;
        }
      }
      publish();
      if (interrupted)
        return false;
      // DMA auto-clear supplies zero after queued PCM drains. Never enqueue
      // silence here: source/UI service also runs between valid music blocks.
      if (paused)
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (paused);
    return true;
  }
  bool flush() {
    if (!used)
      return true;
    if (!service() || !write(output, used, true))
      return false;
    used = 0;
    return true;
  }
  bool emit(const int16_t *data, size_t frames, int channels, uint32_t rate) {
    if (rate < 8000 || rate > OUTPUT_RATE || (channels != 1 && channels != 2))
      return false;
    for (size_t i = 0; i < frames; ++i) {
      int sample = data[i * channels];
      if (channels == 2)
        sample = (sample + data[i * channels + 1]) / 2;
      int16_t value = static_cast<int16_t>(sample * volume / 100);
      uint32_t mag = static_cast<uint32_t>(sample < 0 ? -sample : sample);
      if (mag > wavePeak)
        wavePeak = mag;
      if (++waveSamples >= 256) {
        uint8_t level = static_cast<uint8_t>(std::min<uint32_t>(255, (wavePeak * 255) / 20000));
        memmove(waveBins, waveBins + 1, sizeof(waveBins) - 1);
        waveBins[sizeof(waveBins) - 1] = level;
        wavePeak = 0;
        waveSamples = 0;
      }
      // Rational zero-order-hold resampling to a stable I2S clock.
      phase += OUTPUT_RATE;
      while (phase >= rate) {
        phase -= rate;
        // Duplicate mono into both slots: any MAX98357A channel selection works.
        output[used * 2] = value;
        output[used * 2 + 1] = value;
        ++used;
        if (used == OUTPUT_FRAMES && !flush())
          return false;
      }
    }
    return true;
  }
  void loadLyrics(const std::string &path) {
    lyrics.lines.clear();
    std::string lrc = path;
    if (path.compare(0, 7, "http://") == 0)
      lrc = "/sdcard/music/" + path.substr(path.find_last_of('/') + 1);
    auto query = lrc.find('?');
    if (query != std::string::npos)
      lrc.resize(query);
    auto dot = lrc.find_last_of('.');
    if (dot == std::string::npos)
      return;
    lrc.replace(dot, std::string::npos, ".lrc");
    FILE *f = fopen(lrc.c_str(), "rb");
    if (!f)
      return;
    if (fseek(f, 0, SEEK_END) == 0) {
      long n = ftell(f);
      if (n > 0 && n <= 32768 && fseek(f, 0, SEEK_SET) == 0) {
        std::string text(static_cast<size_t>(n), '\0');
        if (fread(&text[0], 1, n, f) == static_cast<size_t>(n))
          lyrics.parse(text);
      }
    }
    fclose(f);
  }
  bool wav(Source &source) {
    auto callback = [&]() {
      status = source.buffering() ? "Buffering" : "Playing";
      return service();
    };
    WavInfo info;
    if (!parseWav([&](uint8_t *p, size_t n) { return source.read(p, n, callback) == n; },
                  [&](size_t n) { return source.skip(n, callback); }, info))
      return false;
    total = info.duration();
    size_t frameBytes = info.channels * (info.bits / 8);
    uint32_t left = info.bytes;
    while (left) {
      size_t n = std::min<size_t>(left, 1024);
      n -= n % frameBytes;
      if (source.read(input.get(), n, callback) != n)
        return false;
      status = "Playing";
      size_t count = n / (info.bits / 8);
      for (size_t i = 0; i < count; ++i)
        pcm[i] = info.bits == 8 ? (int(input[i]) - 128) * 256
                                : static_cast<int16_t>(little16(input.get() + 2 * i));
      if (!emit(pcm.get(), n / frameBytes, info.channels, info.rate))
        return false;
      left -= n;
    }
    return flush() && !source.error();
  }
  bool mp3Stream(Source &source) {
    auto callback = [&]() {
      status = source.buffering() ? "Buffering" : "Playing";
      return service();
    };
    mp3dec_init(mp3.get());
    size_t count = source.read(input.get(), 10, callback);
    uint32_t tagBytes = 0;
    if (count == 10 && !memcmp(input.get(), "ID3", 3)) {
      for (int i = 6; i < 10; ++i)
        if (input[i] & 128)
          return false;
      tagBytes = (uint32_t(input[6]) << 21) | (uint32_t(input[7]) << 14) |
                 (uint32_t(input[8]) << 7) | input[9];
      if (input[3] == 4 && (input[5] & 0x10))
        tagBytes += 10;
      if (tagBytes > 16 * 1024 * 1024 || !source.skip(tagBytes, callback))
        return false;
      tagBytes += 10;
      count = 0;
    }
    bool decoded = false;
    uint32_t skipped = 0;
    while (!interrupted) {
      count += source.read(input.get() + count, 8192 - count, callback);
      if (interrupted)
        return false;
      if (!count)
        break;
      mp3dec_frame_info_t info = {};
      int frames = mp3dec_decode_frame(mp3.get(), input.get(), count, pcm.get(), &info);
      if (frames > 0) {
        status = "Playing";
        if (!decoded && info.bitrate_kbps > 0 && source.size() > tagBytes)
          total =
              std::min<uint64_t>(UINT32_MAX, (source.size() - tagBytes) * 8 / info.bitrate_kbps);
        decoded = true;
        skipped = 0;
        if (!emit(pcm.get(), frames, info.channels, info.hz))
          return false;
      } else
        skipped += std::max(info.frame_bytes, 1);
      size_t consumed = info.frame_bytes > 0 ? static_cast<size_t>(info.frame_bytes) : count;
      if (consumed > count || skipped > 65536)
        return false;
      count -= consumed;
      memmove(input.get(), input.get() + consumed, count);
      if (!service())
        return false;
      // Yield even on damaged MP3 input so the idle task can feed its watchdog.
      vTaskDelay(1);
    }
    return decoded && flush() && !source.error();
  }

public:
  void run() {
    mp3.reset(new (std::nothrow) mp3dec_t);
    pcm.reset(new (std::nothrow) int16_t[MINIMP3_MAX_SAMPLES_PER_FRAME]);
    input.reset(new (std::nothrow) uint8_t[8192]);
    if (!mp3 || !pcm || !input) {
      status = "Audio allocation failed";
      publish(true);
      return;
    }
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // 12 * 256 / 48000 = 64 ms of PCM reserve (12 KiB of DMA RAM).
    channel.dma_desc_num = 12;
    channel.dma_frame_num = 256;
    channel.auto_clear_after_cb = true; // Underrun sends PCM zero, never repeats stale audio.
    i2s_std_config_t cfg = {};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUTPUT_RATE);
    cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_MUSIC_I2S_BCLK);
    cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_MUSIC_I2S_WS);
    cfg.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_MUSIC_I2S_DOUT);
    cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    esp_err_t result = i2s_new_channel(&channel, &tx, nullptr);
    if (result == ESP_OK)
      result = i2s_channel_init_std_mode(tx, &cfg);
    if (result == ESP_OK)
      result = i2s_channel_enable(tx);
    if (result != ESP_OK) {
      ESP_LOGE("player", "I2S init failed: %s", esp_err_to_name(result));
      if (tx) {
        i2s_del_channel(tx);
        tx = nullptr;
      }
      status = "I2S init failed";
      publish(true);
      return;
    }
    bool load = false;
    paused = true;
    for (;;) {
      if (load) {
        index = nextIndex;
        interrupted = paused = false;
        playing = true;
        samples = total = phase = used = 0;
        status = "Buffering";
        loadLyrics(tracks[index]);
        publish(true);
        bool ok = false;
        {
          Source source(tracks[index]);
          if (source.start()) {
            std::string name = tracks[index].substr(0, tracks[index].find('?'));
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            bool isWav = name.size() >= 4 && name.compare(name.size() - 4, 4, ".wav") == 0;
            ok = isWav ? wav(source) : mp3Stream(source);
            if (source.error())
              status = "Network/SD read error";
          } else
            status = "Source allocation failed";
          // Cancel producer and release its handles before opening another track.
        }
        playing = false;
        paused = false;
        if (interrupted)
          load = true;
        else if (ok) {
          nextIndex = (index + 1) % tracks.size();
          load = true;
        } else {
          if (!strcmp(status, "Playing") || !strcmp(status, "Buffering"))
            status = "Format/decode error";
          load = false;
        }
        interrupted = false;
        publish(true);
      } else {
        status = tracks.empty() ? "No tracks" : "Paused";
        service();
        if (interrupted) {
          interrupted = false;
          load = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
  }
};
} // namespace
void playerTask(void *) {
  auto player = new (std::nothrow) Player;
  if (player) {
    player->run();
    delete player;
  } else {
    ESP_LOGE("player", "Cannot allocate player");
  }
  vTaskDelete(nullptr);
}
