#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif
#ifndef STREAM_URL
#define STREAM_URL ""
#endif
// Calibrate these raw touch limits for your panel if necessary.
constexpr int TOUCH_X_MIN = 200, TOUCH_X_MAX = 3900;
constexpr int TOUCH_Y_MIN = 200, TOUCH_Y_MAX = 3900;
constexpr size_t SOURCE_BUFFER = 24 * 1024;
constexpr size_t PREBUFFER = 12 * 1024;
constexpr size_t MAX_TRACKS = 128;
