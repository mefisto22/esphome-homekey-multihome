#include "homekey_button.h"
#if defined(USE_HOMEKEY) && defined(USE_BUTTON)
#include "esphome/core/log.h"
#include "../homekey_store.h"
#include <hap.h>

namespace esphome {
namespace homekit {

static const char *const TAG = "homekey.button";

void HomeKeyButton::press_action() {
  auto &store = HomeKeyStore::get();
  store.begin(hap_get_paired_controller_count());
  switch (this->action_) {
    case HomeKeyButtonAction::RESET_PAIRING_KEEP_HOMEKEYS:
      if (!store.can_enroll_another_home()) {
        ESP_LOGE(TAG, "Not resetting: no room for another Home's HomeKey reader profile (max %u) or the store is "
                      "read-only. Use the HomeKey factory reset instead.",
                 (unsigned) hk_store::MAX_PROFILES);
        return;
      }
      // hap_reset_pairings() erases HAP controllers + accessory identity and
      // reboots ~2 s later from the HAP task. HomeKey data is in a separate NVS
      // namespace and is not touched by it.
      if (hap_reset_pairings() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "HomeKit pairing reset failed (HAP not running?)");
        return;
      }
      ESP_LOGW(TAG, "Resetting HomeKit pairing; HomeKeys of all Homes are kept. Rebooting, then pair the next Home.");
      // Also applied automatically at boot when no controller is paired.
      if (!store.archive_active_home())
        ESP_LOGW(TAG, "Archiving now failed; it is re-applied after the reboot");
      break;
    case HomeKeyButtonAction::FACTORY_RESET_HOMEKEYS:
      // Queue the HAP reset first: if it cannot run, keep the credentials so
      // that HAP pairings and HomeKey data never get out of sync.
      if (hap_reset_pairings() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "HomeKit pairing reset failed (HAP not running?); HomeKey data NOT erased");
        return;
      }
      ESP_LOGW(TAG, "FACTORY RESET: erasing HomeKit pairings and ALL HomeKey credentials, then rebooting");
      store.erase_all();
      break;
  }
}

}  // namespace homekit
}  // namespace esphome
#endif
