#include "homekey_library.h"
#ifdef USE_HOMEKEY
#include <esp_log.h>
#include <HK_HomeKit.h>
#include <hkAuthContext.h>

namespace esphome {
namespace homekit {

void homekey_install_library_hooks(HomeKeyStore &store) {
#ifndef HOMEKEY_INSECURE_DEBUG_LOGGING
  // The HomeKey library logs reader private keys, ECDH shared secrets and
  // session keys at DEBUG/VERBOSE. Never let those through in production, even
  // if the ESP-IDF log level is raised. Build with
  // -DHOMEKEY_INSECURE_DEBUG_LOGGING to opt in for protocol debugging.
  for (const char *tag : {"HK_HomeKit", "HKAuthCtx", "HKStdAuth", "HKFastAuth", "HKAttestAuth", "DigitalKeySC",
                          "ISO18013_SC", "CCUtils", "X963KDF", "simple_tlv", "NDEFMessage"}) {
    if (esp_log_level_get(tag) > ESP_LOG_INFO)
      esp_log_level_set(tag, ESP_LOG_INFO);
  }
#endif
  store.set_provisioner([](readerData_t &profile, std::vector<uint8_t> &tlv, nvs_handle_t scratch) {
    // The library persists its own single-profile copy through this handle;
    // it points at a scratch namespace that the store wipes right after.
    nvs_handle_t handle = scratch;
    HK_HomeKit ctx(profile, handle, hk_store::NVS_KEY_LEGACY, tlv);
    return ctx.processResult();
  });
  store.set_authenticator([](readerData_t &profile, const HKTransceive &nfc, nvs_handle_t readonly_handle) {
    // Read-only handle: the library's own "save READERDATA" attempt after a
    // STANDARD/ATTESTATION flow fails harmlessly; the store persists instead.
    nvs_handle_t handle = readonly_handle;
    HKAuthenticationContext authCtx(nfc, profile, handle);
    return authCtx.authenticate(KeyFlow(kFlowFAST));
  });
}

}  // namespace homekit
}  // namespace esphome
#endif
