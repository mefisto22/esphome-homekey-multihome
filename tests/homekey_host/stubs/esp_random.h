#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
inline void esp_fill_random(void *buf, size_t len) {
  for (size_t i = 0; i < len; i++) static_cast<uint8_t *>(buf)[i] = static_cast<uint8_t>(rand());
}
inline uint32_t esp_random() { return static_cast<uint32_t>(rand()); }
