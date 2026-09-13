cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32 CACHE STRING "CYD uses the original ESP32")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyd_music)
