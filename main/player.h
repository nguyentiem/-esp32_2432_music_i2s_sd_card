#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "services.h"
#include <cstdint>
#include <string>
#include <vector>
enum class Command { Previous, Toggle, Next, Quieter, Louder, Pause, Resume };
enum class Screen : uint8_t {
  Standby,
  Menu,
  Player,
  Settings,
  WifiSettings,
  WifiKeyboard,
  LanguageSettings,
  StandbyModeSettings
};
struct View {
  char title[192] = "CYD MUSIC";
  char lyric[192] = "";
  char status[40] = "Starting...";
  uint32_t ms = 0, total = 0;
  int volume = 35;
  bool playing = false;
  Screen screen = Screen::Standby;
  StandbyMode standbyMode = StandbyMode::ClockWeather;
  Language language = Language::Vietnamese;
  bool wifiConnected = false;
  char wifiIp[20] = "0.0.0.0";
  uint32_t uiTick = 0;
  uint32_t uiDirty = 0;
};
extern QueueHandle_t commands;
extern SemaphoreHandle_t viewLock;
extern View view;
extern std::vector<std::string> tracks;
void playerTask(void *);
void displayInit();
void displayDraw(const View &state);
void pollTouch();
void requestWifiReconnect();
