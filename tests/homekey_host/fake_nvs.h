#pragma once
#include <map>
#include <string>
#include <vector>
#include "nvs.h"

// In-memory NVS. The map persists across HomeKeyStore instances, which is how
// the tests simulate a reboot / OTA (NVS is not touched by an app OTA).
namespace fake_nvs {
using Flash = std::map<std::string, std::map<std::string, std::vector<uint8_t>>>;
Flash &flash();
void reset();
// Next N nvs_set_blob/nvs_set_str calls fail with ESP_ERR_NVS_NOT_ENOUGH_SPACE.
void fail_next_writes(int n);
int write_count();
bool has(const std::string &ns, const std::string &key);
std::vector<uint8_t> get(const std::string &ns, const std::string &key);
void put(const std::string &ns, const std::string &key, const std::vector<uint8_t> &v);
}  // namespace fake_nvs
