# Smart Energy Management – ESP32-C6

Combineer solar opbrenst (Gem 11.950 kWh), electriciteits-nettarieven (Gem. verbruik: 4950 kWh), verbruikers (2 EV laders, 2 Warmtepompen, 2 Was- en 2 droogmachines...) op de meest optimale wijze, zodat onze energiekosten minimaal worden.

- In ons projectdocument "Energy_Management_System — Zarlardinge.md" staan alle planningdetails.
- In het controller document "Energy_Management_System.md" staan alle conceptdetails

---

**Zarlar Smart Energy Controller**  
Firmware: `ESP32_C6_ENERGY_v1_27.ino`  
Controller-ID: `S-ENERGY`  
IP (vast): `192.168.0.73`

---

## 1. Overzicht

Dit project meet en visualiseert in real-time de energiestromen van een woning en bijgebouw (schuur), zodat later slim gestuurd kan worden op:

- zonneproductie
- eigen verbruik
- injectie / afname van het net
- dynamische EPEX-tarieven
- piekvermogen (maandpiek)

Het doel is de energiekosten te minimaliseren door verbruikers (EV-laders, warmtepompen, wasmachines, drogers, …) op de meest optimale momenten aan te sturen — rekening houdend met solar-overschot, lage nettarieven en de maandpiek.

### Kerncijfers (projectcontext)

| Parameter              | Waarde          |
|------------------------|-----------------|
| Gemiddelde solaropbrengst | ± 11.950 kWh/jaar |
| Gemiddeld netverbruik  | ± 4.950 kWh/jaar |
| Verbruikers            | 2× EV-lader, 2× warmtepomp, 2× wasmachine, 2× droger, … |

### Documentatie

| Document | Inhoud |
|----------|--------|
| `Zarlardinge_Smart_Energy_Management.md` | Planning & projectdetails |
| `Energy Management System.md` | Conceptuele uitwerking van het EMS |
| Deze README | Technische beschrijving van de controller-firmware |

---

## 2. Wat doet de controller?

De ESP32-C6 fungeert als **lokale energie-gateway**:

1. **Meet** continu drie S0-kanalen (zonnepanelen + schuur afname/injectie).
2. **Pollt** (of simuleert) de digitale meter via een HomeWizard P1-dongle (woning).
3. **Haalt** elke 15 minuten de actuele EPEX-prijzen op via een Raspberry Pi.
4. **Toont** de status op een 12×4 WS2812B LED-matrix.
5. **Serveert** een webinterface + JSON-API voor monitoring en configuratie.
6. **Bewaart** dagcumulatieven en maandpiek in NVS (non-volatile storage).

Later wordt hier de **Tesla-laadsturing** (en eventueel andere verbruikers) aan gekoppeld, zodat laden automatisch gebeurt bij surplus of lage EPEX-prijs.

---

## 3. Hardware

### 3.1 Controller

| Onderdeel | Specificatie |
|-----------|--------------|
| MCU | ESP32-C6 (32-pin, RISC-V) |
| Shield | Zarlar custom shield |
| Vast IP | 192.168.0.73 |
| Voeding matrix | Externe 5 V / 2 A (niet via shield-PTC) |

### 3.2 Energiemeting – S0 (schuur + solar)

Drie **Inepro PRO380-S** kWh-meters met S0-pulsuitgang:

| Kanaal | GPIO | Meter | Richting | Klemmen |
|--------|------|-------|----------|---------|
| Solar productie | IO5 | A14 | Forward | 18 / 19 |
| Schuur afname | IO6 | A5 | Forward | 18 / 19 |
| Schuur injectie | IO7 | A5 | Reverse | 20 / 21 |

**S0-interface (universeel printje per kanaal):**

```
3,3 V ─[4,7 kΩ]─┬─ GPIO (INPUT, geen interne pull-up)
                [1 kΩ] serie
                 │
                S0+ 
S0– ──────────────── GND
```

**Roomsense RJ45-pinout** (naar S0-printjes):

| Pin | Functie |
|-----|---------|
| 1 | GND |
| 2 | 3,3 V |
| 3 | IO5 (Solar) |
| 4 | IO6 (SCH afname) |
| 5 | IO7 (SCH injectie) |
| 6 | vrij |
| 7 | GND |
| 8 | 5 V |

**Conversie:** 1 puls = 0,1 Wh → vermogen = `360000 / interval_ms` (W).

### 3.3 Energiemeting – P1 (woning)

| Item | Detail |
|------|--------|
| Dongle | HomeWizard P1 Meter HWE-P1-RJ12 |
| Aansluiting | RJ12 op P1-poort digitale meter (WON) |
| API | `GET http://<P1_IP>/api/v1/data` (Local API moet aan staan) |
| Poll-interval | 5 seconden |
| Status | Tot ±2028 nog in **simulatie** (analoge meter); daarna live |

Relevante JSON-velden:

- `active_power_w` → momentaan vermogen (+ afname, − injectie)
- `total_power_import_t1_kwh` + `t2_kwh` → cumulatieve import
- `total_power_export_t1_kwh` + `t2_kwh` → cumulatieve export

### 3.4 LED-matrix

| Parameter | Waarde |
|-----------|--------|
| Type | WS2812B |
| Formaat | 12 kolommen × 4 rijen = 48 pixels |
| Data-pin | IO4 (via 330 Ω) |
| Layout | Verticale serpentine (rechts → links, kolom per kolom) |
| Default helderheid | 55 / 255 |

**Kolomindeling (v1.27 – EPEX-label ontwerp):**

| Col | Betekenis | Kleurlogica |
|-----|-----------|-------------|
| 0 | Solar vermogen (0–6 kW) | Groen |
| 1 | SCH afname (0–10 kW) | Rood |
| 2 | SCH injectie (0–6 kW) | Groen |
| 3 | WON afname (0–10 kW) | Rood |
| 4 | WON injectie (0–6 kW) | Groen (dim amber zolang gesimuleerd) |
| 5 | Maandpiek-status (t.o.v. limiet) | Groen onder limiet, rood daarboven |
| 6 | EPEX all-in prijs (0–40 ct/kWh) | Kleurgradiënt (cyaan → groen → geel → oranje → rood) |
| 7 | Huishoudadvies (goed moment?) | Groen = goed, rood = duur |
| 8 | Batterij (toekomst) | Dim paars = gereserveerd |
| 9 | Heap (ESP32 geheugen) | Groen / geel / rood |
| 10 | WiFi RSSI | Groen / geel / rood |
| 11 | SIM-status (S0 + P1) | Rood knipper = simulatie, groen = live |

### 3.5 Netwerk & externe diensten

| Component | Rol |
|-----------|-----|
| Raspberry Pi (`192.168.0.50:3000`) | EPEX-prijzen via `/api/epex` |
| NTP | `pool.ntp.org` (tijdzone UTC+1, met zomertijd-correctie in firmware) |
| WiFi | Station-modus; bij mislukte verbinding → SoftAP `ZarlarSetup` |

---

## 4. Firmware – functionele beschrijving

### 4.1 Meetsystemen

**S0 (live of simulatie)**

- Interrupts op IO5 / IO6 / IO7 (FALLING edge).
- Elke 5 s: delta-pulsen → Wh, interval → W.
- Timeout: geen pulse meer dan 3 min → vermogen = 0.
- Simulatie (`SIM_S0`): realistisch april-profiel België (solar sinus 07–19 u, ochtend/avond pieken in schuurverbruik).

**P1 (live of simulatie)**

- HTTP GET naar HomeWizard Local API.
- Dagcumulatieven via delta t.o.v. midnight-snapshot.
- Simulatie (`SIM_P1`): typisch huishoudelijk verbruiksprofiel.

Beide simulatievlaggen zijn **bewust handmatig** (web-UI of serial) en worden in NVS opgeslagen. Nooit automatisch omschakelen.

### 4.2 EPEX

- Elke 15 minuten: `GET http://192.168.0.50:3000/api/epex`
- All-in prijs = spotprijs × 0,1 + vaste opslag (`VAST_CT_KWH` = 14,32 ct – Fluvius Imewo + Ecopower).
- Opgeslagen: huidige uur + volgende uur.

### 4.3 Opslag (NVS)

| Key | Inhoud |
|-----|--------|
| `wh_sol` / `wh_schf` / `wh_schr` | Dagcumulatieven (Wh) |
| `piek_w` | Maandpiek (W) |
| `max_piek_w` | Instelbare limiet (default 15 kW) |
| `sim_s0` / `sim_p1` | Simulatievlaggen |
| `p1_ip` | IP van HomeWizard |
| `wifi_ssid` / `wifi_pass` / `static_ip` | Netwerk |
| `bright` | LED-helderheid |

Midnight-reset: dag-Wh naar 0; op de 1e van de maand ook de piek.

### 4.4 Webinterface

| Pad | Functie |
|-----|---------|
| `/` | Statuspagina (live waarden, auto-refresh 5 s) |
| `/json` | Compacte JSON voor externe systemen |
| `/settings` | WiFi, helderheid, max-piek, S0/P1-simulatie, P1-IP |
| `/update` | OTA firmware-upload |
| `/save_settings` | Opslaan + reboot |
| `/factory_reset` | NVS wissen |
| `/reset_dag` / `/reset_piek` | Handmatige resets |

**JSON-velden (`/json`):**

```
a  = Solar W
b  = WON W (P1)
c  = SCH afname W
d  = SCH injectie W
e  = Netto SCH W
h  = Solar dag Wh
i  = WON dag afname Wh
j  = SCH afname dag Wh
k  = SCH injectie dag Wh
vw = WON dag injectie Wh
n  = EPEX nu (ct × 100)
n2 = EPEX +1u (ct × 100)
pt = Maandpiek W
ac = WiFi RSSI
ae = Largest free heap block
sim_s0 / sim_p1 = 0 of 1
ver = firmwareversie
```

### 4.5 Serial-commando’s

```
status          → actuele waarden
sim s0 on/off   → S0 simulatie
sim p1 on/off   → P1 simulatie
reset_nvs       → factory reset
help
```

### 4.6 Partitieschema (16 MB flash)

```
nvs      0x9000    0x5000
otadata  0xe000    0x2000
app0     0x10000   0x600000
app1     0x610000  0x600000
spiffs   0xC10000  0x3F0000
```

Compileer met `partitions_16mb.csv` in de schetsmap.

---

## 5. Bedoeling & roadmap

### Huidige fase (v1.27)

- Betrouwbare meting van solar + schuur + (gesimuleerde) woning.
- Duidelijke visuele feedback via LED-matrix.
- EPEX-prijs als beslissingsinput.
- Stabiele web-API voor integratie.

### Volgende stappen

1. **Tesla-laadsturing** via `tesla-key-esp32` (BLE)  
   - Surplus → laadstroom  
   - Lage EPEX → laden toestaan  
   - Maandpiek respecteren  

2. Andere grote verbruikers (warmtepompen, was/droog) op basis van dezelfde logica.

3. Optioneel: ntfy.sh-push bij piekoverschrijding, automatische DST, vaste opslag dynamisch ophalen.

Zie ook de aparte documentatie voor de Tesla-testsketch (`Charge.test.ino`) en de geplande integratie.

---

## 6. Installatie & gebruik

1. Open de sketch in Arduino IDE (ESP32-C6 board package).
2. Zet partition scheme op de 16 MB-tabel hierboven.
3. Vul eventueel default WiFi in, of configureer later via SoftAP `ZarlarSetup`.
4. Upload.
5. Open `http://192.168.0.73` (of het IP dat de SoftAP toont).
6. Zet `SIM_S0` / `SIM_P1` uit zodra de hardware aangesloten is.

**Belangrijk:** simulatie start standaard **aan**. Schakel bewust om naar live na controle van de bekabeling.

---

## 7. Bestanden & versiehistorie (kort)

| Versie | Datum | Wijziging |
|--------|-------|-----------|
| v1.24 | 24 apr 2026 | Eerste productieversie – live S0 ISR |
| v1.25 | 25 apr 2026 | Enkelvoudige SIMULATION_MODE |
| v1.26 | 25 apr 2026 | Twee onafhankelijke simulatievlaggen (S0 + P1) |
| v1.27 | 28 apr 2026 | Matrix-layout definitief (EPEX-label) |

---

## 8. Licentie & credits

- Firmware: Filip Delannoy / Zarlar (april 2026)
- Hardware: Zarlar shield + Roomsense RJ45 + Inepro PRO380-S + HomeWizard P1
- Toekomstige Tesla-sturing: gebaseerd op [tesla-key-esp32](https://github.com/0Bu/tesla-key-esp32) (AGPL-3.0)

---

*Laatste update van deze README: september 2026*
```
