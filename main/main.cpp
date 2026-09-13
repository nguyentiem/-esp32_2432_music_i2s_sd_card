#include "config.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "player.h"
#include "sdmmc_cmd.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
QueueHandle_t commands;
SemaphoreHandle_t viewLock;
View view;
std::vector<std::string> tracks;
namespace {
void scanSD() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = 23;
  bus.miso_io_num = 19;
  bus.sclk_io_num = 18;
  bus.quadwp_io_num = bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI3_HOST;
  host.max_freq_khz = 10000;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = SPI3_HOST;
  slot.gpio_cs = GPIO_NUM_5;
  esp_vfs_fat_sdmmc_mount_config_t mount = {};
  mount.format_if_mount_failed = false;
  mount.max_files = 4;
  mount.allocation_unit_size = 16 * 1024;
  sdmmc_card_t *card = nullptr;
  esp_err_t result = esp_vfs_fat_sdspi_mount(SD_ROOT, &host, &slot, &mount, &card);
  if (result != ESP_OK) {
    ESP_LOGW("sd", "Mount failed: %s; HTTP remains available", esp_err_to_name(result));
    return;
  }
  sdmmc_card_print_info(stdout, card);
  DIR *root = opendir(SD_ROOT);
  if (root) {
    while (auto *entry = readdir(root))
      ESP_LOGI("sd", "Root entry: [%s] type=%u", entry->d_name, unsigned(entry->d_type));
    closedir(root);
  } else {
    ESP_LOGE("sd", "Cannot open root: errno=%d (%s)", errno, strerror(errno));
  }
  const char *musicRoot = "/sdcard/music";
  DIR *dir = opendir(musicRoot);
  if (!dir && errno == ENOENT) {
    ESP_LOGW("sd", "/music missing; scanning card root instead");
    musicRoot = SD_ROOT;
    dir = opendir(musicRoot);
  }
  if (!dir)
    ESP_LOGE("sd", "Cannot open %s: errno=%d (%s)", musicRoot, errno, strerror(errno));
  if (dir) {
    for (;;) {
      errno = 0;
      auto *entry = readdir(dir);
      if (!entry) {
        if (errno)
          ESP_LOGE("sd", "Read music directory: errno=%d (%s)", errno, strerror(errno));
        break;
      }
      ESP_LOGI("sd", "Music entry: [%s] type=%u", entry->d_name, unsigned(entry->d_type));
      if (tracks.size() >= MAX_TRACKS)
        break;
      std::string name = entry->d_name, lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower.size() < 4 || (lower.compare(lower.size() - 4, 4, ".mp3") &&
                               lower.compare(lower.size() - 4, 4, ".wav"))) {
        ESP_LOGI("sd", "Skipped: unsupported extension");
        continue;
      }
      std::string path = std::string(musicRoot) + "/" + name;
      struct stat st = {};
      if (stat(path.c_str(), &st) != 0) {
        ESP_LOGE("sd", "stat [%s]: errno=%d (%s)", path.c_str(), errno, strerror(errno));
      } else if (S_ISREG(st.st_mode)) {
        tracks.push_back(path);
        ESP_LOGI("sd", "Accepted: %ld bytes", long(st.st_size));
      } else {
        ESP_LOGW("sd", "Skipped: not a regular file, mode=%lx", (unsigned long)st.st_mode);
      }
    }
    closedir(dir);
  }
  std::sort(tracks.begin(), tracks.end());
  FILE *list = fopen("/sdcard/streams.txt", "r");
  if (list) {
    char line[1024];
    while (tracks.size() < MAX_TRACKS && fgets(line, sizeof(line), list)) {
      size_t n = strlen(line);
      if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
        int c;
        while ((c = fgetc(list)) != '\n' && c != EOF) {
        }
        continue;
      }
      while (n && std::isspace(static_cast<unsigned char>(line[n - 1])))
        line[--n] = 0;
      char *start = line;
      while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
      if (!strncmp(start, "http://", 7))
        tracks.emplace_back(start);
    }
    fclose(list);
  }
  ESP_LOGI("sd", "Found %u tracks/streams", unsigned(tracks.size()));
}
void wifiEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED))
    esp_wifi_connect();
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    ESP_LOGI("wifi", "Connected");
}
void startWiFi() {
  if (!strlen(WIFI_SSID))
    return;
  wifi_config_t settings = {};
  if (strlen(WIFI_SSID) > sizeof(settings.sta.ssid) ||
      strlen(WIFI_PASSWORD) > sizeof(settings.sta.password)) {
    ESP_LOGE("wifi", "SSID/password too long");
    return;
  }
  memcpy(settings.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
  memcpy(settings.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_ERR_NO_MEM);
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &settings));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}
} // namespace
extern "C" void app_main() {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  commands = xQueueCreate(8, sizeof(Command));
  viewLock = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(commands && viewLock ? ESP_OK : ESP_ERR_NO_MEM);
  displayInit();
  scanSD();
  if (!strncmp(STREAM_URL, "http://", 7) && tracks.size() < MAX_TRACKS)
    tracks.emplace_back(STREAM_URL);
  startWiFi();
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, 115200));
  if (xTaskCreatePinnedToCore(playerTask, "player", 24576, nullptr, 4, nullptr, 1) != pdPASS) {
    snprintf(view.status, sizeof(view.status), "Task allocation failed");
    displayDraw(view);
    ESP_LOGE("main", "Cannot start player");
    return;
  }
  ESP_LOGI("main", "p=play/pause, n=next, b=previous, +/- volume; free heap=%lu",
           (unsigned long)esp_get_free_heap_size());
  int64_t lastDraw = 0;
  int64_t lastMemoryReport = 0;
  View previous;
  for (;;) {
    uint8_t c;
    while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) {
      int key = c == 'b' ? 0 : c == 'p' ? 1 : c == 'n' ? 2 : c == '-' ? 3 : c == '+' ? 4 : -1;
      if (key >= 0) {
        Command cmd = static_cast<Command>(key);
        xQueueSend(commands, &cmd, 0);
      }
    }
    pollTouch();
    if (esp_timer_get_time() - lastMemoryReport >= 10000000) {
      ESP_LOGI("memory", "heap free=%u, minimum=%u, largest internal=%u bytes",
               unsigned(esp_get_free_heap_size()), unsigned(esp_get_minimum_free_heap_size()),
               unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      lastMemoryReport = esp_timer_get_time();
    }
    if (esp_timer_get_time() - lastDraw > 250000) {
      View copy;
      xSemaphoreTake(viewLock, portMAX_DELAY);
      copy = view;
      xSemaphoreGive(viewLock);
      if (!lastDraw || strcmp(copy.title, previous.title) || strcmp(copy.status, previous.status) ||
          strcmp(copy.lyric, previous.lyric) || copy.ms / 1000 != previous.ms / 1000 ||
          copy.total != previous.total || copy.volume != previous.volume ||
          copy.playing != previous.playing)
        displayDraw(copy);
      previous = copy;
      lastDraw = esp_timer_get_time();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
