// Host stub of esphome/core/log.h (unit tests only).
#pragma once
#include "esp_log.h"
#define ESP_LOGCONFIG(tag, fmt, ...) test_log("C", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
