# HomeKey Multi-Home for ESPHome

[English](README.md) | **Magyar**

Apple HomeKey NFC zárolvasó ESP32-re, ESPHome-mal. **Több, egymástól független Apple Otthon** iPhone-jai nyithatják ugyanazt az olvasót, Otthon-megosztás nélkül.

> Ez egy önálló projekt, amely a [rednblkx/HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome) HomeKit-komponenseire épül. Az eredeti projekt egy Apple Otthont támogat. Ez a változat a HomeKey-kezelést írta át úgy, hogy több Otthon kulcsai egyszerre éljenek. Az Apple-lel semmilyen kapcsolatban nem áll.

## Miben más

Minden Apple Otthon a saját olvasókulcsát írja a zárba. Az eredeti megoldás csak egyet tárolt, ezért egy második Otthon beállítása elrontotta az elsőt. Itt:

- **Otthononként külön kulcs.** Az A Otthon iPhone-ja és a B Otthon iPhone-ja is nyit, közös Otthon nélkül.
- **Új Otthon felvétele firmware-módosítás nélkül.** Egy gombnyomás („Reset HomeKit pairing (keep HomeKeys)”), majd párosítás az új Otthonnal.
- **A meglévő HomeKey-ek megmaradnak** újraindítás, áramszünet, OTA frissítés és a párosítás törlése után is.
- **Kétféle reset:** csak a HomeKit párosítás törlése (a HomeKey-ek maradnak), vagy teljes gyári visszaállítás (minden törlődik).
- **Diagnosztika Home Assistantban:** Otthonok, felhasználók és eszközök száma. Kulcsanyagot nem tesz közzé.
- **Biztonság:** a privát kulcsok nem kerülnek naplóba, a sérült tárolt adat nem okoz újraindulási hurkot, egy rosszindulatú NFC eszköz hibás adatai nem omlasztják össze az olvasót, a régi formátumú adat automatikusan átkerül.
- **Setup kód** futásidőben is módosítható, újrafordítás nélkül.

HomeKit-en keresztül egyszerre csak egy Otthon vezérli a zárat. Ez a HomeKit protokoll korlátja; NFC-n viszont minden beléptetett Otthon nyit.

## Hardver

- ESP32 (tesztelve: ESP32 DevKit V1 / `esp32dev`), 4 MB flash, ESP-IDF keretrendszer
- PN532 NFC modul **SPI módban** (`pn532_spi`), 3,3 V
- HomeKey-képes iPhone vagy Apple Watch

Példa bekötés: SCK GPIO18, MISO GPIO19, MOSI GPIO23, CS GPIO5.

## Gyors kezdés

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

Teljes példa az összes új entitással: [examples/homekey-virtual-lock.yaml](examples/homekey-virtual-lock.yaml), olvasó saját virtuális zárral, amelyet minden HomeKey nyit és zár, magyar megjegyzésekkel.

## Két Otthon beléptetése

1. Párosítsd az eszközt az **A Otthonnal**, és add hozzá a HomeKey-t a Wallethez.
2. Nyomd meg a **„Reset HomeKit pairing (keep HomeKeys)”** gombot. Az eszköz újraindul és újra párosítható. Az A Otthonból **ne** távolítsd el.
3. Párosítsd a **B Otthonnal**, és add hozzá a HomeKey-t.
4. Mindkét telefon nyit. Egy újabb Otthon ugyanígy felvehető (legfeljebb 8).

> **Fontos:** HomeKey-jel minden beléptetett Otthon nyit, de a Home appból csak az éppen párosított Otthon vezérli az eszközt. Ez minden `homekit:` alatti entitásra igaz (pl. egy lámpára is); a többi Otthonban ezek „Nem válaszol” állapotúak. Az Apple Otthonba egyáltalán csak az kerül, ami a `homekit:` alatt szerepel. Ha valamit minden Otthonból vezérelni szeretnél, használd a Home Assistant HomeKit Bridge integrációját. Részletek: [Mi jelenik meg az Apple Otthonban](docs/homekey-multi-home.hu.md#mi-jelenik-meg-az-apple-otthonban-és-melyik-otthonból-vezérelhető).

Részletek, mentési táblázat, helyreállítás és biztonsági megjegyzések: [docs/homekey-multi-home.hu.md](docs/homekey-multi-home.hu.md).

## Dokumentáció

- [docs/homekey-multi-home.hu.md](docs/homekey-multi-home.hu.md): a több Otthonos HomeKey működése, beléptetés, OTA, gyári visszaállítás
- [docs/components.hu.md](docs/components.hu.md): az összes komponens és konfigurációs opció referenciája (a HomeKey mellett lámpa, kapcsoló, szenzor és ventilátor is)

Minden dokumentum angolul is elérhető, a lapok tetején lehet nyelvet váltani.

## Tesztek

```bash
./tests/homekey_host/run_tests.sh
```

Három tesztcsomag fut gépen: a HomeKey-tároló logikája, a valódi HomeKey-könyvtár egy szimulált, valódi kriptográfiát használó telefonnal, valamint hibás vagy rosszindulatú NFC bemenetek, amelyek nem okozhatnak összeomlást. A GitHub CI minden PR-on futtatja őket.

## Állapot

A HomeKey protokoll visszafejtésen alapul; az Apple bármikor változtathat rajta. A több Otthonos működés tesztekkel és szimulált telefonnal ellenőrzött, valódi iPhone-okkal végzett hosszú távú tapasztalat még nincs.

## Köszönet és licenc

- [rednblkx/HAP-ESPHome](https://github.com/rednblkx/HAP-ESPHome) és [HK-HomeKit-Lib](https://github.com/rednblkx/HK-HomeKit-Lib) ([@rednblkx](https://github.com/rednblkx)): az eredeti HomeKit- és HomeKey-komponensek
- [@kormax](https://github.com/kormax): a HomeKey NFC protokoll és az ECP [feltérképezése](https://github.com/kormax/apple-home-key)
- [@kupa22](https://github.com/kupa22): a HomeKey HAP-részének [dokumentálása](https://github.com/kupa22/apple-homekey)
- [ESPHome](https://github.com/esphome/esphome) (PN532 komponens) és [Espressif](https://github.com/espressif/esp-homekit-sdk) (esp-homekit-sdk)

A `homekit` és `homekit_base` komponensek GPL-3.0, a `pn532` és `pn532_spi` komponensek az ESPHome licence alatt állnak; lásd az egyes mappák `LICENSE` fájljait a [components](components) könyvtárban.
