#pragma once
#include "esphome/core/defines.h"
#if defined(USE_HOMEKEY) && defined(USE_BUTTON)
#include "esphome/components/button/button.h"

namespace esphome {
namespace homekit {

enum class HomeKeyButtonAction : uint8_t {
  // Reset HAP pairing only; HomeKey credentials of every Home are kept.
  RESET_PAIRING_KEEP_HOMEKEYS,
  // Reset HAP pairing and erase all HomeKey credentials.
  FACTORY_RESET_HOMEKEYS,
};

class HomeKeyButton : public button::Button {
 public:
  void set_action(HomeKeyButtonAction action) { this->action_ = action; }

 protected:
  void press_action() override;
  HomeKeyButtonAction action_{HomeKeyButtonAction::RESET_PAIRING_KEEP_HOMEKEYS};
};

}  // namespace homekit
}  // namespace esphome
#endif
