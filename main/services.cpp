#include "services.h"
#include "player.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {
constexpr char kTag[] = "services";
constexpr char kNvsNamespace[] = "music_ui";
SemaphoreHandle_t weatherLock = nullptr;
WeatherInfo currentWeather;

// --- Vietnamese Lunar Calendar (Ho Ngoc Duc algorithm, UTC+7) ---
int jdFromDate(int dd, int mm, int yy) {
  int a = (14 - mm) / 12;
  int y = yy + 4800 - a;
  int m = mm + 12 * a - 3;
  int jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
  if (jd < 2299161)
    jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - 32083;
  return jd;
}

double newMoon(int k) {
  double T = k / 1236.85;
  double T2 = T * T;
  double T3 = T2 * T;
  double dr = M_PI / 180.0;
  double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
  Jd1 += 0.00033 * sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
  double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
  double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
  double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
  double C1 = (0.1734 - 0.000393 * T) * sin(M * dr) + 0.0021 * sin(2 * dr * M);
  C1 -= 0.4068 * sin(Mpr * dr) + 0.0161 * sin(dr * 2 * Mpr) - 0.0004 * sin(dr * 3 * Mpr);
  C1 += 0.0104 * sin(dr * 2 * F) - 0.0051 * sin(dr * (M + Mpr)) - 0.0074 * sin(dr * (M - Mpr));
  C1 += 0.0004 * sin(dr * (2 * F + M)) - 0.0004 * sin(dr * (2 * F - M)) - 0.0006 * sin(dr * (2 * F + Mpr));
  C1 += 0.0010 * sin(dr * (2 * F - Mpr)) + 0.0005 * sin(dr * (2 * Mpr + M));
  double deltat = (T < -11)
                      ? (0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3)
                      : (-0.000278 + 0.000265 * T + 0.000262 * T2);
  return Jd1 + C1 - deltat;
}

int getNewMoonDay(int k, double timeZone) {
  return int(floor(newMoon(k) + 0.5 + timeZone / 24.0));
}

double sunLongitude(double jdn) {
  double T = (jdn - 2451545.0) / 36525.0;
  double T2 = T * T;
  double dr = M_PI / 180.0;
  double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
  double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
  double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * sin(dr * M);
  DL += (0.019993 - 0.000101 * T) * sin(dr * 2 * M) + 0.000290 * sin(dr * 3 * M);
  double L = (L0 + DL) * dr;
  L -= M_PI * 2 * floor(L / (M_PI * 2));
  return L;
}

int getSunLongitude(int dayNumber, double timeZone) {
  return int(floor(sunLongitude(dayNumber - 0.5 - timeZone / 24.0) / M_PI * 6));
}

int getLunarMonth11(int yy, double timeZone) {
  int off = jdFromDate(31, 12, yy) - 2415021;
  int k = int(floor(off / 29.530588853));
  int nm = getNewMoonDay(k, timeZone);
  if (getSunLongitude(nm, timeZone) >= 9)
    nm = getNewMoonDay(k - 1, timeZone);
  return nm;
}

int getLeapMonthOffset(int a11, double timeZone) {
  int k = int(floor((a11 - 2415021.076998695) / 29.530588853 + 0.5));
  int last = 0, arc = 0, i = 1;
  arc = getSunLongitude(getNewMoonDay(k + i, timeZone), timeZone);
  do {
    last = arc;
    i++;
    arc = getSunLongitude(getNewMoonDay(k + i, timeZone), timeZone);
  } while (arc != last && i < 14);
  return i - 1;
}

void describeWeather(int code, char *vi, size_t viLen, char *en, size_t enLen) {
  if (code == 0) {
    snprintf(vi, viLen, "Trời quang");
    snprintf(en, enLen, "Clear Sky");
  } else if (code <= 3) {
    snprintf(vi, viLen, "Có mây");
    snprintf(en, enLen, "Partly Cloudy");
  } else if (code <= 48) {
    snprintf(vi, viLen, "Sương mù");
    snprintf(en, enLen, "Foggy");
  } else if (code <= 67) {
    snprintf(vi, viLen, "Có mưa");
    snprintf(en, enLen, "Rainy");
  } else if (code <= 77) {
    snprintf(vi, viLen, "Có tuyết");
    snprintf(en, enLen, "Snow");
  } else if (code <= 82) {
    snprintf(vi, viLen, "Mưa rào");
    snprintf(en, enLen, "Rain Showers");
  } else {
    snprintf(vi, viLen, "Có dông");
    snprintf(en, enLen, "Thunderstorm");
  }
}

void fetchWeatherOnce() {
  const char *url =
      "http://api.open-meteo.com/v1/forecast?latitude=21.0285&longitude=105.8542&current=temperature_2m,weather_code&timezone=auto";
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client)
    return;

  if (esp_http_client_open(client, 0) == ESP_OK) {
    int contentLen = esp_http_client_fetch_headers(client);
    if (contentLen >= 0) {
      char buf[512];
      int total = 0;
      int r = 0;
      while (total < int(sizeof(buf) - 1) && (r = esp_http_client_read(client, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += r;
      buf[total] = '\0';

      const char *cur = strstr(buf, "\"current\"");
      if (cur) {
        const char *tPos = strstr(cur, "\"temperature_2m\":");
        const char *cPos = strstr(cur, "\"weather_code\":");
        if (tPos && cPos) {
          float temp = strtof(tPos + strlen("\"temperature_2m\":"), nullptr);
          int code = int(strtol(cPos + strlen("\"weather_code\":"), nullptr, 10));
          if (weatherLock)
            xSemaphoreTake(weatherLock, portMAX_DELAY);
          currentWeather.tempC = int(lroundf(temp));
          currentWeather.code = code;
          currentWeather.valid = true;
          describeWeather(code, currentWeather.descVi, sizeof(currentWeather.descVi),
                          currentWeather.descEn, sizeof(currentWeather.descEn));
          if (weatherLock)
            xSemaphoreGive(weatherLock);
          ESP_LOGI(kTag, "Weather updated: %d C, code=%d", currentWeather.tempC, currentWeather.code);
        }
      }
    }
    esp_http_client_close(client);
  }
  esp_http_client_cleanup(client);
}

void weatherTask(void *) {
  bool sntpStarted = false;
  for (;;) {
    bool connected = false;
    if (viewLock && xSemaphoreTake(viewLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      connected = view.wifiConnected;
      xSemaphoreGive(viewLock);
    }
    if (!connected) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!sntpStarted) {
      servicesInitSntp();
      sntpStarted = true;
    }
    fetchWeatherOnce();
    vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
  }
}
} // namespace

LunarDate solarToLunar(int dd, int mm, int yy) {
  constexpr double tz = 7.0;
  int dayNumber = jdFromDate(dd, mm, yy);
  int k = int(floor((dayNumber - 2415021.076998695) / 29.530588853));
  int monthStart = getNewMoonDay(k + 1, tz);
  if (monthStart > dayNumber)
    monthStart = getNewMoonDay(k, tz);
  int a11 = getLunarMonth11(yy, tz);
  int b11 = a11;
  int lunarYear;
  if (a11 >= monthStart) {
    lunarYear = yy;
    a11 = getLunarMonth11(yy - 1, tz);
  } else {
    lunarYear = yy + 1;
    b11 = getLunarMonth11(yy + 1, tz);
  }
  int lunarDay = dayNumber - monthStart + 1;
  int diff = int(floor((monthStart - a11) / 29.0));
  bool lunarLeap = false;
  int lunarMonth = diff + 11;
  if (b11 - a11 > 365) {
    int leapMonthDiff = getLeapMonthOffset(a11, tz);
    if (diff >= leapMonthDiff) {
      lunarMonth = diff + 10;
      if (diff == leapMonthDiff)
        lunarLeap = true;
    }
  }
  if (lunarMonth > 12)
    lunarMonth -= 12;
  if (lunarMonth >= 11 && diff < 4)
    lunarYear -= 1;
  return {lunarDay, lunarMonth, lunarYear, lunarLeap};
}

void servicesInitSntp() {
  setenv("TZ", "ICT-7", 1);
  tzset();
  if (esp_sntp_enabled())
    return;
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
  ESP_LOGI(kTag, "SNTP initialized (UTC+7)");
}

void servicesStartWeatherTask() {
  if (!weatherLock)
    weatherLock = xSemaphoreCreateMutex();
  xTaskCreate(weatherTask, "weather", 8192, nullptr, 2, nullptr);
}

WeatherInfo servicesGetWeather() {
  WeatherInfo copy;
  if (weatherLock)
    xSemaphoreTake(weatherLock, portMAX_DELAY);
  copy = currentWeather;
  if (weatherLock)
    xSemaphoreGive(weatherLock);
  return copy;
}

void servicesLoadSettings(StandbyMode &mode, Language &lang) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
    uint8_t m = 0, l = 0;
    if (nvs_get_u8(h, "standby", &m) == ESP_OK && m <= 1)
      mode = static_cast<StandbyMode>(m);
    if (nvs_get_u8(h, "lang", &l) == ESP_OK && l <= 1)
      lang = static_cast<Language>(l);
    nvs_close(h);
  }
}

void servicesSaveSettings(StandbyMode mode, Language lang) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "standby", static_cast<uint8_t>(mode));
    nvs_set_u8(h, "lang", static_cast<uint8_t>(lang));
    nvs_commit(h);
    nvs_close(h);
  }
}

namespace {
SemaphoreHandle_t wifiScanLock = nullptr;
std::vector<WifiApInfo> wifiScanResults;
bool wifiScanning = false;
uint32_t wifiScanSeq = 0;

void wifiScanTask(void *) {
  // Pause any pending reconnect so esp_wifi_scan_start does not fail with ESP_ERR_WIFI_STATE.
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));

  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  esp_err_t err = esp_wifi_scan_start(&cfg, true);
  ESP_LOGI(kTag, "WiFi scan finished: %s", esp_err_to_name(err));
  std::vector<WifiApInfo> found;
  if (err == ESP_OK) {
    uint16_t count = 16;
    wifi_ap_record_t records[16];
    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
      for (uint16_t i = 0; i < count; ++i) {
        if (records[i].ssid[0] == '\0')
          continue;
        const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
        auto it = std::find_if(found.begin(), found.end(),
                               [&](const WifiApInfo &ap) { return strcmp(ap.ssid, ssid) == 0; });
        if (it != found.end()) {
          if (records[i].rssi > it->rssi) {
            it->rssi = records[i].rssi;
            it->open = (records[i].authmode == WIFI_AUTH_OPEN);
          }
          continue;
        }
        WifiApInfo ap{};
        snprintf(ap.ssid, sizeof(ap.ssid), "%s", ssid);
        ap.rssi = records[i].rssi;
        ap.open = (records[i].authmode == WIFI_AUTH_OPEN);
        found.push_back(ap);
      }
      std::sort(found.begin(), found.end(),
                [](const WifiApInfo &a, const WifiApInfo &b) { return a.rssi > b.rssi; });
    }
  }
  if (wifiScanLock)
    xSemaphoreTake(wifiScanLock, portMAX_DELAY);
  wifiScanResults = std::move(found);
  wifiScanning = false;
  ++wifiScanSeq;
  if (wifiScanLock)
    xSemaphoreGive(wifiScanLock);
  vTaskDelete(nullptr);
}
} // namespace

void servicesStartWifiScan() {
  if (!wifiScanLock)
    wifiScanLock = xSemaphoreCreateMutex();
  xSemaphoreTake(wifiScanLock, portMAX_DELAY);
  if (wifiScanning) {
    xSemaphoreGive(wifiScanLock);
    return;
  }
  wifiScanning = true;
  ++wifiScanSeq;
  xSemaphoreGive(wifiScanLock);
  xTaskCreate(wifiScanTask, "wifi_scan", 4096, nullptr, 2, nullptr);
}

bool servicesIsWifiScanActive() {
  if (!wifiScanLock)
    return false;
  xSemaphoreTake(wifiScanLock, portMAX_DELAY);
  bool active = wifiScanning;
  xSemaphoreGive(wifiScanLock);
  return active;
}

uint32_t servicesGetWifiScanSeq() {
  if (!wifiScanLock)
    return 0;
  xSemaphoreTake(wifiScanLock, portMAX_DELAY);
  uint32_t seq = wifiScanSeq;
  xSemaphoreGive(wifiScanLock);
  return seq;
}

std::vector<WifiApInfo> servicesGetWifiScanResults(bool &scanning) {
  if (!wifiScanLock) {
    scanning = false;
    return {};
  }
  xSemaphoreTake(wifiScanLock, portMAX_DELAY);
  scanning = wifiScanning;
  auto copy = wifiScanResults;
  xSemaphoreGive(wifiScanLock);
  return copy;
}

void servicesConnectWifi(const char *ssid, const char *password) {
  wifi_config_t cfg = {};
  snprintf(reinterpret_cast<char *>(cfg.sta.ssid), sizeof(cfg.sta.ssid), "%s", ssid ? ssid : "");
  snprintf(reinterpret_cast<char *>(cfg.sta.password), sizeof(cfg.sta.password), "%s", password ? password : "");
  cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
  esp_wifi_disconnect();
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  ESP_LOGI(kTag, "Connecting WiFi '%s': set_config=%s", ssid ? ssid : "", esp_err_to_name(err));
  esp_wifi_connect();
}
