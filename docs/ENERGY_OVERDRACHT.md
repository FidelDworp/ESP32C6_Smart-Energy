# Overdracht — Smart Energy-controller (ESP32C6_Smart-Energy)

*Stand 7 november 2026 — Filip Delannoy (FiDel), Zarlardinge (BE)*

Dit document start een nieuw gesprek met Claude over S-ENERGY: zonne-opbrengst, net, EPEX-tarieven, maandpiek en Tesla-laden. Alle details: [`README.md`](../README.md) en [`Energy_Management_System_Zarlardinge.md`](Energy_Management_System_Zarlardinge.md).

**Werkwijze en afspraken:** zie de [overkoepelende overdracht](https://github.com/FidelDworp/Zarlardinge-Home-Automation/blob/main/docs/ZARLAR_OVERDRACHT.md). Kort: Nederlands · nooit een sketch of script zonder GO · eerst een plan, één stap per versie (versiekop + changelog) en daarna een 48 u-test · alles via OTA · `/json`-keys en bedieningsadressen niet wijzigen zonder akkoord · README en deze overdracht mee bijwerken · na GO compileert Claude en zet de `.bin` in `bin/`.

---

## 1. Wat en waar

| | |
|---|---|
| Adres | `http://192.168.0.73` · `smart-energy.local` (sinds v1.30) |
| Sketch | `ESP32_C6_ENERGY/ESP32_C6_ENERGY.ino` **v1.30** (6/11/2026) |
| Klaar om te uploaden | `bin/ESP32_C6_ENERGY_v1.30.ino.bin` |
| Metingen | 3 S0-kanalen (zon, schuur afname/injectie), HomeWizard P1 (woning), EPEX via de RPi, matrix 12×4 |
| Tesla | proxy `tesla-key-esp32` (firmware 0Bu, wasplaats), gevonden via mDNS `tesla-key-esp32.local` |
| Systeemdocument | Master-document §6 en Portal Plan (energie) in `Zarlardinge-Home-Automation/docs/` |

## 2. Stand (7/11)

- v1.30 werkt (heap 248 KB), **48 u-test loopt** sinds 6/11 ±21:15.
- Post zelf niet naar Google; het Dashboard logt.

## 3. Open punten

1. 48 u-test v1.30 beoordelen.
2. Kop van de sketch noemt nog `partitions_16mb.csv` → `partitions.csv` bij v1.31.
3. Tesla-laadsturing uitwerken (surplus → laadstroom, lage EPEX → laden, maandpiek respecteren); daarna warmtepompen en was/droog.
4. Optioneel: push bij piekoverschrijding, automatische DST, vaste opslag dynamisch ophalen.

## 4. Praktisch

- Compileren: ESP32C6 Dev Module, Flash 16 MB, Partition Custom, USB CDC On Boot Enabled. Bibliotheken: ArduinoJson, Adafruit NeoPixel, AsyncTCP + ESPAsyncWebServer.
- OTA: enkel `ESP32_C6_ENERGY.ino.bin` via `/update`.
