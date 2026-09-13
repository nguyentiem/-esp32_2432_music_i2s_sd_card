#pragma once
#include "sdkconfig.h"
#include <cstddef>
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID CONFIG_MUSIC_WIFI_SSID
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD CONFIG_MUSIC_WIFI_PASSWORD
#endif
#ifndef STREAM_URL
#define STREAM_URL CONFIG_MUSIC_STREAM_URL
#endif
constexpr size_t SOURCE_BUFFER = 24 * 1024;
constexpr size_t PREBUFFER = 12 * 1024;
constexpr size_t MAX_TRACKS = 128;
constexpr const char *SD_ROOT = "/sdcard";
