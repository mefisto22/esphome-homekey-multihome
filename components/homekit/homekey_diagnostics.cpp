#include "homekey_diagnostics.h"
#ifdef USE_HOMEKEY
#include "homekey_store.h"
#include <hap.h>

namespace esphome {
namespace homekit {

void HomeKeyDiagnostics::setup() { HomeKeyStore::get().begin(hap_get_paired_controller_count()); }

void HomeKeyDiagnostics::loop() {
  auto &store = HomeKeyStore::get();
  uint32_t gen = store.generation();
  if (gen == this->seen_generation_)
    return;
  this->seen_generation_ = gen;
  HomeKeyStats st = store.stats();
#ifdef USE_SENSOR
  if (this->homes_ != nullptr)
    this->homes_->publish_state(st.profiles);
  if (this->issuers_ != nullptr)
    this->issuers_->publish_state(st.issuers);
  if (this->endpoints_ != nullptr)
    this->endpoints_->publish_state(st.endpoints);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->provisioned_ != nullptr)
    this->provisioned_->publish_state(st.profiles > 0);
  if (this->active_home_ != nullptr)
    this->active_home_->publish_state(st.active_provisioned);
#endif
}

}  // namespace homekit
}  // namespace esphome
#endif
