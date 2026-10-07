# HomeKey with multiple independent Apple Homes

**English** | [Magyar](homekey-multi-home.hu.md)

Two (or more) people whose iPhones are in **different Apple Homes** can each get a working HomeKey for the same reader, without Home sharing. Only one Home controls the lock through HomeKit at a time, but the HomeKeys of every enrolled Home unlock it over NFC.

## What was the problem

Every Apple Home writes its **own reader private key** to the lock. The iPhone looks for a hash of this key (the reader group identifier) in the NFC polling frame and during authentication, and verifies the reader's signature with the key of **its own Home**.

The previous firmware stored only one reader key. When a second Home set up HomeKey, it overwrote the first Home's key, and the first Home's iPhones stopped working. And when the accessory was removed from a Home, all HomeKey data was erased.

## How the fix works

- **One "reader profile" per Home.** A profile holds that Home's reader key, its users (issuers) and their devices (iPhone, Watch).
  - The profile of the currently paired Home is **active**: all HomeKit provisioning goes there.
  - Profiles of earlier Homes are **archived**: HomeKit traffic does not modify them, but they keep unlocking over NFC.
- **On a tap, the reader tries every Home.** The PN532 alternates the wake-up (ECP) frame between Homes, so each iPhone wakes up in Express Mode for its own Home. The Home that woke the phone is tried first. If that fails, the reader restarts the transaction with the next Home, without signalling an error to the phone in between.
- **No duplicates.** The same user, device or re-paired Home is not created twice; it is merged.
- At most **8 Homes** can be stored.

## Enrolling Phone A and Phone B

1. Install the new firmware via OTA. Existing HomeKey data is migrated to the new format automatically.
2. Pair the accessory with **Home A** (Phone A) using the setup code, add the HomeKey to Wallet, and try it on the reader.
3. Press the **"Reset HomeKit pairing (keep HomeKeys)"** button. The device reboots and becomes pairable again.
   - In Home A the lock will show "No Response"; this is expected.
   - **Do not remove the accessory from Home A!** Removing it revokes Home A's HomeKeys, and the Home app may also delete the Wallet key.
4. Pair with **Home B** (Phone B) using the same setup code, add the HomeKey, and try it.
5. From now on both phones unlock the reader, also after reboots, power loss and OTA updates. As a check, the "HomeKey Homes" sensor reads 2.

A Home C can be added the same way later (steps 3 and 4). No firmware change is needed.

## What appears in Apple Home, and which Home can control it

**Only what is listed in the YAML `homekit:` section appears in Apple Home** (`light`, `lock`, `switch`, `fan`, `climate`, `sensor`). Every other ESPHome entity is visible only in Home Assistant: the buttons, the HomeKey diagnostic sensors, text sensors, and anything else that is not under `homekit:`. This holds even for entities with `platform: homekit`; that only means the `homekit` component provides them, they are not exposed to HomeKit. The device itself appears as a HomeKit bridge, shown in the accessory settings of the Home app, not as a separate tile.

**Through HomeKit, only the currently paired (active) Home controls the device.** This applies to every HomeKit entity of the device, not just the lock. If you add, for example, a light under `homekit:`, only the phones of the active Home can switch it. In the earlier, archived Homes both the light and the lock show "No Response". **Only HomeKey (tapping the Wallet key) works across Homes.**

| | Active Home (the most recently paired) | Archived Homes (paired earlier) |
|---|---|---|
| Entities under `homekit:` (light, switch, lock, ...) in the Home app | ✅ from every phone and member of that Home | ❌ "No Response" |
| Lock state in the Home app | ✅ | ❌ "No Response" |
| HomeKey in Wallet (NFC tap on the reader) | ✅ | ✅ |
| In Home Assistant (all entities) | ✅ | ✅ |

The reason: "Reset HomeKit pairing (keep HomeKeys)" removes the previous Home's HomeKit pairing and keeps only its HomeKey keys for the NFC reader. Under the HAP protocol an accessory is controlled by one Home at a time.

**To control an entity from every Home** (a light, the HomeKey virtual lock, a real lock, ...), do not put it in the ESP `homekit:` section; use the Home Assistant [HomeKit Bridge](https://www.home-assistant.io/integrations/homekit/) integration instead. Home Assistant can run several HomeKit Bridge instances, each paired with a different Home, and each can contain the same entities. Keep only the HomeKey lock in the ESP `homekit:` section, because the Wallet key needs it.

## What the device stores, and what each operation does

| NVS namespace / key | Content | Pairing reset (HomeKeys kept) | Removed from the paired Home | Full HomeKey factory reset |
|---|---|---|---|---|
| `hap_ctrl`, `hap_main` | HomeKit pairings, accessory ID | erased | controllers erased | erased |
| `HK_DATA/HKSTORE` | all reader profiles (keys, issuers, devices, persistent keys) | **kept**; active Home archived | only that Home's profile removed | erased |
| `HK_DATA/READERDATA` | old single-Home data (migration source, backup for downgrades) | kept | erased | erased |
| `HK_DATA/HKSTORE_BAD` | copy of an unreadable store (only after corruption) | kept | kept | erased |
| `HK_TMP` | the library's temporary copy during provisioning, erased immediately | - | - | erased |
| `hap_esph/setup_code` | optional setup code set at runtime | kept | kept | kept |

## OTA and recovery

- **Updates:** use only normal ESPHome OTA. OTA rewrites only the app partition; the keys in NVS stay untouched. Avoid the "erase flash" / "erase device" option, because it erases every Home's HomeKeys.
- **Migration:** on the first boot of the new firmware, the old `READERDATA` becomes the first profile. If HomeKit is still paired it becomes active, otherwise it is archived.
- **The old `homekit_base` "factory_reset" button keeps the HomeKeys too:** at boot, if no controller is paired, the active profile is archived automatically.
- **Full factory reset:** the "Factory reset HomeKit + HomeKeys" button erases the HomeKit pairings and all HomeKey data of every Home, then reboots. Afterwards remove the accessory from every Home app and pair again as needed.
- **Corrupted data:** an unreadable store does not cause a crash or a reboot loop. It is copied to the `HKSTORE_BAD` key, logged, and the device falls back to the old data or to an empty store. A store written by a newer firmware is never overwritten. Saving is atomic (a single NVS blob). If a provisioning change cannot be saved, it is rolled back and HomeKit receives an error.

## Setup code at runtime

The setup code can be changed without recompiling and without flashing over USB, using the Home Assistant `set_homekit_setup_code` action (see the example YAML). It is saved to NVS and applies after the next reboot. `clear_homekit_setup_code` restores the code from the YAML. Trivial codes (e.g. `111-11-111`, `123-45-678`) are rejected.

## Security notes

- Reader private keys, session keys and persistent keys are **never logged**. The HomeKey library's debug logs that contain keys are clamped to INFO even if you raise the ESP-IDF log level. Only for protocol debugging can this be disabled, with the `-DHOMEKEY_INSECURE_DEBUG_LOGGING` build flag.
- Anyone who can press the reset button in Home Assistant or on the web server can enroll a new Home. Protect the web server and HA accordingly.
- A pairing reset does **not** revoke the HomeKeys of archived Homes. The currently paired Home is revoked by removing the accessory in the Home app; an archived Home only by a full factory reset, followed by re-enrolling the Homes you want to keep.
- No new device or user can be added to an archived Home through HomeKit, because it is no longer paired. The attestation flow might accept a new iPhone if its issuer is known, but this is not guaranteed. Otherwise that Home has to be paired again (step 3, then 4).

## ESPHome entities

```yaml
button:
  - platform: homekit
    reset_pairing_keep_homekeys:      # HomeKit pairing reset, HomeKeys kept
      name: "Reset HomeKit pairing (keep HomeKeys)"
    factory_reset_homekeys:           # HomeKit pairing reset + ALL HomeKeys erased
      name: "Factory reset HomeKit + HomeKeys"

sensor:
  - platform: homekit
    homekey_homes:                    # number of enrolled Homes
      name: "HomeKey Homes"
    homekey_issuers:                  # number of users (issuers) in total
      name: "HomeKey issuers"
    homekey_endpoints:                # number of devices (iPhone / Watch) in total
      name: "HomeKey devices"

binary_sensor:
  - platform: homekit
    homekey_provisioned:              # at least one Home wrote its reader key
      name: "HomeKey provisioned"
    homekey_active_home_provisioned:  # the paired Home wrote its reader key
      name: "HomeKey paired Home provisioned"
```

All are optional and only work with a `homekit: lock:` entry that has `nfc_id`. None of them exposes key material. Complete example (a reader with its own virtual lock that every HomeKey toggles): [examples/homekey-virtual-lock.yaml](../examples/homekey-virtual-lock.yaml).

## What is tested, and what is not

- **Compiled** for `esp32dev` (ESP-IDF): an existing YAML compiles unchanged with the new code (backwards compatible), and so does the example in `examples/`.
- **Host tests** (`tests/homekey_host/run_tests.sh`):
  - the store logic with 272 checks (persistence, migration, resets, duplicates, corrupted data, log safety);
  - the real HomeKey library with 59 checks, against a simulated phone using real cryptography (two Homes, fast and full authentication, reboot);
  - 43 malformed or malicious NFC inputs, each in a separate process, none of which may crash or hang the library, plus genuine attestation documents that must still be accepted.
- **Not yet tried with real iPhones.** Two things can only be verified that way:
  - whether iOS keeps Home A's Wallet key working while the lock shows "No Response" there;
  - how a real iPhone reacts when the reader first tries the wrong Home's key.
- Because the wake-up frame alternates between Homes, Express Mode may react a little more slowly.

## Limitations

- Only one Home controls the device through HomeKit at a time (the lock and every other entity under `homekit:`). This is a limitation of the HAP protocol: a paired accessory only accepts new controllers from its own admin. See [What appears in Apple Home, and which Home can control it](#what-appears-in-apple-home-and-which-home-can-control-it).
- Several HomeKey locks on the same device share the same reader data (HomeKit pairing is per device, and Apple Homes use the same reader key for all their locks).

## HomeKey library

The HomeKey library ([HK-HomeKit-Lib](https://github.com/rednblkx/HK-HomeKit-Lib)) is included in this repository (`components/homekit/HK-HomeKit-Lib`) instead of being downloaded at build time. The original version could be crashed (the ESP32 rebooted) by a malicious NFC device sending malformed data; it could not unlock anything with it. This is fixed here: every length and field coming from the NFC device is checked first. The changes are listed in [VENDORED.md](../components/homekit/HK-HomeKit-Lib/VENDORED.md); the handling of valid iPhone traffic is unchanged.
