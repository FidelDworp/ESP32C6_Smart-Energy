# Tesla Smart Charging – ESP32-C6

Lokale sturing van het laden van een Tesla via een ESP32-C6, volledig zonder cloud of officiële Tesla API.

Dit project bestaat uit twee fasen:

1. **TeslaChargeTest.ino** – eenvoudige testinterface om de communicatie met de Tesla te valideren
2. **ESP32_C6_ENERGY.ino** – definitieve integratie in de bestaande energie-controller (solar, schuur, EPEX, P1)

---

## Architectuur

```text
┌─────────────────────────────┐
│  ESP32-C6 (tesla-key-esp32) │  ← BLE-sleutel (Charging Manager)
│  Dicht bij de auto          │
└──────────────┬──────────────┘
               │ HTTP (lokaal)
┌──────────────▼──────────────┐
│  ESP32-C6 (jouw controller) │
│  - Charge.test.ino (test)   │
│  - ESP32_C6_ENERGY (live)   │
└─────────────────────────────┘
```

De **tesla-key-esp32** fungeert als Bluetooth Low Energy proxy.  
Jouw eigen ESP32 praat er via eenvoudige HTTP-calls mee.

Bron van de proxy:  
[https://github.com/0Bu/tesla-key-esp32](https://github.com/0Bu/tesla-key-esp32)

Stappen

1. Firmware installeren
Ga naar de web-installer: https://0bu.github.io/tesla-key-esp32/
Chrome/Edge, USB-kabel, selecteer je ESP32-C6.
Klik Install (eerste keer volledige erase).

2. WiFi + VIN configureren
Verbind met het open netwerk tesla-key-esp32-setup.
Wacht op de automatische webpage (Of ga naar http://192.168.4.1) en vul je WiFi + 17-cijferige VIN in.

=> Apparaat herstart en komt op je netwerk als http://tesla-key-esp32.local/
Opm: In Zarlardinge kreeg deze controller IP: 192.168.0.134

4. Pairen met de auto
Zet de ESP32 dicht bij de auto (binnen ~10 m).
Open http://tesla-key-esp32.local.
Leg een Tesla NFC-keycard op de middenconsole.
Bevestig op het scherm van de auto “Add key”.
In de web-UI zie je “paired”.

=> Bij intikken van 192.168.0.134 krijg je de interface te zien: Gepaird of niet + alle andere data.

6. Bedienen
Via de web-UI of rechtstreeks via HTTP (perfect voor een eenvoudige eigen site):textPOST /api/1/vehicles/{VIN}/command/charge_start
POST /api/1/vehicles/{VIN}/command/charge_stop
POST /api/1/vehicles/{VIN}/command/set_charging_amps
Body: {"charging_amps": 16}
De web-UI toont status (batterij, laadstatus, enz.).

Beschikbaar: REST JSON API op poort 80 van de tesla-key-esp32. Geen websockets of speciale protocollen nodig.
Antwoord is JSON, ongeveer in deze vorm:

JSON{
  "response": {
    "response": {
      "charge_state": {
        "battery_level": 72,
        "usable_battery_level": 71,
        "charging_state": "Charging",
        "charge_amps": 16,
        "charger_power": 3.7,
        "charger_voltage": 230,
        "charger_phases": 1,
        "charge_limit_soc": 80,
        "charge_energy_added": 12.4,
        "minutes_to_full_charge": 95,
        "battery_range": 280.5,
        "charge_port_door_open": true,
        "charge_port_latch": "Engaged",
        ...
      },
      "climate_state": {
        "inside_temp": 21.5,
        "outside_temp": 14.2,
        "driver_temp_setting": 21.0,
        "is_climate_on": false,
        ...
      },
      "vehicle_state": {
        "locked": true,
        "odometer": 45230.1,
        "is_user_present": false,
        ...
      },
      "tire_pressure_state": {
        "front_left": 2.9,
        "front_right": 2.9,
        "rear_left": 2.8,
        "rear_right": 2.8
      }
      // eventueel nog closures_state, drive_state, ...
    }
  }
}

Opm: Het project is AGPL-3.0 en volledig open source. Houd het alleen op je vertrouwde LAN (niet openzetten naar internet).

---

## Vereisten

| Component              | Opmerking                                      |
|------------------------|------------------------------------------------|
| ESP32-C6 (1×)          | Voor **tesla-key-esp32** (dicht bij de auto)   |
| ESP32-C6 (1×)          | Voor de test- of energy-sketch                 |
| Tesla                  | Gepaard als *Charging Manager*                 |
| WiFi                   | Zelfde netwerk                                 |
| Libraries              | ESPAsyncWebServer, AsyncTCP, ArduinoJson       |

---

## Fase 1 – Charge.test.ino

Eenvoudige webinterface om de verbinding en alle laadcommando’s te testen.

### Functionaliteit

- Live data van de Tesla (batterij %, laadstatus, ampère, vermogen, charge limit, range)
- Knoppen:
  - Start laden
  - Stop laden
  - Wake-up
  - Open / sluit laadpoort
- Slider om de laadstroom in te stellen (5–32 A)

### Installatie

1. Flash eerst **tesla-key-esp32** op een ESP32-C6 en pair deze met de auto.
2. Noteer het IP-adres van de tesla-key-esp32.
3. Open `Charge.test.ino` en vul in:

   ```cpp
   const char* WIFI_SSID      = "jouw-wifi";
   const char* WIFI_PASS      = "jouw-wachtwoord";
   const char* TESLA_KEY_HOST = "192.168.0.xx";   // of tesla-key-esp32.local
   const char* TESLA_VIN      = "5YJ............";
   ```

4. Upload naar een **tweede** ESP32-C6.
5. Open in de browser het IP-adres van deze test-ESP.

### Belangrijkste API-calls

| Actie              | Endpoint                                      | Body                          |
|--------------------|-----------------------------------------------|-------------------------------|
| Data ophalen       | `GET /api/1/vehicles/{VIN}/vehicle_data`      | –                             |
| Start laden        | `POST .../command/charge_start`               | (leeg of `true`)              |
| Stop laden         | `POST .../command/charge_stop`                | (leeg of `false`)             |
| Ampère instellen   | `POST .../command/set_charging_amps`          | `{"charging_amps": 16}`       |
| Wake-up            | `POST .../command/wake_up`                    | –                             |

---

## Fase 2 – Integratie in ESP32_C6_ENERGY_v1_27.ino

De bestaande energie-controller meet al:

- Zonneproductie (`w_sol`)
- Schuur afname / injectie (`w_schf`, `w_schr`)
- WON (P1 of simulatie)
- EPEX-prijs
- Nettovermogen en maandpiek

### Doel

Automatisch de Tesla laden met de **beschikbare surplus** (of bij lage EPEX-prijs), zonder de maandpiek te overschrijden.

### Geplande stuurlogica (voorbeeld)

```cpp
// Elke 15–30 seconden
float surplus_w = w_sol - w_schf + w_schr;   // of uitgebreidere berekening

bool goed_moment = (epex_nu < 18.0f) || (w_sol > 1500.0f);

int target_amps = 0;
if (goed_moment && surplus_w > 1200.0f) {
  target_amps = constrain((int)(surplus_w / 230.0f), 5, 16);  // 1-fase
  // of / 690.0f bij 3-fase
}

setTeslaAmps(target_amps);   // start of stop automatisch
```

### Belangrijke ontwerpkeuzes

- Hysterese (alleen wijzigen bij ≥ 2 A verschil) om flappen te voorkomen
- Maximum frequentie van commando’s (15–30 s) om de auto niet onnodig wakker te maken
- Respecteren van de maandpiek (`max_piek_w`)
- Optioneel: alleen laden bij lage EPEX of voldoende solar

De exacte implementatie volgt na succesvolle tests met `Charge.test.ino`.

---

## Bestanden in deze repo

| Bestand                        | Beschrijving                                      |
|--------------------------------|---------------------------------------------------|
| `Charge.test.ino`              | Standalone testinterface                          |
| `ESP32_C6_ENERGY_v1_27.ino`    | Bestaande energie-controller (nog zonder Tesla)   |
| `README.md`                    | Dit document                                      |

---

## Status

| Onderdeel                      | Status            |
|--------------------------------|-------------------|
| tesla-key-esp32 hardware       | Te installeren    |
| Charge.test.ino                | Klaar om te testen|
| Integratie in ENERGY_v1_27     | Gepland           |
| Live surplus-sturing           | Gepland           |

---

## Notities & aandachtspunten

- De tesla-key-esp32 moet **dicht bij de auto** staan (BLE-bereik).
- Eerste commando na lange stilte kan 5–15 seconden duren (wake + BLE reconnect).
- De API heeft geen authenticatie – alleen gebruiken op een vertrouwd LAN.
- Begin altijd met een lage `TESLA_MAX_AMPS` tijdens de eerste tests.

---

## Licentie & credits

- tesla-key-esp32: AGPL-3.0 – [0Bu/tesla-key-esp32](https://github.com/0Bu/tesla-key-esp32)
- Deze controller-sketch: eigen code (Filip Delannoy / Zarlar)

---

*Laatste update: september 2026*
