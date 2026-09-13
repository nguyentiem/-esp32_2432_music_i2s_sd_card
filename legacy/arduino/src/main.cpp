#include "duration.h"
#include "lyrics.h"
#include "source.h"
#include <Arduino.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <esp_wifi.h>

TFT_eSPI screen;
U8g2_for_TFT_eSPI unicode;
SPIClass sdBus(VSPI);
std::vector<String> tracks;
QueueHandle_t commands;
SemaphoreHandle_t stateLock;
struct View {
  char title[96] = "Insert SD: /music";
  char lyric[192] = "";
  char status[32] = "Ready";
  uint32_t ms = 0, total = 0;
  int volume = 35;
} view;
enum Command { Previous, Toggle, Next, Quieter, Louder };
class Output : public AudioOutputI2S {
public:
  uint64_t elapsedUs = 0;
  uint32_t fraction = 0;
  Output() : AudioOutputI2S(0, INTERNAL_DAC, 4) {}
  bool begin() override {
    if (i2sOn)
      return true;
    i2s_config_t cfg = {};
    cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    cfg.sample_rate = hertz;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 128;
    cfg.tx_desc_auto_clear = true;
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK)
      return false;
    // Do not call i2s_set_pin(NULL): it enables both DACs, including touch CLK GPIO25.
    if (i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN) != ESP_OK) {
      i2s_driver_uninstall(I2S_NUM_0);
      return false;
    }
    i2sOn = true;
    return true;
  }
  bool stop() override {
    if (i2sOn) {
      i2s_stop(I2S_NUM_0);
      i2s_driver_uninstall(I2S_NUM_0);
      i2sOn = false;
    }
    dacWrite(26, 128);
    return true;
  }
  void silence() {
    int16_t neutral = bps == 8 ? 128 : 0;
    int16_t s[2] = {neutral, neutral};
    for (int i = 0; i < 512 && AudioOutputI2S::ConsumeSample(s); ++i) {
    }
  }
  bool ConsumeSample(int16_t s[2]) override {
    if (!AudioOutputI2S::ConsumeSample(s))
      return false;
    fraction += 1000000;
    elapsedUs += fraction / hertz;
    fraction %= hertz;
    return true;
  }
};
void publish(const String &title, const String &status, uint32_t ms, int volume,
             const String &lyric, uint32_t total = 0) {
  xSemaphoreTake(stateLock, portMAX_DELAY);
  strlcpy(view.title, title.c_str(), sizeof(view.title));
  strlcpy(view.status, status.c_str(), sizeof(view.status));
  strlcpy(view.lyric, lyric.c_str(), sizeof(view.lyric));
  view.ms = ms;
  view.volume = volume;
  view.total = total;
  xSemaphoreGive(stateLock);
}
class Mp3Decoder : public AudioGeneratorMP3 {
public:
  Mp3Decoder() {
    stream = nullptr;
    frame = nullptr;
    synth = nullptr;
  }
};
void player(void *) {
  Output out;
  out.SetOutputModeMono(true);
  BufferedSource *source = nullptr;
  AudioGenerator *decoder = nullptr;
  Lyrics lyrics;
  int index = 0, volume = 35;
  bool paused = false, load = !tracks.empty();
  String status = "No tracks";
  uint32_t lastUpdate = 0, total = 0;
  for (;;) {
    Command cmd;
    while (xQueueReceive(commands, &cmd, 0) == pdTRUE) {
      if (cmd == Next || cmd == Previous) {
        if (!tracks.empty()) {
          index = (index + tracks.size() + (cmd == Next ? 1 : -1)) % tracks.size();
          load = true;
        }
      }
      if (cmd == Toggle) {
        if (decoder)
          paused = !paused;
        else
          load = !tracks.empty();
      }
      if (cmd == Quieter)
        volume = std::max(0, volume - 5);
      if (cmd == Louder)
        volume = std::min(100, volume + 5);
    }
    out.SetGain(volume / 100.0f);
    if (load) {
      if (decoder) {
        decoder->stop();
        delete decoder;
        decoder = nullptr;
      }
      delete source;
      source = nullptr;
      lyrics.lines.clear();
      out.elapsedUs = 0;
      out.fraction = 0;
      paused = false;
      load = false;
      String path = tracks[index];
      total = 0;
      {
        String lyricPath = path;
        if (path.startsWith("http://"))
          lyricPath = "/music/" + path.substring(path.lastIndexOf('/') + 1);
        else
          total = durationMs(path);
        String lrc = lyricPath.substring(0, lyricPath.lastIndexOf('.')) + ".lrc";
        File f = SD.open(lrc);
        if (f && f.size() <= 32768) {
          String data = f.readString();
          lyrics.parse(std::string(data.c_str()));
        }
        f.close();
      }
      source = new BufferedSource;
      source->onWait = [&]() {
        out.silence();
        publish(tracks[index], "Buffering", out.elapsedUs / 1000, volume, "", total);
      };
      if (!source->start(path)) {
        delete source;
        source = nullptr;
        status = "Out of memory";
      } else
        status = "Buffering";
    }
    if (source && !decoder && source->ready() && source->error()) {
      delete source;
      source = nullptr;
      status = "Network/read error";
    }
    if (source && !decoder && source->ready()) {
      String lower = tracks[index];
      lower.toLowerCase();
      decoder = lower.endsWith(".wav") ? static_cast<AudioGenerator *>(new AudioGeneratorWAV)
                                       : static_cast<AudioGenerator *>(new Mp3Decoder);
      if (!decoder->begin(source, &out)) {
        decoder->stop();
        delete decoder;
        decoder = nullptr;
        delete source;
        source = nullptr;
        status = "Open/decode error";
      } else
        status = "Playing";
    }
    if (decoder && paused)
      out.silence();
    if (decoder && !paused) {
      if (!decoder->loop()) {
        bool error = source->error() || out.elapsedUs < 1000;
        decoder->stop();
        delete decoder;
        decoder = nullptr;
        delete source;
        source = nullptr;
        status = error ? "Read/decode error" : "Finished";
        if (!error && !tracks.empty()) {
          index = (index + 1) % tracks.size();
          load = true;
        }
      }
    }
    if (millis() - lastUpdate >= 100) {
      uint32_t ms = out.elapsedUs / 1000;
      int line = lyrics.index(ms);
      publish(tracks.empty() ? "SD /music or /streams.txt" : tracks[index],
              paused ? "Paused" : status, ms, volume,
              line >= 0 ? String(lyrics.lines[line].text.c_str()) : String(""), total);
      lastUpdate = millis();
    }
    vTaskDelay(1);
  }
}
// Software SPI keeps the touch controller independent of LCD HSPI and SD VSPI.
uint16_t touchRead(uint8_t command) {
  digitalWrite(33, LOW);
  for (int i = 7; i >= 0; --i) {
    digitalWrite(32, (command >> i) & 1);
    digitalWrite(25, HIGH);
    delayMicroseconds(1);
    digitalWrite(25, LOW);
  }
  uint16_t value = 0;
  for (int i = 0; i < 16; ++i) {
    digitalWrite(25, HIGH);
    delayMicroseconds(1);
    value = (value << 1) | digitalRead(39);
    digitalWrite(25, LOW);
  }
  digitalWrite(33, HIGH);
  return (value >> 3) & 4095;
}
void utf8Lines(const String &text, int y, int maxLines) {
  String line;
  int row = 0;
  for (unsigned i = 0; i < text.length() && row < maxLines;) {
    unsigned char c = text[i];
    unsigned n = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
    String glyph = text.substring(i, std::min<unsigned>(i + n, text.length()));
    i += n;
    String candidate = line + glyph;
    if (unicode.getUTF8Width(candidate.c_str()) > 296) {
      unicode.drawUTF8(12, y + row * 19, line.c_str());
      ++row;
      line = "";
    }
    line += glyph;
  }
  if (row < maxLines)
    unicode.drawUTF8(12, y + row * 19, line.c_str());
}
void draw(const View &v) {
  screen.fillRect(0, 0, 320, 190, TFT_BLACK);
  screen.setTextColor(TFT_CYAN, TFT_BLACK);
  screen.drawString("CYD MUSIC", 12, 8, 2);
  screen.setTextColor(TFT_WHITE, TFT_BLACK);
  String title = v.title;
  int slash = title.lastIndexOf('/');
  if (slash >= 0)
    title = title.substring(slash + 1);
  unicode.setForegroundColor(TFT_WHITE);
  utf8Lines(title, 50, 1);
  screen.setTextColor(TFT_YELLOW, TFT_BLACK);
  screen.drawString(v.status, 12, 62, 2);
  unicode.setForegroundColor(TFT_WHITE);
  utf8Lines(v.lyric, 106, 3);
  char time[48];
  snprintf(time, sizeof(time), "%02lu:%02lu    Volume %d%%", (unsigned long)(v.ms / 60000),
           (unsigned long)(v.ms / 1000 % 60), v.volume);
  screen.drawString(time, 12, 156, 2);
  screen.drawRect(12, 181, 296, 7, TFT_DARKGREY);
  if (v.total)
    screen.fillRect(13, 182, std::min<uint32_t>(294, uint64_t(v.ms) * 294 / v.total), 5, TFT_CYAN);
  const char *labels[] = {"PREV", "PLAY", "NEXT", "VOL-", "VOL+"};
  for (int i = 0; i < 5; ++i) {
    screen.drawRoundRect(i * 64 + 2, 198, 60, 38, 5, TFT_CYAN);
    screen.drawCentreString(labels[i], i * 64 + 32, 209, 2);
  }
}
void setup() {
  Serial.begin(115200);
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  pinMode(33, OUTPUT);
  digitalWrite(33, HIGH);
  pinMode(25, OUTPUT);
  pinMode(32, OUTPUT);
  pinMode(39, INPUT);
  pinMode(36, INPUT);
  screen.init();
  unicode.begin(screen);
  unicode.setFont(u8g2_font_unifont_t_vietnamese2);
  unicode.setFontMode(1);
  screen.setRotation(1);
  screen.fillScreen(TFT_BLACK);
  screen.setTextColor(TFT_WHITE);
  screen.drawString("Starting...", 10, 10, 2);
  sdBus.begin(18, 19, 23, 5);
  if (SD.begin(5, sdBus, 10000000)) {
    File dir = SD.open("/music");
    if (dir && dir.isDirectory()) {
      for (File f = dir.openNextFile(); f && tracks.size() < MAX_TRACKS; f = dir.openNextFile()) {
        String name = f.name(), lower = name;
        lower.toLowerCase();
        if (!f.isDirectory() && (lower.endsWith(".mp3") || lower.endsWith(".wav")))
          tracks.push_back(name.startsWith("/") ? name : "/music/" + name);
        f.close();
      }
    }
    dir.close();
    std::sort(tracks.begin(), tracks.end());
    File list = SD.open("/streams.txt");
    while (list && list.available() && tracks.size() < MAX_TRACKS) {
      String line = list.readStringUntil('\n');
      line.trim();
      if (line.startsWith("http://"))
        tracks.push_back(line);
    }
    list.close();
  } else
    Serial.println("SD mount failed");
  if (strlen(STREAM_URL) && String(STREAM_URL).startsWith("http://") && tracks.size() < MAX_TRACKS)
    tracks.push_back(STREAM_URL);
  if (strlen(WIFI_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000)
      delay(100);
    if (WiFi.status() == WL_CONNECTED)
      esp_wifi_set_ps(WIFI_PS_NONE);
  }
  commands = xQueueCreate(8, sizeof(Command));
  stateLock = xSemaphoreCreateMutex();
  if (!commands || !stateLock ||
      xTaskCreatePinnedToCore(player, "player", 8192, nullptr, 3, nullptr, 1) != pdPASS) {
    screen.drawString("Task allocation failed", 10, 40, 2);
    while (true)
      delay(1000);
  }
  Serial.println("Commands: p=play/pause, n=next, b=previous, +/- volume");
}
void loop() {
  static uint32_t lastDraw = 0, lastTouch = 0;
  static View previous;
  static bool first = true;
  if (millis() - lastDraw > 250) {
    View copy;
    xSemaphoreTake(stateLock, portMAX_DELAY);
    copy = view;
    xSemaphoreGive(stateLock);
    if (first || strcmp(copy.title, previous.title) || strcmp(copy.lyric, previous.lyric) ||
        strcmp(copy.status, previous.status) || copy.ms / 1000 != previous.ms / 1000 ||
        copy.volume != previous.volume) {
      draw(copy);
      previous = copy;
      first = false;
    }
    lastDraw = millis();
  }
  if (digitalRead(36) == LOW && millis() - lastTouch > 300) {
    int rawY = touchRead(0x90), rawX = touchRead(0xD0);
    int x = constrain(map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, 0, 319), 0, 319);
    int y = constrain(map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 239), 0, 239);
    if (y >= 195) {
      Command c = static_cast<Command>(x / 64);
      xQueueSend(commands, &c, 0);
    }
    lastTouch = millis();
  }
  if (Serial.available()) {
    char c = Serial.read();
    int n = c == 'b' ? 0 : c == 'p' ? 1 : c == 'n' ? 2 : c == '-' ? 3 : c == '+' ? 4 : -1;
    if (n >= 0) {
      Command cmd = static_cast<Command>(n);
      xQueueSend(commands, &cmd, 0);
    }
  }
  delay(10);
}
