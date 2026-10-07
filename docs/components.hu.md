# Komponens-referencia

[English](components.md) | **Magyar**

A `homekit_base`, `homekit`, `pn532` és `pn532_spi` komponensek konfigurációs referenciája.
Az eredeti [rednblkx/HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome) projekt README-jén alapul, kiegészítve ennek a projektnek a több Otthonos HomeKey-támogatásával ([4.3](#43-homekey-több-egymástól-független-apple-otthonnal)).
Áttekintés és gyors kezdés: [README](../README.hu.md).

## 1. Bevezetés

A projekt célja, hogy az ESPHome-konfigurációval flashelt ESP32 eszközök HomeKit-támogatást kapjanak, így közvetlenül, bármilyen közvetítő nélkül vezérelhetők az Apple Home appból.

A komponensek a többi külső komponenshez hasonlóan importálhatók:

```yaml
external_components:
  source: github://mefisto22/esphome-homekey-multihome@main
  refresh: 0s
```

A részletes leírás a [Komponensek](#3-komponensek) fejezetben található.

> [!IMPORTANT]  
> Egyes komponensek, például a Bluetooth, sok RAM-ot foglalnak, és fordítási hibát okozhatnak. Ilyenkor a naplóban valami hasonló jelenik meg: `section '.iram0.text' will not fit in region 'iram0_0_seg'`.

### Támogatott entitástípusok

| Típus | Tulajdonságok | Megjegyzés |
|--------|-----------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------|
| Lámpa (Light) | Be/Ki, fényerő, RGB, színhőmérséklet | |
| Zár (Lock) | Zárás/Nyitás | HomeKey bekapcsolható, de csak a `pn532_spi` komponenssel használható |
| Kapcsoló (Switch) | Be/Ki | |
| Szenzor (Sensor) | Hőmérséklet, páratartalom, megvilágítás, levegőminőség, CO2, CO, PM10, PM2.5 | A `device_class` tulajdonságot a szenzor típusának megfelelően meg kell adni, a HASS [dokumentáció](https://www.home-assistant.io/integrations/sensor/#device-class) szerint |
| Ventilátor (Fan) | Be/Ki | |

## 2. Előfeltételek

A komponensek az ESP-IDF 5-ös verziójához készültek.

Az alábbi beállításoknak szerepelniük kell a YAML-fájlban:

```yaml
esp32:
  board: <insert board id>
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_COMPILER_OPTIMIZATION_SIZE: y
      CONFIG_LWIP_MAX_SOCKETS: "16"
      CONFIG_MBEDTLS_HKDF_C: y
```

A `CONFIG_COMPILER_OPTIMIZATION_SIZE` működéshez nem szükséges, de segít a méret csökkentésében, mert egy teljes konfiguráció sok helyet foglal.

## 3. Komponensek

A projekt két komponensből áll: a `homekit_base` kezeli a híd (bridge) logikáját, a `homekit` pedig magukat a tartozékokat (lámpák, kapcsolók stb.).

A repóban találhatók még a `pn532` és `pn532_spi` komponensek is. Ezek az ESPHome hivatalos komponenseinek a HomeKey igényeihez kissé módosított változatai, új opciót nem kaptak és nem is veszítettek el. Nem garantált, hogy követik az ESPHome későbbi változásait.

> [!TIP]
> Konfigurációs példákat a repó `.yaml` fájljaiban találsz, például: [lights-c3.yaml](../lights-c3.yaml)

### 3.1. `homekit_base`

> [!NOTE]  
> A `homekit_base` komponenst csak akkor kell a konfigurációba felvenni, ha az alább felsorolt tulajdonságok közül valamelyiket használni szeretnéd, mert a `homekit` komponens automatikusan betölti.

#### 3.1.1. Konfigurációs változók:

- **port** (Opcionális, egész szám): a port, amelyen a HomeKit figyel
- **meta** (Opcionális): a híd adatai
  - **name** (Opcionális, szöveg): a híd tartozék neve
  - **model** (Opcionális, szöveg): a híd tartozék modellneve
  - **manufacturer** (Opcionális, szöveg): a híd tartozék gyártója
  - **serial_number** (Opcionális, szöveg): a híd tartozék sorozatszáma
  - **fw_rev** (Opcionális, szöveg): a híd tartozék firmware-verziója
- **setup_code** (Opcionális, szöveg): a HomeKit setup kód `XXX-XX-XXX` formátumban - **Alapértelmezés:** `159-35-728`
- **setup_id** (Opcionális, szöveg): a Setup ID, amellyel párosító QR-kód generálható - **Alapértelmezés:** `ES32`
- **id** (Opcionális, ID): csak akkor kell, ha az alábbi futásidejű függvényeket lambdából hívod

A setup kód futásidőben, újrafordítás nélkül is módosítható: a `set_setup_code_override("XXX-XX-XXX")` elmenti az NVS-be, és a következő újraindítás után ezt használja a `setup_code` helyett; a `clear_setup_code_override()` visszaállítja a YAML-ben megadott értéket. Triviális kódokat (`111-11-111`, `123-45-678`, ...) elutasít. Példa Home Assistant-műveletként:

```yaml
api:
  actions:
    - action: set_homekit_setup_code
      variables:
        code: string
      then:
        - lambda: 'id(homekit_bridge).set_setup_code_override(code);'
    - action: clear_homekit_setup_code
      then:
        - lambda: 'id(homekit_bridge).clear_setup_code_override();'
```

Konfigurációs példa:

```yaml

homekit_base:
  meta:
    name: "PRIMO"
    manufacturer: "AMICI&CO"
    model: "IMPERIUM"
    serial_number: "16161616"
    fw_rev: "0.16.2"
  setup_code: '159-35-728'
  setup_id: "ES32"
```

#### 3.1.2. Gyári visszaállítás

A `homekit_base` a button komponens platformjaként is használható a HomeKit párosítások törlésére:

  ```yaml
  button:
  - platform: homekit_base
    factory_reset:
      name: "Reset HomeKit pairings"
  ```
Ez úgy működik, mint bármely ESPHome gomb, tehát megjelenik a webes felületen és a HASS-ban.

Csak a HomeKit (HAP) párosításokat törli. A HomeKey-adatok **megmaradnak** (lásd [4.3](#43-homekey-több-egymástól-független-apple-otthonnal)); HomeKey-t is figyelembe vevő párosítás-törléshez vagy teljes HomeKey gyári visszaállításhoz a `homekit` button platformot használd.

### 3.2. `homekit`

Ez kezeli a tartozékok logikáját: az állapotok szinkronizálását a HomeKit és az ESPHome között, valamint az alapadatokat (név, tulajdonságok stb.).

Az Apple Otthonban csak az itt felsorolt entitások jelennek meg, és HomeKit-en keresztül mindig csak az éppen párosított Otthon vezérli őket; a korábban beléptetett Otthonokban „Nem válaszol” állapotúak. Lásd: [Mi jelenik meg az Apple Otthonban, és melyik Otthonból vezérelhető](homekey-multi-home.hu.md#mi-jelenik-meg-az-apple-otthonban-és-melyik-otthonból-vezérelhető).

#### 3.2.1. Konfigurációs változók:
- **light** (Opcionális): lámpa-entitások listája
  - **id** (Kötelező, [Light](https://esphome.io/components/light/)) - a lámpa-entitás azonosítója
  - **meta** (Opcionális): a tartozék adatai
    - **name** (Opcionális, szöveg): a tartozék neve, alapértelmezés szerint az entitás neve
    - **model** (Opcionális, szöveg): a tartozék modellneve
    - **manufacturer** (Opcionális, szöveg): a tartozék gyártója
    - **serial_number** (Opcionális, szöveg): a tartozék sorozatszáma, alapértelmezés szerint a belső objektumazonosító
    - **fw_rev** (Opcionális, szöveg): a tartozék firmware-verziója
   
  Példa:
  ```yaml
  homekit:
    light:
      - id: desk_light
        meta:
          name: "RGB Light"
          manufacturer: "AMICI&CO"
          model: "IGNIS"
          serial_number: "42424242"
          fw_rev: "0.16.2"
  ```

- **lock** (Opcionális): zár-entitások listája
  - **id** (Kötelező, [Lock](https://esphome.io/components/lock/)) - a zár-entitás azonosítója
  - **meta** (Opcionális): a tartozék adatai
    - **name** (Opcionális, szöveg): a tartozék neve, alapértelmezés szerint az entitás neve
    - **model** (Opcionális, szöveg): a tartozék modellneve
    - **manufacturer** (Opcionális, szöveg): a tartozék gyártója
    - **serial_number** (Opcionális, szöveg): a tartozék sorozatszáma, alapértelmezés szerint a belső objektumazonosító
    - **fw_rev** (Opcionális, szöveg): a tartozék firmware-verziója
  - **nfc_id** (Opcionális, [PN532](https://esphome.io/components/binary_sensor/pn532.html#over-spi)): a `pn532_spi` komponens azonosítója, a HomeKey funkcióhoz
  - **on_hk_success** (Opcionális, [Action](https://esphome.io/automations/actions)): sikeres HomeKey-hitelesítéskor végrehajtandó művelet
  - **on_hk_fail** (Opcionális, [Action](https://esphome.io/automations/actions)): sikertelen HomeKey-hitelesítéskor végrehajtandó művelet
  - **hk_hw_finish** (Opcionális, szöveg): a HomeKey-kártya színe a `BLACK`, `SILVER`, `GOLD` és `TAN` értékek közül, alapértelmezés: `BLACK`
 
  Példa:
  ```yaml
  homekit:
    lock:
      - id: this_lock
        meta:
          manufacturer: "AMICI&CO"
          model: "IMPEDIO"
          serial_number: "42424242"
          fw_rev: "0.16.2"
        nfc_id: nfc_spi_module
        on_hk_success:
          lambda: |-
            ESP_LOGI("HEREHERE", "IssuerID: %s", x.c_str());
            ESP_LOGI("HEREHERE", "EndpointID: %s", y.c_str());
            id(test_light).toggle().perform();
        on_hk_fail:
          lambda: |-
            ESP_LOGI("GSDGSGS", "IT FAILED :(");
        hk_hw_finish: "SILVER"
  ```
- **sensor**
  - **id** (Kötelező, [Sensor](https://esphome.io/components/sensor/)): a szenzor-entitás azonosítója
  - **meta** (Opcionális): a tartozék adatai
    - **name** (Opcionális, szöveg): a tartozék neve, alapértelmezés szerint az entitás neve
    - **model** (Opcionális, szöveg): a tartozék modellneve
    - **manufacturer** (Opcionális, szöveg): a tartozék gyártója
    - **serial_number** (Opcionális, szöveg): a tartozék sorozatszáma, alapértelmezés szerint a belső objektumazonosító
    - **fw_rev** (Opcionális, szöveg): a tartozék firmware-verziója

  Példa:
  ```yaml
  homekit:
    sensor:
      - id: my_sensor
        meta:
          manufacturer: "AMICI&CO"
          model: "VARIO"
          serial_number: "42424242"
          fw_rev: "0.16.2"
  ```
- **switch**
  - **id** (Kötelező, [Switch](https://esphome.io/components/switch/)): a kapcsoló-entitás azonosítója
  - **meta** (Opcionális): a tartozék adatai
    - **name** (Opcionális, szöveg): a tartozék neve, alapértelmezés szerint az entitás neve
    - **model** (Opcionális, szöveg): a tartozék modellneve
    - **manufacturer** (Opcionális, szöveg): a tartozék gyártója
    - **serial_number** (Opcionális, szöveg): a tartozék sorozatszáma, alapértelmezés szerint a belső objektumazonosító
    - **fw_rev** (Opcionális, szöveg): a tartozék firmware-verziója

  Példa:
  ```yaml
    switch:
      - id: some_switch
        meta:
          manufacturer: "AMICI&CO"
          model: "TRANSMUTO"
          serial_number: "42424242"
          fw_rev: "0.16.2"
  ```
 - **fan** (Opcionális): ventilátor-entitások listája
   - **id** (Kötelező, [Fan](https://esphome.io/components/fan/)): a ventilátor-entitás azonosítója
   - **meta** (Opcionális): a tartozék adatai
     - **name** (Opcionális, szöveg): a tartozék neve, alapértelmezés szerint az entitás neve
     - **model** (Opcionális, szöveg): a tartozék modellneve
     - **manufacturer** (Opcionális, szöveg): a tartozék gyártója
     - **serial_number** (Opcionális, szöveg): a tartozék sorozatszáma, alapértelmezés szerint a belső objektumazonosító
     - **fw_rev** (Opcionális, szöveg): a tartozék firmware-verziója

   Példa:
   ```yaml
   homekit:
     fan:
       - id: my_fan
         meta:
           name: "Living Room Fan"
           manufacturer: "AMICI&CO"
           model: "VENTUS"
           serial_number: "42424242"
           fw_rev: "0.16.2"
   ```
## 4. HomeKey

> [!NOTE]
> Ha a naplóban a `Can't decode message length.` hibaüzenetet látod, nyugodtan figyelmen kívül hagyhatod. A projekt a hivatalos pn532 komponens átalakított változatát használja, és ez az üzenet a normál működés része, mert a komponenst eredetileg „hagyományos” NFC-címkékhez készítették.

### 4.1 Figyelmeztetés

> [!WARNING]
> A HomeKey teljes működése visszafejtésen alapul, mert a HomeKit specifikáció már nem érhető el hobbifejlesztők számára. Ezért a működés egésze vagy egy része a jövőben elromolhat, és hiányozhatnak belőle hivatalos funkciók vagy belső megoldások.

### 4.2 Beállítás

> [!IMPORTANT]
> Jelenleg csak a SPI-n csatlakozó PN532 (`pn532_spi` komponens) támogatott, mert a szükséges módosítások más protokollokra és chipekre nincsenek átültetve.

> [!NOTE]
> A gyors reagálás és a hibák elkerülése érdekében ne állítsd az `update_interval` értékét 500 ms fölé.

```yaml
spi:
  clk_pin: 4
  miso_pin: 5
  mosi_pin: 6

pn532_spi:
  id: nfc_spi_module
  cs_pin: 7
  update_interval: 100ms

homekit:
  lock:
    - id: <insert lock id>
      nfc_id: nfc_spi_module
```

### 4.3 HomeKey több, egymástól független Apple Otthonnal

Több, egymástól független Apple Otthon HomeKey-e nyithatja ugyanazt az olvasót. Minden Otthon saját olvasóprofilt kap; új Otthon futásidőben, a `homekit` button platformmal vehető fel:

```yaml
button:
  - platform: homekit
    reset_pairing_keep_homekeys:      # HomeKit párosítás törlése, HomeKey-ek maradnak
      name: "Reset HomeKit pairing (keep HomeKeys)"
    factory_reset_homekeys:           # HomeKit párosítás + MINDEN HomeKey törlése
      name: "Factory reset HomeKit + HomeKeys"

sensor:
  - platform: homekit
    homekey_homes:
      name: "HomeKey Home-ok száma"
    homekey_issuers:
      name: "HomeKey kiállítók száma"
    homekey_endpoints:
      name: "HomeKey eszközök száma"

binary_sensor:
  - platform: homekit
    homekey_provisioned:
      name: "HomeKey kiosztva"
    homekey_active_home_provisioned:
      name: "HomeKey: párosított Home kiosztva"
```

Mindegyik opcionális, és `nfc_id`-vel rendelkező `homekit: lock:` bejegyzést igényel. A működés, a beléptetés lépései, a tárolt adatok, az OTA, a helyreállítás, a biztonsági megjegyzések és a korlátok leírása: [HomeKey több, egymástól független Apple Otthonnal](homekey-multi-home.hu.md).

## Eredeti projekt

A `homekit`, `homekit_base`, `pn532` és `pn532_spi` komponensek a [@rednblkx](https://github.com/rednblkx) által készített [rednblkx/HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome) projektből származnak. A szerző munkáját a [GitHub Sponsors](https://github.com/sponsors/rednblkx) oldalon lehet támogatni.

## Köszönet

[@kormax](https://github.com/kormax) - a HomeKey NFC protokoll [feltérképezése](https://github.com/kormax/apple-home-key), az [ECP](https://github.com/kormax/apple-enhanced-contactless-polling) és a [koncepcióbizonyítás (PoC)](https://github.com/kormax/apple-home-key-reader)

[@kupa22](https://github.com/kupa22) - a HomeKey HAP-részének [dokumentálása](https://github.com/kupa22/apple-homekey)

[ESPHome](https://github.com/esphome/esphome) - az ESPHome és a PN532 modul

[Espressif](https://github.com/espressif) - [esp-homekit-sdk](https://github.com/espressif/esp-homekit-sdk)

## Licenc

A repó több licenc alatt álló részt tartalmaz, mivel egyes komponensek ([pn532](../components/pn532) és [pn532_spi](../components/pn532_spi)) eredetileg az [ESPHome](https://github.com/esphome/esphome) repóból származnak. Az egyes mappák `LICENSE` fájljait a [components](../components) könyvtárban találod.
