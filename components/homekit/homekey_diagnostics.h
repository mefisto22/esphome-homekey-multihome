#pragma once
#include <esphome/core/defines.h>
#ifdef USE_HOMEKEY
#include <esphome/core/component.h>
#ifdef USE_SENSOR
#include <esphome/components/sensor/sensor.h>
#endif
#ifdef USE_BINARY_SENSOR
#include <esphome/components/binary_sensor/binary_sensor.h>
#endif

namespace esphome {
namespace homekit {

// Publishes non-secret HomeKey store statistics. Never exposes key material.
class HomeKeyDiagnostics : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
#ifdef USE_SENSOR
  void set_homes_sensor(sensor::Sensor *s) { this->homes_ = s; }
  void set_issuers_sensor(sensor::Sensor *s) { this->issuers_ = s; }
  void set_endpoints_sensor(sensor::Sensor *s) { this->endpoints_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_provisioned_binary_sensor(binary_sensor::BinarySensor *s) { this->provisioned_ = s; }
  void set_active_home_binary_sensor(binary_sensor::BinarySensor *s) { this->active_home_ = s; }
#endif

 protected:
  uint32_t seen_generation_{0};
#ifdef USE_SENSOR
  sensor::Sensor *homes_{nullptr};
  sensor::Sensor *issuers_{nullptr};
  sensor::Sensor *endpoints_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *provisioned_{nullptr};
  binary_sensor::BinarySensor *active_home_{nullptr};
#endif
};

}  // namespace homekit
}  // namespace esphome
#endif
