// Host stub of ESP-IDF esp_log.h (unit tests only). Per-tag levels behave like
// ESP-IDF; the default is VERBOSE, i.e. the worst case of a user who raised the
// ESP-IDF log level all the way up.
#pragma once
#include <cstdio>
#include "test_log.h"
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
esp_log_level_t esp_log_level_get(const char *tag);
void esp_log_level_set(const char *tag, esp_log_level_t level);
bool test_log_enabled(const char *tag, esp_log_level_t level);
#define TEST_LOG_AT(lvl, name, tag, fmt, ...) \
  do { if (test_log_enabled(tag, lvl)) test_log(name, tag, fmt __VA_OPT__(, ) __VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) TEST_LOG_AT(ESP_LOG_ERROR, "E", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) TEST_LOG_AT(ESP_LOG_WARN, "W", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) TEST_LOG_AT(ESP_LOG_INFO, "I", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) TEST_LOG_AT(ESP_LOG_DEBUG, "D", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) TEST_LOG_AT(ESP_LOG_VERBOSE, "V", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOG_BUFFER_HEX_LEVEL(...)
#define ESP_LOG_BUFFER_HEX(...)
