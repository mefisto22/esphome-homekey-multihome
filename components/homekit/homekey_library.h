#pragma once
#include <esphome/core/defines.h>
#ifdef USE_HOMEKEY
#include "homekey_store.h"

namespace esphome {
namespace homekit {

// Connects the HomeKeyStore to the pinned HomeKey protocol library
// (HK_HomeKit for provisioning, HKAuthenticationContext for NFC) and clamps
// the library's log levels so key material is never logged. Idempotent.
void homekey_install_library_hooks(HomeKeyStore &store);

}  // namespace homekit
}  // namespace esphome
#endif
