#include "fake_nvs.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "test_log.h"

namespace fake_nvs {
static Flash g_flash;
static std::map<nvs_handle_t, std::pair<std::string, nvs_open_mode_t>> g_handles;
static nvs_handle_t g_next = 1;
static int g_fail = 0;
static int g_writes = 0;
Flash &flash() { return g_flash; }
void reset() {
  g_flash.clear();
  g_handles.clear();
  g_fail = 0;
  g_writes = 0;
}
void fail_next_writes(int n) { g_fail = n; }
int write_count() { return g_writes; }
bool has(const std::string &ns, const std::string &key) {
  auto it = g_flash.find(ns);
  return it != g_flash.end() && it->second.count(key) > 0;
}
std::vector<uint8_t> get(const std::string &ns, const std::string &key) { return g_flash[ns][key]; }
void put(const std::string &ns, const std::string &key, const std::vector<uint8_t> &v) { g_flash[ns][key] = v; }
}  // namespace fake_nvs

using namespace fake_nvs;

const char *esp_err_to_name(esp_err_t code) {
  switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_READ_ONLY: return "ESP_ERR_NVS_READ_ONLY";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_NVS_INVALID_HANDLE: return "ESP_ERR_NVS_INVALID_HANDLE";
    default: return "ESP_ERR_OTHER";
  }
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
  if (mode == NVS_READONLY && g_flash.find(ns) == g_flash.end())
    return ESP_ERR_NVS_NOT_FOUND;
  g_flash[ns];  // RW open creates the namespace
  *out = g_next++;
  g_handles[*out] = {ns, mode};
  return ESP_OK;
}

static bool lookup(nvs_handle_t h, std::string &ns, nvs_open_mode_t &mode) {
  auto it = g_handles.find(h);
  if (it == g_handles.end())
    return false;
  ns = it->second.first;
  mode = it->second.second;
  return true;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
  std::string ns;
  nvs_open_mode_t mode;
  if (!lookup(h, ns, mode))
    return ESP_ERR_NVS_INVALID_HANDLE;
  auto &m = g_flash[ns];
  auto it = m.find(key);
  if (it == m.end())
    return ESP_ERR_NVS_NOT_FOUND;
  if (out == nullptr) {
    *len = it->second.size();
    return ESP_OK;
  }
  if (*len < it->second.size())
    return ESP_ERR_NVS_INVALID_LENGTH;
  memcpy(out, it->second.data(), it->second.size());
  *len = it->second.size();
  return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len) {
  std::string ns;
  nvs_open_mode_t mode;
  if (!lookup(h, ns, mode))
    return ESP_ERR_NVS_INVALID_HANDLE;
  if (mode == NVS_READONLY)
    return ESP_ERR_NVS_READ_ONLY;
  if (g_fail > 0) {
    g_fail--;
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  }
  g_writes++;
  const uint8_t *p = static_cast<const uint8_t *>(value);
  g_flash[ns][key] = std::vector<uint8_t>(p, p + len);
  return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
  return nvs_get_blob(h, key, out, len);
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
  return nvs_set_blob(h, key, value, strlen(value) + 1);
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
  std::string ns;
  nvs_open_mode_t mode;
  if (!lookup(h, ns, mode))
    return ESP_ERR_NVS_INVALID_HANDLE;
  if (mode == NVS_READONLY)
    return ESP_ERR_NVS_READ_ONLY;
  return g_flash[ns].erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_all(nvs_handle_t h) {
  std::string ns;
  nvs_open_mode_t mode;
  if (!lookup(h, ns, mode))
    return ESP_ERR_NVS_INVALID_HANDLE;
  if (mode == NVS_READONLY)
    return ESP_ERR_NVS_READ_ONLY;
  g_flash[ns].clear();
  return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) {
  return g_handles.count(h) ? ESP_OK : ESP_ERR_NVS_INVALID_HANDLE;
}

void nvs_close(nvs_handle_t h) { g_handles.erase(h); }

// ---- log capture ----
#include <map>
#include "esp_log.h"
static std::map<std::string, esp_log_level_t> g_levels;
esp_log_level_t esp_log_level_get(const char *tag) {
  auto it = g_levels.find(tag);
  return it == g_levels.end() ? ESP_LOG_VERBOSE : it->second;
}
void esp_log_level_set(const char *tag, esp_log_level_t level) { g_levels[tag] = level; }
bool test_log_enabled(const char *tag, esp_log_level_t level) { return level <= esp_log_level_get(tag); }
static std::string g_log;
void test_log(const char *level, const char *tag, const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log += std::string(level) + " [" + tag + "] " + buf + "\n";
  if (getenv("HK_TEST_VERBOSE"))
    printf("  %s [%s] %s\n", level, tag, buf);
}
const std::string &test_log_buffer() { return g_log; }
void test_log_clear() { g_log.clear(); }
