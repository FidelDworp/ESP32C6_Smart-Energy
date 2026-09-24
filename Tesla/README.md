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

---

# Overnamedocument – Tesla Smart Charging integratie

**Voor:** Claude (vervolgontwikkeling)  
**Van:** Grok / Filip Delannoy  
**Datum:** 24 september 2026  
**Project:** Zarlar Smart Energy Management + Tesla laden

---

## 1. Doel van dit document

Dit document vat samen wat er al bereikt is en wat de volgende stap is: integratie van Tesla-laadsturing in de bestaande energy-controller `ESP32_C6_ENERGY_v1_27.ino`.

Claude moet hiermee verder kunnen zonder de volledige chatgeschiedenis.

---

## 2. Wat al bereikt is

### 2.1 Architectuurkeuze

Tesla wordt **lokaal** gestuurd via BLE, niet via de cloud-API.

```
ESP32-C6 (energy-controller)  --HTTP-->  ESP32 (tesla-key-esp32)  --BLE-->  Tesla
```

- **tesla-key-esp32** (https://github.com/0Bu/tesla-key-esp32): BLE-proxy, pairt als *Charging Manager*, exposeert REST op poort 80.
- Geen Tesla-account tokens, geen rate limits, werkt offline op het LAN.
- Bron / installer: https://0bu.github.io/tesla-key-esp32/

### 2.2 Werkende test-sketch

Er is een standalone test-sketch gemaakt en succesvol getest op een ESP32-C6 (4 MB flash):

- Bestandsnaam (gebruiker): `TeslaChargeTest_24sep26_1400.ino` (varianten)
- Toont live `charge_state` data
- Knoppen: start/stop laden, wake-up, set ampère, open/sluit laadpoort
- Range omgerekend van miles → km
- Raw JSON-blok om exact te zien wat de proxy teruggeeft
- Climate / Vehicle / Tyres blijven vaak leeg (Charging Manager + Infotainment alleen als auto wakker is) → voor smart charging niet essentieel

**Belangrijke API-calls (tesla-key-esp32):**

| Actie | Methode | Pad |
|-------|---------|-----|
| Data | GET | `/api/1/vehicles/{VIN}/vehicle_data` |
| Start laden | POST | `/api/1/vehicles/{VIN}/command/charge_start` |
| Stop laden | POST | `/api/1/vehicles/{VIN}/command/charge_stop` |
| Ampère | POST | `/api/1/vehicles/{VIN}/command/set_charging_amps` body `{"charging_amps":N}` |
| Wake | POST | `/api/1/vehicles/{VIN}/command/wake_up` |
| Laadpoort open/dicht | POST | `.../command/charge_port_door_open` / `charge_port_door_close` |

JSON: vaak genest als `response.response.charge_state` (soms één niveau minder).

### 2.3 Energy-controller (bestaand)

Firmware: `ESP32_C6_ENERGY_v1_27.ino`  
Hardware: ESP32-C6 + Zarlar 32-pin shield (eigen pinout), S0-meters, P1 (HomeWizard), WS2812B 12×4 matrix, EPEX via Raspberry Pi.

Meet o.a.:
- `w_sol` – zonneproductie (W)
- `w_schf` / `w_schr` – schuur afname / injectie
- `w_won` – woning (P1 of simulatie)
- `epex_nu` – actuele all-in prijs (ct/kWh)
- Maandpiek, dagcumulatieven in NVS
- Simulatievlaggen `SIM_S0` en `SIM_P1` (handmatig, opgeslagen in NVS)

Web-UI op vast IP (typisch `192.168.0.73`), JSON op `/json`, settings, OTA.

**Let op:** de energy-sketch is gemaakt voor **16 MB flash** met custom `partitions_16mb.csv`. De test-sketch draait op **4 MB**. Bij integratie partition scheme respecteren.

### 2.4 Bewuste scope-beperking

Alleen **smart charging** (SoC, laadstatus, ampère, power, start/stop).  
Climate / tyres / closures niet verplicht in de UI.

---

## 3. Wat de gebruiker nu wil

Integratie in **`ESP32_C6_ENERGY_v1_27.ino`**:

### 3.1 UI-uitbreiding

- Sectie (of pagina) met Tesla-data: SoC, laadstatus, actuele A, power, charge limit, range (km), eventueel poortstatus
- Bedieningsknoppen: Start laden, Stop laden, Wake-up, Ampère instellen (slider of input)
- Duidelijke status of de proxy bereikbaar is / auto wakker is

### 3.2 Energy Management – actieve sturing

Het EMS moet de Tesla **effectief sturen**:

- Laden bij **energieoverschot** (solar surplus t.o.v. verbruik)
- Optioneel meewegen van **lage EPEX-prijs**
- **Maandpiek** niet overschrijden
- Werkt ook in **simulatiemode** (`SIM_S0` / `SIM_P1`), zodat logica getest kan worden zonder live meters
- Hysterese / minimale wijzigingsdrempel (bijv. alleen ampère aanpassen bij ≥ 2 A verschil) om flappen te voorkomen
- Beperkte commandofrequentie (bijv. max elke 15–30 s) zodat de auto niet onnodig wakker blijft

Voorbeeldrichting surplus:

```text
surplus_w ≈ w_sol - (relevante afname) + injectie
target_amps = constrain(surplus_w / 230, 5, max_amps)   // 1-fase; of /690 bij 3-fase
```

Exacte formule moet aansluiten op de bestaande netto-/pieklogica in de sketch.

### 3.3 Registratie zonnestroom naar Tesla (“tesla1”)

Bijhouden hoeveel de Tesla uit **zonne-overschot** geladen heeft:

- **Per dag** (kWh)
- **Totaal** (kWh, persistent in NVS)

Logica-idee:
- Als de auto aan het laden is én er is surplus (of een deel van het laadvermogen kan aan solar toegeschreven worden), cumuleer `Wh_solar_to_tesla`.
- Midnight: dagteller resetten; totaal behouden.
- Tonen in UI en eventueel in `/json`.

Naam in systeem: **tesla1** (voorbereid op eventueel later tesla2).

### 3.4 Configuratie

Instelbaar (web settings + NVS), o.a.:
- IP/host van tesla-key-esp32
- VIN
- Max laadstroom
- Of automatische sturing aan/uit
- Eventueel min. surplus of EPEX-drempel

---

## 4. Technische aandachtspunten

1. **Partition / flash:** energy-sketch = 16 MB layout; test-sketch = 4 MB. Geen `partitions_16mb.csv` meenemen naar een 4 MB-board.
2. **Libraries:** WiFi, HTTPClient, ArduinoJson, (Async)WebServer zoals in de energy-sketch al gebruikt.
3. **Wake-up:** eerste commando na slaap kan 5–15 s duren; retry/logica voorzien.
4. **Charging Manager:** geen deuren ontgrendelen; laden + status lezen is het doel.
5. **Simulatie:** surplus en “solar→Tesla”-telling moeten ook met `SIM_S0`/`SIM_P1` zinvol werken (gesimuleerde vermogens gebruiken).
6. Gebruiker ontwikkelt verder bij voorkeur met **Claude**; dit document is de overdracht.

---

## 5. Test-sketch (referentie-implementatie)

Onderstaande sketch is de werkende testversie (range in km + Raw JSON). Gebruik als referentie voor HTTP-calls, JSON-parsing en UI-patronen — niet 1-op-1 plakken in de energy-sketch, maar de logica hergebruiken.

```cpp
/*
 * Tesla Test Controller – ESP32-C6
 * Versie: 24 sep 2026 – range in km + Raw JSON
 * Referentie voor integratie in ESP32_C6_ENERGY_v1_27.ino
 *
 * Libraries: ESPAsyncWebServer, AsyncTCP, ArduinoJson
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ========== AANPASSEN ==========
const char* WIFI_SSID      = "jouw-wifi";
const char* WIFI_PASS      = "jouw-wachtwoord";
const char* TESLA_KEY_HOST = "192.168.0.xx";        // of tesla-key-esp32.local
const char* TESLA_VIN      = "5YJ3E7EA0JF000000";   // jouw VIN
// ==============================

AsyncWebServer server(80);

String  lastJson   = "{}";
String  lastError  = "";
unsigned long lastFetch = 0;

String httpGet(const String& path) {
  HTTPClient http;
  String url = String("http://") + TESLA_KEY_HOST + path;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  if (code != 200) lastError = "GET " + path + " → HTTP " + String(code);
  http.end();
  return body;
}

bool httpPost(const String& path, const String& body = "") {
  HTTPClient http;
  String url = String("http://") + TESLA_KEY_HOST + path;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(12000);
  int code = http.POST(body);
  bool ok = (code == 200);
  if (!ok) lastError = "POST " + path + " → HTTP " + String(code);
  http.end();
  return ok;
}

void fetchVehicleData() {
  String path = String("/api/1/vehicles/") + TESLA_VIN + "/vehicle_data";
  String body = httpGet(path);
  if (body.length() > 20) {
    lastJson = body;
    lastError = "";
  }
  lastFetch = millis();
}

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Tesla Test Controller</title>
  <style>
    body{font-family:system-ui,Arial,sans-serif;margin:0;background:#f0f2f5;color:#222}
    .hdr{background:#cc0000;color:#fff;padding:14px 18px;font-size:18px;font-weight:600}
    .card{background:#fff;margin:12px;border-radius:10px;padding:14px 16px;box-shadow:0 2px 8px rgba(0,0,0,.08)}
    h2{margin:0 0 10px;font-size:13px;color:#666;text-transform:uppercase;letter-spacing:.4px}
    .grid{display:grid;grid-template-columns:1fr 1fr;gap:6px 14px}
    .grid3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px 10px}
    .label{color:#666;font-size:12px}
    .value{font-size:16px;font-weight:600}
    .btn{display:inline-block;padding:9px 16px;margin:4px 5px 4px 0;border:none;border-radius:6px;
         font-size:13px;font-weight:600;cursor:pointer;color:#fff}
    .btn-green{background:#2a8a3e}.btn-red{background:#c00}.btn-blue{background:#036}.btn-gray{background:#555}
    .btn:active{opacity:.85}
    input[type=range]{width:100%;max-width:260px}
    .status{font-size:12px;color:#666;margin-top:6px}
    .err{color:#c00;font-weight:600}
    #ampsVal{font-weight:700;color:#036}
    .badge{display:inline-block;padding:2px 7px;border-radius:4px;font-size:11px;font-weight:600}
    .ok{background:#d4edda;color:#155724}.bad{background:#f8d7da;color:#721c24}
    pre.raw{background:#1e1e1e;color:#d4d4d4;padding:12px;border-radius:8px;font-size:11px;
            overflow:auto;max-height:420px;white-space:pre-wrap;word-break:break-all}
  </style>
</head>
<body>
  <div class="hdr">Tesla Test Controller <small style="font-weight:400;opacity:.8">– range km + raw JSON</small></div>

  <div class="card">
    <h2>Charge</h2>
    <div class="grid">
      <div><div class="label">Batterij</div><div class="value" id="soc">—</div></div>
      <div><div class="label">Usable</div><div class="value" id="usable">—</div></div>
      <div><div class="label">Laadstatus</div><div class="value" id="state">—</div></div>
      <div><div class="label">Charge limit</div><div class="value" id="limit">—</div></div>
      <div><div class="label">Huidige A</div><div class="value" id="amps">—</div></div>
      <div><div class="label">Requested A</div><div class="value" id="reqAmps">—</div></div>
      <div><div class="label">Vermogen</div><div class="value" id="power">—</div></div>
      <div><div class="label">Spanning</div><div class="value" id="volt">—</div></div>
      <div><div class="label">Fasen</div><div class="value" id="phases">—</div></div>
      <div><div class="label">Energy added</div><div class="value" id="energy">—</div></div>
      <div><div class="label">Minuten tot vol</div><div class="value" id="ttf">—</div></div>
      <div><div class="label">Range</div><div class="value" id="range">—</div></div>
      <div><div class="label">Laadpoort</div><div class="value" id="port">—</div></div>
      <div><div class="label">Latch</div><div class="value" id="latch">—</div></div>
    </div>
  </div>

  <div class="card">
    <h2>Climate</h2>
    <div class="grid">
      <div><div class="label">Binnen</div><div class="value" id="inside">—</div></div>
      <div><div class="label">Buiten</div><div class="value" id="outside">—</div></div>
      <div><div class="label">Driver set</div><div class="value" id="drvTemp">—</div></div>
      <div><div class="label">Passenger set</div><div class="value" id="pasTemp">—</div></div>
      <div><div class="label">Climate aan</div><div class="value" id="climateOn">—</div></div>
      <div><div class="label">Auto conditioning</div><div class="value" id="autoCond">—</div></div>
    </div>
  </div>

  <div class="card">
    <h2>Vehicle / Closures</h2>
    <div class="grid">
      <div><div class="label">Vergrendeld</div><div class="value" id="locked">—</div></div>
      <div><div class="label">User present</div><div class="value" id="user">—</div></div>
      <div><div class="label">Asleep</div><div class="value" id="asleep">—</div></div>
      <div><div class="label">Odometer</div><div class="value" id="odo">—</div></div>
      <div><div class="label">Frunk</div><div class="value" id="frunk">—</div></div>
      <div><div class="label">Trunk</div><div class="value" id="trunk">—</div></div>
    </div>
  </div>

  <div class="card">
    <h2>Tyre pressure (bar)</h2>
    <div class="grid3">
      <div><div class="label">FL</div><div class="value" id="tpFL">—</div></div>
      <div><div class="label">FR</div><div class="value" id="tpFR">—</div></div>
      <div><div class="label">RL</div><div class="value" id="tpRL">—</div></div>
      <div><div class="label">RR</div><div class="value" id="tpRR">—</div></div>
    </div>
  </div>

  <div class="card">
    <h2>Bediening</h2>
    <button class="btn btn-green" onclick="cmd('charge_start')">▶ Start laden</button>
    <button class="btn btn-red"   onclick="cmd('charge_stop')">■ Stop laden</button>
    <button class="btn btn-blue"  onclick="cmd('wake_up')">☀ Wake-up</button>
    <br><br>
    <label>Laadstroom: <span id="ampsVal">16</span> A</label><br>
    <input type="range" id="ampsSlider" min="5" max="32" value="16"
           oninput="document.getElementById('ampsVal').textContent=this.value">
    <br>
    <button class="btn btn-blue" onclick="setAmps()">Zet ampère</button>
    <br><br>
    <button class="btn btn-gray" onclick="cmd('charge_port_door_open')">Open laadpoort</button>
    <button class="btn btn-gray" onclick="cmd('charge_port_door_close')">Sluit laadpoort</button>
  </div>

  <div class="card">
    <div class="status" id="ts">Laatste update: —</div>
    <div class="status err" id="err"></div>
  </div>

  <div class="card">
    <h2>Raw JSON (proxy response)</h2>
    <pre class="raw" id="rawJson">—</pre>
  </div>

<script>
function num(v, dec=1, unit='') {
  if (v === null || v === undefined || v === '—') return '—';
  return Number(v).toFixed(dec) + (unit ? ' '+unit : '');
}
function boolBadge(v) {
  if (v === true)  return '<span class="badge ok">ja</span>';
  if (v === false) return '<span class="badge bad">nee</span>';
  return '—';
}

function refresh() {
  fetch('/data').then(r=>r.json()).then(d=>{
    if (d.error) {
      document.getElementById('err').textContent = d.error;
      document.getElementById('rawJson').textContent = JSON.stringify(d, null, 2);
      return;
    }
    document.getElementById('err').textContent = '';
    document.getElementById('rawJson').textContent = JSON.stringify(d, null, 2);

    const cs = d.charge_state || {};
    const cl = d.climate_state || {};
    const vs = d.vehicle_state || d.closures_state || {};
    const tp = d.tire_pressure_state || d.tyre_pressure_state || {};

    document.getElementById('soc').textContent    = (cs.battery_level ?? '—') + ' %';
    document.getElementById('usable').textContent = (cs.usable_battery_level ?? '—') + ' %';
    document.getElementById('state').textContent  = cs.charging_state || '—';
    document.getElementById('limit').textContent  = (cs.charge_limit_soc ?? '—') + ' %';
    document.getElementById('amps').textContent   = (cs.charge_amps ?? cs.charger_actual_current ?? '—') + ' A';
    document.getElementById('reqAmps').textContent= (cs.charge_current_request ?? '—') + ' A';
    document.getElementById('power').textContent  = (cs.charger_power ?? '—') + ' kW';
    document.getElementById('volt').textContent   = (cs.charger_voltage ?? '—') + ' V';
    document.getElementById('phases').textContent = cs.charger_phases ?? '—';
    document.getElementById('energy').textContent = num(cs.charge_energy_added, 2, 'kWh');
    document.getElementById('ttf').textContent    = cs.minutes_to_full_charge ?? '—';

    const rangeMi = cs.battery_range ?? cs.ideal_battery_range;
    document.getElementById('range').textContent = rangeMi != null
      ? num(rangeMi * 1.60934, 1, 'km') : '—';

    document.getElementById('port').innerHTML     = boolBadge(cs.charge_port_door_open);
    document.getElementById('latch').textContent  = cs.charge_port_latch || '—';

    document.getElementById('inside').textContent  = num(cl.inside_temp, 1, '°C');
    document.getElementById('outside').textContent = num(cl.outside_temp, 1, '°C');
    document.getElementById('drvTemp').textContent = num(cl.driver_temp_setting, 1, '°C');
    document.getElementById('pasTemp').textContent = num(cl.passenger_temp_setting, 1, '°C');
    document.getElementById('climateOn').innerHTML = boolBadge(cl.is_climate_on);
    document.getElementById('autoCond').innerHTML  = boolBadge(cl.is_auto_conditioning_on);

    document.getElementById('locked').innerHTML  = boolBadge(vs.locked);
    document.getElementById('user').innerHTML    = boolBadge(vs.is_user_present);
    document.getElementById('asleep').innerHTML  = boolBadge(d.is_asleep ?? vs.is_asleep);
    const odo = vs.odometer;
    document.getElementById('odo').textContent   = odo != null ? num(odo * 1.60934, 1, 'km') : '—';
    document.getElementById('frunk').innerHTML   = boolBadge(vs.ft ?? vs.front_trunk);
    document.getElementById('trunk').innerHTML   = boolBadge(vs.rt ?? vs.rear_trunk);

    document.getElementById('tpFL').textContent = num(tp.front_left  ?? tp.fl, 2);
    document.getElementById('tpFR').textContent = num(tp.front_right ?? tp.fr, 2);
    document.getElementById('tpRL').textContent = num(tp.rear_left   ?? tp.rl, 2);
    document.getElementById('tpRR').textContent = num(tp.rear_right  ?? tp.rr, 2);

    document.getElementById('ts').textContent = 'Laatste update: ' + new Date().toLocaleTimeString('nl-BE');
  }).catch(e=>{
    document.getElementById('err').textContent = 'Kan data niet ophalen';
    document.getElementById('rawJson').textContent = String(e);
  });
}

function cmd(name) {
  fetch('/cmd?c=' + name, {method:'POST'})
    .then(r=>r.text())
    .then(t=>{ alert(t); setTimeout(refresh, 2500); })
    .catch(()=>alert('Fout bij commando'));
}

function setAmps() {
  const a = document.getElementById('ampsSlider').value;
  fetch('/cmd?c=set_charging_amps&amps=' + a, {method:'POST'})
    .then(r=>r.text())
    .then(t=>{ alert(t); setTimeout(refresh, 2500); })
    .catch(()=>alert('Fout bij ampère'));
}

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Tesla Test Controller (km + raw JSON) ===");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi verbinden");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (millis() - lastFetch > 4000) fetchVehicleData();

    StaticJsonDocument<8192> doc;
    DeserializationError err = deserializeJson(doc, lastJson);

    if (err || lastError.length()) {
      String e = lastError.length() ? lastError : String("JSON parse error");
      req->send(200, "application/json", "{\"error\":\"" + e + "\"}");
      return;
    }

    JsonObject root = doc["response"]["response"];
    if (root.isNull()) root = doc["response"];
    if (root.isNull()) root = doc.as<JsonObject>();

    String out;
    serializeJson(root, out);
    req->send(200, "application/json", out);
  });

  server.on("/cmd", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasArg("c")) {
      req->send(400, "text/plain", "Missing command");
      return;
    }
    String cmd = req->arg("c");
    String path = String("/api/1/vehicles/") + TESLA_VIN + "/command/" + cmd;
    String body = "";

    if (cmd == "set_charging_amps") {
      int amps = req->hasArg("amps") ? req->arg("amps").toInt() : 16;
      amps = constrain(amps, 0, 48);
      body = "{\"charging_amps\":" + String(amps) + "}";
    }

    bool ok = httpPost(path, body);
    req->send(200, "text/plain", ok ? ("OK: " + cmd) : ("FOUT: " + lastError));
  });

  server.begin();
  fetchVehicleData();
  Serial.println("Klaar → http://" + WiFi.localIP().toString());
}

void loop() {
  delay(1000);
}
```

---

## 6. Suggestie voor Claude – werkwijze

1. Gebruiker levert de **volledige actuele** `ESP32_C6_ENERGY_v1_27.ino`.
2. Voeg modulaire Tesla-laag toe (config, HTTP client, state struct, NVS keys voor solar→tesla1 dag/totaal).
3. UI: sectie “Tesla1” op statuspagina + settings voor host/VIN/max A/auto-sturing.
4. In de bestaande 5 s- of 15 s-tick: surplus berekenen → `setTeslaAmps` / start/stop met hysterese.
5. Parallel: als charging + surplus → `wh_solar_tesla1_day` / `_total` ophogen.
6. Simulatiemode: dezelfde stuurlogica op gesimuleerde `w_sol` e.d.
7. Niet de hele test-HTML overnemen; wel de bewezen API- en parse-logica.

---

## 7. Referenties

| Item | Link / bestand |
|------|----------------|
| tesla-key-esp32 | https://github.com/0Bu/tesla-key-esp32 |
| Web installer | https://0bu.github.io/tesla-key-esp32/ |
| Energy-sketch | `ESP32_C6_ENERGY_v1_27.ino` (door gebruiker aan te leveren) |
| Test-sketch | Zie sectie 5 hierboven |
| Projectcontext | Solar ±11.950 kWh/j, net ±4.950 kWh/j, 2 EV-laders, WP, was/droog, … |

---

*Einde overnamedocument – klaar voor Claude.*
```

