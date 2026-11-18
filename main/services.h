#pragma once
#include <cstdint>
#include <vector>

enum class StandbyMode : uint8_t { ClockWeather = 0, PcStats = 1 };
enum class Language : uint8_t { Vietnamese = 0, English = 1 };

struct LunarDate {
  int day;
  int month;
  int year;
  bool leap;
};

struct WeatherInfo {
  int tempC = 28;
  int code = 2;
  bool valid = false;
  char descVi[32] = "Co may";
  char descEn[32] = "Partly Cloudy";
};

// Convert Gregorian date to Vietnamese Lunar date (UTC+7).
LunarDate solarToLunar(int dd, int mm, int yy);

// Initialize SNTP for UTC+7 (Vietnam).
void servicesInitSntp();

// Start background weather polling task (Open-Meteo HTTP).
void servicesStartWeatherTask();

// Get latest weather snapshot (thread-safe).
WeatherInfo servicesGetWeather();

// NVS persistence for user settings.
void servicesLoadSettings(StandbyMode &mode, Language &lang);
void servicesSaveSettings(StandbyMode mode, Language lang);

struct WifiApInfo {
  char ssid[33];
  int8_t rssi;
  bool open;
};

void servicesStartWifiScan();
bool servicesIsWifiScanActive();
uint32_t servicesGetWifiScanSeq();
std::vector<WifiApInfo> servicesGetWifiScanResults(bool &scanning);
void servicesConnectWifi(const char *ssid, const char *password);
