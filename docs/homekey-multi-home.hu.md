# HomeKey több, egymástól független Apple Otthonnal

[English](homekey-multi-home.md) | **Magyar**

Két (vagy több) ember, akiknek az iPhone-ja **különböző Apple Otthonban** van, ugyanahhoz az olvasóhoz kaphat működő HomeKey-t, Otthon-megosztás nélkül. HomeKit-en keresztül egyszerre mindig csak egy Otthon vezérli a zárat, de az összes beléptetett Otthon HomeKey-e nyitja NFC-n.

## Mi volt a gond

Minden Apple Otthon a **saját olvasó privát kulcsát** írja a zárba. Az iPhone ennek a kulcsnak a hash-ét (olvasócsoport-azonosító) keresi az NFC lekérdező keretben és a hitelesítéskor, és az olvasó aláírását az **saját Otthona** kulcsával ellenőrzi.

A korábbi firmware csak egyetlen olvasókulcsot tárolt. Amikor egy második Otthon beállította a HomeKey-t, felülírta az első Otthon kulcsát, és az első Otthon iPhone-jai többé nem működtek. Ha pedig a tartozékot eltávolították egy Otthonból, minden HomeKey-adat törlődött.

## Hogyan működik a javítás

- **Otthononként egy „olvasóprofil”.** Egy profil tartalmazza az adott Otthon olvasókulcsát, a felhasználóit (kiállítók) és a hozzájuk tartozó eszközöket (iPhone, Watch).
  - Az éppen párosított Otthon profilja **aktív**: minden HomeKit-es beállítás ide kerül.
  - A korábbi Otthonok profiljai **archiváltak**: HomeKit-forgalom nem módosítja őket, de NFC-n továbbra is nyitnak.
- **Érintéskor az olvasó minden Otthont végigpróbál.** A PN532 Otthononként felváltva sugározza az ébresztő (ECP) keretet, így minden iPhone a saját Otthonára ébred fel Expressz módban. Először azt az Otthont próbálja, amelyik a telefont felébresztette. Ha az nem sikerül, újraindítja a tranzakciót a következő Otthonnal, és közben nem jelez hibát a telefonnak.
- **Nincs duplikáció.** Ugyanaz a felhasználó, eszköz vagy újrapárosított Otthon nem jön létre kétszer, hanem összevonódik.
- Legfeljebb **8 Otthon** tárolható.

## Telefon A és Telefon B beléptetése

1. Töltsd fel OTA-val az új firmware-t. A meglévő HomeKey-adat automatikusan átkerül az új formátumba.
2. Párosítsd a tartozékot az **A Otthonnal** (A telefon) a setup kóddal, add hozzá a HomeKey-t a Wallethez, és próbáld ki az olvasón.
3. Nyomd meg a **„Reset HomeKit pairing (keep HomeKeys)”** gombot. Az eszköz újraindul és újra párosítható lesz.
   - Az A Otthonban a zár „Nem válaszol” állapotú lesz; ez várható.
   - **Ne távolítsd el a tartozékot az A Otthonból!** Az eltávolítás visszavonja az A Otthon HomeKey-eit, és a Home app a Wallet-kulcsot is törölheti.
4. Párosítsd a **B Otthonnal** (B telefon) ugyanazzal a setup kóddal, add hozzá a HomeKey-t, és próbáld ki.
5. Mostantól mindkét telefon nyitja az olvasót, újraindítás, áramszünet és OTA frissítés után is. Ellenőrzésként a „HomeKey Home-ok száma” szenzor értéke 2.

Egy C Otthon később ugyanígy felvehető (3. és 4. lépés). Firmware-módosítás nem kell hozzá.

## Mi jelenik meg az Apple Otthonban, és melyik Otthonból vezérelhető

**Az Apple Otthonba csak az kerül, ami a YAML `homekit:` szakaszában szerepel** (`light`, `lock`, `switch`, `fan`, `climate`, `sensor`). Minden más ESPHome entitás csak a Home Assistantban jelenik meg: a gombok, a HomeKey diagnosztikai szenzorok, a szöveges szenzorok, és minden, ami nincs a `homekit:` alatt. Ez akkor is így van, ha az entitásnál `platform: homekit` áll. Ez csak azt jelenti, hogy a `homekit` komponens adja az entitást, HomeKit-be ettől nem kerül át. Maga az eszköz HomeKit-hídként jelenik meg. Ezt a Home app a tartozék beállításai között mutatja, külön csempeként nem.

**HomeKit-en keresztül mindig csak az éppen párosított (aktív) Otthon vezérli az eszközt.** Ez az eszköz minden HomeKit-entitására igaz, nem csak a zárra. Ha például egy lámpát is felveszel a `homekit:` alá, azt csak az aktív Otthon telefonjai kapcsolhatják. A korábbi, archivált Otthonokban a lámpa és a zár is „Nem válaszol” állapotú. Otthonok között **csak a HomeKey (a Wallet-kulcs NFC-érintése) működik**.

| | Aktív Otthon (az utoljára párosított) | Archivált Otthonok (a korábban párosítottak) |
|---|---|---|
| A `homekit:` alatti entitások (lámpa, kapcsoló, zár stb.) a Home appban | ✅ az Otthon minden telefonjáról és tagjától | ❌ „Nem válaszol” |
| A zár állapota a Home appban | ✅ | ❌ „Nem válaszol” |
| HomeKey a Walletben (NFC-érintés az olvasón) | ✅ | ✅ |
| A Home Assistantban (minden entitás) | ✅ | ✅ |

Ennek oka, hogy a „Reset HomeKit pairing (keep HomeKeys)” törli az előző Otthon HomeKit-párosítását, és csak a HomeKey-kulcsokat tartja meg az NFC-olvasóhoz. A HAP protokoll szerint egy tartozékot egyszerre csak egy Otthon vezérelhet.

**Ha egy entitást minden Otthonból vezérelni szeretnél** (lámpa, a HomeKey virtuális zár, egy valódi zár stb.), ne az ESP `homekit:` szakaszába tedd, hanem használd a Home Assistant [HomeKit Bridge](https://www.home-assistant.io/integrations/homekit/) integrációját. A HA-ban több HomeKit Bridge példány hozható létre, és mindegyik más-más Otthonhoz párosítható. Mindegyikbe ugyanazok az entitások is betehetők. Az ESP `homekit:` szakaszában csak a HomeKey-es zár maradjon, mert a Wallet-kulcshoz arra szükség van.

## Mit tárol az eszköz, és mi történik a különböző műveleteknél

| NVS névtér / kulcs | Tartalom | Párosítás törlése (HomeKey marad) | Eltávolítás a párosított Otthonból | Teljes HomeKey gyári visszaállítás |
|---|---|---|---|---|
| `hap_ctrl`, `hap_main` | HomeKit párosítások, tartozékazonosító | törlődik | a vezérlők törlődnek | törlődik |
| `HK_DATA/HKSTORE` | az összes olvasóprofil (kulcsok, kiállítók, eszközök, perzisztens kulcsok) | **megmarad**; az aktív Otthon archiválódik | csak annak az Otthonnak a profilja törlődik | törlődik |
| `HK_DATA/READERDATA` | a régi, egy-Otthonos adat (migráció forrása, biztonsági mentés visszaálláshoz) | megmarad | törlődik | törlődik |
| `HK_DATA/HKSTORE_BAD` | olvashatatlan tároló másolata (csak sérülés esetén) | megmarad | megmarad | törlődik |
| `HK_TMP` | a könyvtár ideiglenes másolata beállításkor, azonnal törlődik | - | - | törlődik |
| `hap_esph/setup_code` | opcionális, futásidőben beállított setup kód | megmarad | megmarad | megmarad |

## OTA és helyreállítás

- **Frissítés:** csak normál ESPHome OTA-t használj. Az OTA csak az alkalmazáspartíciót írja újra, az NVS-ben lévő kulcsok érintetlenek maradnak. Kerüld a „flash törlés” / „erase device” opciót, mert az minden Otthon HomeKey-ét törli.
- **Migráció:** az új firmware első indulásakor a régi `READERDATA` lesz az első profil. Ha a HomeKit még párosítva van, aktív lesz, különben archiválódik.
- **A régi `homekit_base` „factory_reset” gomb is megtartja a HomeKey-eket:** induláskor, ha nincs párosított vezérlő, az aktív profil automatikusan archiválódik.
- **Teljes gyári visszaállítás:** a „Factory reset HomeKit + HomeKeys” gomb törli a HomeKit párosításokat és minden Otthon összes HomeKey-adatát, majd újraindít. Utána töröld a tartozékot minden Home appból, és párosítsd újra, amire szükség van.
- **Sérült adat:** egy olvashatatlan tároló nem okoz összeomlást vagy újraindulási hurkot. Átmásolódik a `HKSTORE_BAD` kulcsba, naplóba kerül, és az eszköz a régi adatra vagy üres tárolóra áll vissza. Újabb firmware által írt tárolót sosem ír felül. A mentés atomikus (egyetlen NVS blob). Ha egy beállítási változás nem menthető, visszaáll, és a HomeKit hibát kap.

## Setup kód futásidőben

A setup kód újrafordítás és USB-s flashelés nélkül is módosítható: a Home Assistant `set_homekit_setup_code` műveletével (lásd a példa YAML-t). Az NVS-be mentődik, és a következő újraindítás után érvényes. A `clear_homekit_setup_code` visszaállítja a YAML-ben megadott kódot. Triviális kódokat (pl. `111-11-111`, `123-45-678`) az eszköz elutasít.

## Biztonsági megjegyzések

- Olvasó privát kulcs, munkamenet-kulcs és perzisztens kulcs **sosem kerül naplóba**. A HomeKey könyvtár kulcsokat tartalmazó debug naplói INFO szintre vannak korlátozva akkor is, ha az ESP-IDF naplószintet felemeled. Csak protokoll-hibakereséshez kapcsolható ki a `-DHOMEKEY_INSECURE_DEBUG_LOGGING` fordítási kapcsolóval.
- Aki megnyomhatja a reset gombot a Home Assistantban vagy a webszerveren, az új Otthont léptethet be. Ennek megfelelően védd a webszervert és a HA-t.
- A párosítás törlése **nem** vonja vissza az archivált Otthonok HomeKey-eit. Az éppen párosított Otthont az Home appból való eltávolítással lehet visszavonni; archivált Otthont csak teljes gyári visszaállítással, majd a megtartandó Otthonok újrabeléptetésével.
- Archivált Otthonba nem lehet HomeKit-en keresztül új eszközt vagy felhasználót felvenni, mert már nincs párosítva. Egy új iPhone-t az attesztációs folyamat esetleg elfogadhat, ha a kiállítója ismert, de ez nem garantált. Egyébként azt az Otthont újra kell párosítani (3., majd 4. lépés).

## ESPHome entitások

```yaml
button:
  - platform: homekit
    reset_pairing_keep_homekeys:      # HomeKit párosítás törlése, HomeKey-ek maradnak
      name: "Reset HomeKit pairing (keep HomeKeys)"
    factory_reset_homekeys:           # HomeKit párosítás + MINDEN HomeKey törlése
      name: "Factory reset HomeKit + HomeKeys"

sensor:
  - platform: homekit
    homekey_homes:                    # beléptetett Otthonok száma
      name: "HomeKey Home-ok száma"
    homekey_issuers:                  # felhasználók (kiállítók) száma összesen
      name: "HomeKey kiállítók száma"
    homekey_endpoints:                # eszközök (iPhone / Watch) száma összesen
      name: "HomeKey eszközök száma"

binary_sensor:
  - platform: homekit
    homekey_provisioned:              # legalább egy Otthon beírta az olvasókulcsát
      name: "HomeKey kiosztva"
    homekey_active_home_provisioned:  # a párosított Otthon beírta az olvasókulcsát
      name: "HomeKey: párosított Home kiosztva"
```

Mindegyik opcionális, és csak `nfc_id`-vel rendelkező `homekit: lock:` bejegyzéssel használható. Kulcsanyagot egyik sem tesz közzé. Teljes példa (olvasó saját virtuális zárral, amelyet minden HomeKey nyit és zár): [examples/homekey-virtual-lock.yaml](../examples/homekey-virtual-lock.yaml).

## Mi van tesztelve, és mi nem

- **Lefordítva** `esp32dev`-re (ESP-IDF): a régi YAML változtatás nélkül is fordul az új kóddal (visszafelé kompatibilis), és az `examples/` mappában lévő példa is.
- **Gépi tesztek** (`tests/homekey_host/run_tests.sh`):
  - a tároló logikája 272 ellenőrzéssel (perzisztencia, migráció, resetek, duplikációk, sérült adat, naplóbiztonság);
  - a valódi HomeKey könyvtár 59 ellenőrzéssel, egy valódi kriptográfiát használó szimulált telefonnal (két Otthon, gyors és teljes hitelesítés, újraindítás);
  - 43 hibás vagy rosszindulatú NFC bemenet, mindegyik külön folyamatban, amelyek egyike sem okozhat összeomlást vagy lefagyást, valamint valódi szerkezetű attesztációs dokumentumok, amelyeket továbbra is el kell fogadnia.
- **Valódi iPhone-okkal még nincs kipróbálva.** Két dolog csak így igazolható:
  - hogy az iOS működőképesen hagyja-e az A Otthon Wallet-kulcsát, miközben a zár ott „Nem válaszol”;
  - hogy a valódi iPhone hogyan reagál, ha először a rossz Otthon kulcsával próbálkozik az olvasó.
- Az Otthonok közötti felváltott ébresztés miatt az Expressz mód kicsit lassabban reagálhat.

## Korlátok

- HomeKit-en keresztül egyszerre csak egy Otthon vezérli az eszközt (a zárat és minden más `homekit:` alatti entitást). Ez a HAP protokoll korlátja: párosított tartozék csak a saját adminjától fogad el új vezérlőt. Lásd: [Mi jelenik meg az Apple Otthonban, és melyik Otthonból vezérelhető](#mi-jelenik-meg-az-apple-otthonban-és-melyik-otthonból-vezérelhető).
- Ugyanazon az eszközön lévő több HomeKey-es zár közös olvasóadatot használ (a HomeKit párosítás eszközszintű, és az Apple Otthonok minden zárjukhoz ugyanazt az olvasókulcsot használják).

## HomeKey könyvtár

A HomeKey könyvtár ([HK-HomeKit-Lib](https://github.com/rednblkx/HK-HomeKit-Lib)) ebben a repóban található (`components/homekit/HK-HomeKit-Lib`), nem fordításkor töltődik le. Az eredeti változatot egy rosszindulatú NFC eszköz hibás adatokkal össze tudta omlasztani (az ESP32 újraindult); nyitni nem tudott vele. Ez itt javítva van: az NFC eszköztől érkező minden hossz és mező előbb ellenőrzésre kerül. A változtatások listája: [VENDORED.md](../components/homekit/HK-HomeKit-Lib/VENDORED.md) (angolul); a valódi iPhone-forgalom kezelése nem változott.
