# HomeKey Multi-Home for ESPHome

**English** | [Magyar](README.hu.md)

An Apple HomeKey NFC lock reader for ESP32, built on ESPHome. iPhones from **several independent Apple Homes** can unlock the same reader, without Home sharing.

> This is a separate project built on the HomeKit components of [rednblkx/HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome). The original project supports one Apple Home; this version reworks HomeKey handling so the keys of several Homes stay valid at the same time. It is not affiliated with Apple.

## What is different

Every Apple Home writes its own reader key to the lock. The original implementation stored only one, so setting up a second Home broke the first. Here:

- **A separate key per Home.** The iPhone from Home A and the iPhone from Home B both unlock, without a shared Home.
- **New Homes without a firmware change.** Press a button ("Reset HomeKit pairing (keep HomeKeys)"), then pair the new Home.
- **Existing HomeKeys are kept** across reboots, power loss, OTA updates and pairing resets.
- **Two kinds of reset:** reset only the HomeKit pairing (HomeKeys are kept), or a full factory reset (everything is erased).
- **Diagnostics in Home Assistant:** number of Homes, users and devices. No key material is exposed.
- **Security:** private keys are never logged, corrupted stored data does not cause a reboot loop, malformed data from a malicious NFC device does not crash the reader, and data in the old format is migrated automatically.
- **Setup code** can be changed at runtime, without recompiling.

Only one Home controls the lock through HomeKit at a time. This is a limitation of the HomeKit protocol; over NFC, every enrolled Home unlocks.

## Hardware

- ESP32 (tested: ESP32 DevKit V1 / `esp32dev`), 4 MB flash, ESP-IDF framework
- PN532 NFC module **in SPI mode** (`pn532_spi`), 3.3 V
- HomeKey-capable iPhone or Apple Watch

Example wiring: SCK GPIO18, MISO GPIO19, MOSI GPIO23, CS GPIO5.

## Quick start

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/mefisto22/esphome-homekey-multihome
      ref: main
    components: [homekit, homekit_base, pn532, pn532_spi]

spi:
  clk_pin: GPIO18
  miso_pin: GPIO19
  mosi_pin: GPIO23

pn532_spi:
  id: nfc_spi_module
  cs_pin: GPIO5
  update_interval: 100ms

lock:
  - platform: template
    id: front_door
    name: "Front door"
    optimistic: true

homekit_base:
  setup_code: "159-35-728"

homekit:
  lock:
    - id: front_door
      nfc_id: nfc_spi_module
      on_hk_success:
        - lock.unlock: front_door

button:
  - platform: homekit
    reset_pairing_keep_homekeys:
      name: "Reset HomeKit pairing (keep HomeKeys)"
    factory_reset_homekeys:
      name: "Factory reset HomeKit + HomeKeys"
```

Complete example, with all new entities: [examples/homekey-virtual-lock.yaml](examples/homekey-virtual-lock.yaml), a reader with its own virtual lock that every HomeKey toggles.

## Enrolling two Homes

1. Pair the device with **Home A** and add the HomeKey to Wallet.
2. Press **"Reset HomeKit pairing (keep HomeKeys)"**. The device reboots and becomes pairable again. Do **not** remove it from Home A.
3. Pair with **Home B** and add the HomeKey.
4. Both phones unlock. Further Homes are added the same way (up to 8).

> **Important:** every enrolled Home can unlock with HomeKey, but through the Home app only the currently paired Home controls the device. This applies to everything under `homekit:` (e.g. a light too); in the other Homes these show "No Response". Only what is listed under `homekit:` appears in Apple Home at all. To control something from every Home, use Home Assistant's HomeKit Bridge. Details: [What appears in Apple Home](docs/homekey-multi-home.md#what-appears-in-apple-home-and-which-home-can-control-it).

Details, persistence table, recovery and security notes: [docs/homekey-multi-home.md](docs/homekey-multi-home.md).

## Documentation

- [docs/homekey-multi-home.md](docs/homekey-multi-home.md): how multi-Home HomeKey works, enrolment, OTA, factory reset
- [docs/components.md](docs/components.md): reference for all components and configuration options (besides HomeKey also lights, switches, sensors and fans)

Every document is also available in Hungarian; switch the language at the top of each page.

## Tests

```bash
./tests/homekey_host/run_tests.sh
```

Three test suites run on the host: the HomeKey store logic, the real HomeKey library against a simulated phone that uses real cryptography, and malformed or malicious NFC input that must not crash the device. GitHub CI runs them on every PR.

## Status

The HomeKey protocol is reverse engineered; Apple can change it at any time. The multi-Home behaviour is verified with tests and a simulated phone; there is no long-term experience with real iPhones yet.

## Credits and license

- [rednblkx/HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome) and [HK-HomeKit-Lib](https://github.com/rednblkx/HK-HomeKit-Lib) ([@rednblkx](https://github.com/rednblkx)): the original HomeKit and HomeKey components
- [@kormax](https://github.com/kormax): [research](https://github.com/kormax/apple-home-key) of the HomeKey NFC protocol and ECP
- [@kupa22](https://github.com/kupa22): [documentation](https://github.com/kupa22/apple-homekey) of the HAP part of HomeKey
- [ESPHome](https://github.com/esphome/esphome) (PN532 component) and [Espressif](https://github.com/espressif/esp-homekit-sdk) (esp-homekit-sdk)

The `homekit` and `homekit_base` components are licensed under GPL-3.0, the `pn532` and `pn532_spi` components under the ESPHome license; see the `LICENSE` file in each folder of the [components](components) directory.
