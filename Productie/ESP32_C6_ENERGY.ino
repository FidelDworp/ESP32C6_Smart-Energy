// ESP32_C6_ENERGY_v1_28.ino — Zarlar Smart Energy Controller
// Developed by Filip Delannoy, april 2026.
// Bereikbaar op http://192.168.0.73
//
// PARTITIETABEL: Compileer met "partitions_16mb.csv" in de schetsmap
//   nvs,     data, nvs,   0x9000,   0x5000,
//   otadata, data, ota,   0xe000,   0x2000,
//   app0,    app,  ota_0, 0x10000,  0x600000,
//   app1,    app,  ota_1, 0x610000, 0x600000,
//   spiffs,  data, spiffs,0xC10000, 0x3F0000,
//
// ── VERSIEHISTORIE ──────────────────────────────────────────
// 24sep26 v1.28 Tesla1: pagina /tesla — data volgen + handmatig start/stop/wake/ampère
//               via proxy tesla-key-esp32 (BLE). Eigen minimale HTTP-client (geen
//               HTTPClient/String), stream-parse met ArduinoJson-filter. Geen auto-sturing.
//               Fix: middernacht-reset elke 5s-tick + dag in NVS (was 3-minutenvenster
//               in het 15-min-blok, ~1 op 5 kans).
//               Fix: automatische zomertijd (configTzTime CET/CEST) i.p.v. vaste UTC+2.
//               TODO-blok onderaan verplaatst naar OPENSTAAND hieronder.
// 28apr26 v1.27 Matrix layout definitief (EPEX-label 27 april)
// 25apr26 v1.26 Twee onafhankelijke simulatievlaggen (SIM_S0 / SIM_P1)
// 25apr26 v1.25 Enkelvoudige SIMULATION_MODE (archief)
// 24apr26 v1.24 Eerste productieversie — live S0 ISR
//
// ── OPENSTAAND ──────────────────────────────────────────────
//  - ntfy.sh push bij piekdrempel overschreden
//  - Vaste opslag ophalen via /api/settings (nu hardcoded VAST_CT_KWH)
//  - fetchP1(): imp_midnight/exp_midnight-snapshot wordt niet bij middernacht vernieuwd
//    -> WON-dagwaarden lopen op in LIVE-modus (relevant vanaf ~2028)
//  - piek_w is momentaan en enkel SCH (geen WON, geen kwartiergemiddelde)
//  - WON individuele piek bijhouden (key pw) zodra P1-dongle actief
//  - Tesla1 later: solar->tesla1 dag/totaal-tellers, t1_-keys in /json (+ GAS/Dashboard),
//    matrix-pixels (col 11 rij 2-3 vrij), automatische sturing (solar/EPEX/piek)
//
// ── HARDWARE ────────────────────────────────────────────────
//   ESP32-C6 32-pin · Zarlar shield · IP 192.168.0.73
//   Roomsense RJ45 → S0 interface printje → 3× Inepro PRO380-S
//   Pixel-line connector → WS2812B matrix 12×4 (48 px, aparte 5V)
//   Voeding: 5V/2A extern (matrix NIET via shield PTC)
//
// S0 interface per kanaal (universeel printje):
//   3,3V ─[4,7kΩ]─┬─ GPIO (INPUT, geen interne pull-up)
//                 [1kΩ] serie
//                  │
//                 S0+ klem 18/20 (Inepro PRO380-S)
//                 S0– klem 19/21 ──── GND
//
// RJ45 Roomsense pinout:
//   Pin 1: GND | Pin 2: 3,3V | Pin 3: IO5 | Pin 4: IO6
//   Pin 5: IO7 | Pin 6: vrij | Pin 7: GND | Pin 8: 5V
//
// S0 kanalen:
//   IO5  Zonnepanelen A14 — forward productie  (klem 18/19)
//   IO6  Schuur A5 — forward afname            (klem 18/19)
//   IO7  Schuur A5 — reverse injectie          (klem 20/21)
//   IO4  WS2812B matrix DIN (via 330Ω, Pixel-line connector)
//
// HomeWizard P1 Meter HWE-P1-RJ12:
//   Model: HWE-P1-RJ12 · 5V 500mA · 2.4GHz WiFi
//   Aansluting: RJ12 op P1-poort digitale meter (WON, ~2028)
//   Activatie: HomeWizard Energy app → Settings → Meters → Local API AAN
//   Endpoint: GET http://<P1_IP>/api/v1/data (plain HTTP, geen auth)
//   Documentatie: https://api-documentation.homewizard.com/docs/introduction/
//   GitHub library: https://github.com/jvandenaardweg/homewizard-energy-api
//   Update: elke seconde (DSMR 5.0), elke 10s (oudere meters)
//   Wij pollen elke 5s (zelfde als S0 tick) — ruim voldoende
//
// WON-verbruik: niet gemeten in fase 1 (analoge meter)
//   SIM_P1=true → gesimuleerde data  (tot ~2028)
//   SIM_P1=false + P1_IP ingesteld → live HomeWizard data (~2028+)

// VERPLICHT voor ESP32-C6 (RISC-V) in Arduino IDE
#define Serial Serial0

#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <math.h>

// ── VERSIE ──────────────────────────────────────────────────
#define FW_VERSION   "1.28"
#define CTRL_ID      "S-ENERGY"
#define NVS_NS       "senrg"

// ── PINS ────────────────────────────────────────────────────
#define S0_SOL_PIN    5
#define S0_SCHF_PIN   6
#define S0_SCHR_PIN   7
#define LED_PIN       4   // IO4 via 330Ω, Pixel-line connector — Zarlar shield

// ── MATRIX 12×4 = 48 pixels ─────────────────────────────────
// Serpentine: rij 0 top L→R, rij 1 R→L, rij 2 L→R, rij 3 bodem R→L
// Labels voor op behuizing (zie ook projectdocument §11.2):
//  Col 0=SOL W   Col 1=SOL kWh  Col 2=SCH AF   Col 3=SCH INJ
//  Col 4=NETTO   Col 5=EPEX     Col 6=EPEX+1   Col 7=PIEK%
//  Col 8=KOKEN?  Col 9=WASSEN?  Col10=HEAP     Col11=WiFi
#define MATRIX_COLS  12
#define MATRIX_ROWS   4
#define NUM_PIXELS   48
#define DEF_BRIGHT   55

// ── ENERGIE ─────────────────────────────────────────────────
#define WH_PER_PULS    0.1f
#define WATT_FACTOR    360000.0f
#define POWER_TO_MS    180000UL
#define MAX_SOL_W      6000.0f
#define MAX_SCH_W     10000.0f
#define MAX_DAG_WH    30000.0f
#define VAST_CT_KWH   14.32f      // vaste opslag Fluvius Imewo + Ecopower

// ── SIMULATIE PROFIELPARAMETERS (april, België) ─────────────
#define SIM_SOLAR_PEAK_W  4000.0f  // W zonnepanelen piek
#define SIM_BASE_LOAD_W    500.0f  // W basis SCH verbruik
#define SIM_WON_BASE_W     400.0f  // W basis WON verbruik
#define SIM_TICK_S           5.0f  // seconden per tik

// ── NETWERK ─────────────────────────────────────────────────
const char* DEF_SSID  = "";
const char* DEF_PASS  = "";
const char* DEF_IP    = "192.168.0.73";
const char* AP_SSID   = "ZarlarSetup";
const char* RPI_BASE  = "http://192.168.0.50:3000";
const char* NTP_SRV   = "pool.ntp.org";
#define     TZ_INFO    "CET-1CEST,M3.5.0,M10.5.0/3"   // CET/CEST, automatische zomertijd

// ── TESLA1 (proxy tesla-key-esp32) ──────────────────────────
#define TESLA_POLL_VIEW_MS   15000UL    // poll-interval terwijl /tesla open staat
#define TESLA_POLL_IDLE_MS  300000UL    // poll-interval als niemand kijkt (5 min)
#define TESLA_POLL_FAIL_MS   60000UL    // na een mislukte poll
#define TESLA_GET_TIMEOUT    10000UL    // BLE-antwoorden kunnen traag zijn
#define TESLA_CMD_TIMEOUT    12000UL    // wake-up kan 5-15 s duren
#define TESLA_AMPS_MIN           5
#define TESLA_AMPS_CAP          32      // harde bovengrens voor de instelling
#define DEF_TESLA_MAXA          16

// ── NVS KEYS ────────────────────────────────────────────────
const char* NVS_SSID     = "wifi_ssid";
const char* NVS_PASS     = "wifi_pass";
const char* NVS_IP       = "static_ip";
const char* NVS_SOL      = "wh_sol";
const char* NVS_SCHF     = "wh_schf";
const char* NVS_SCHR     = "wh_schr";
const char* NVS_PIEK     = "piek_w";
const char* NVS_BRI      = "bright";
const char* NVS_MPIEK    = "max_piek_w";
const char* NVS_SIM_S0   = "sim_s0";   // bool — S0 kanalen simuleren
const char* NVS_SIM_P1   = "sim_p1";   // bool — P1 dongle simuleren
const char* NVS_P1_IP    = "p1_ip";    // string — HomeWizard P1 IP adres
const char* NVS_DAY      = "last_day"; // int — laatst verwerkte dag (YYYYMMDD)
const char* NVS_T_HOST   = "t1_host";  // string — tesla-key-esp32 host/IP
const char* NVS_T_VIN    = "t1_vin";   // string — VIN
const char* NVS_T_MAXA   = "t1_maxa";  // uchar — max laadstroom (A)

// ── OBJECTEN ────────────────────────────────────────────────
AsyncWebServer    server(80);
DNSServer         dnsServer;
Preferences       prefs;
Adafruit_NeoPixel strip(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ── GLOBALS ─────────────────────────────────────────────────
bool          ap_mode      = false;
bool          SIM_S0       = true;   // ⚠️ START IN SIMULATIE — zet UIT na S0 bekabeling
bool          SIM_P1       = true;   // ⚠️ START IN SIMULATIE — zet UIT na P1-dongle (~2028)
wl_status_t   last_wifi_st = WL_IDLE_STATUS;
char          wifi_ssid[64]= "";
char          wifi_pass[64]= "";
char          static_ip[24]= "";
char          p1_ip[24]    = "";     // HomeWizard P1 Meter IP (instelbaar)
uint32_t      max_piek_w   = 15000;  // Default 15kW conform EPEX-label

// ── ISR VARIABELEN (S0 live modus) ──────────────────────────
volatile uint32_t      isr_sol_cnt  = 0, isr_schf_cnt  = 0, isr_schr_cnt  = 0;
volatile unsigned long isr_sol_last = 0, isr_schf_last = 0, isr_schr_last = 0;
volatile unsigned long isr_sol_iv   = 0, isr_schf_iv   = 0, isr_schr_iv   = 0;

// ── WERKDATA — SCH (S0) ──────────────────────────────────────
float    w_sol   = 0, w_schf  = 0, w_schr  = 0, w_netto = 0;
float    wh_sol  = 0, wh_schf = 0, wh_schr = 0;
float    piek_w  = 0;

// ── WERKDATA — WON (P1) ──────────────────────────────────────
float    w_won        = 0;   // W momentaan (+ = afname, − = injectie)
float    wh_won_imp   = 0;   // Wh dag afname WON
float    wh_won_exp   = 0;   // Wh dag injectie WON
// NB: dagcumulatieven WON worden berekend als delta tov NVS-snapshot bij midnight

// ── EPEX ─────────────────────────────────────────────────────
float    epex_nu = 0, epex_p1h = 0;  // all-in ct/kWh

// ── TIMING ──────────────────────────────────────────────────
unsigned long t_5s   = 0;
unsigned long t_15m  = 0;
unsigned long t_epex = 0;
unsigned long t_p1   = 0;
int32_t       last_day  = -1;   // YYYYMMDD laatst verwerkte dag (NVS)
uint32_t      prev_sol = 0, prev_schf = 0, prev_schr = 0;

// ── ISR ─────────────────────────────────────────────────────
void IRAM_ATTR isrSol() {
  unsigned long n = millis();
  if (isr_sol_last)  isr_sol_iv  = n - isr_sol_last;
  isr_sol_last = n; isr_sol_cnt++;
}
void IRAM_ATTR isrSchF() {
  unsigned long n = millis();
  if (isr_schf_last) isr_schf_iv = n - isr_schf_last;
  isr_schf_last = n; isr_schf_cnt++;
}
void IRAM_ATTR isrSchR() {
  unsigned long n = millis();
  if (isr_schr_last) isr_schr_iv = n - isr_schr_last;
  isr_schr_last = n; isr_schr_cnt++;
}

// ── MATRIX HELPERS ──────────────────────────────────────────
// VERTICALE serpentine: rechts→links, kolom-per-kolom
// Oneven cols (11,9,7,5,3,1): onder→boven ↑
// Even cols (10,8,6,4,2,0): boven→onder ↓
inline int pxIdx(int col, int row) {
  int col_from_right = 11 - col;  // Col 11→0, col 10→1, etc.
  int pixel_base = col_from_right * 4;
  
  if (col_from_right % 2 == 0) {
    // Even col_from_right (cols 11,9,7,5,3,1): onder→boven
    return pixel_base + (3 - row);
  } else {
    // Odd col_from_right (cols 10,8,6,4,2,0): boven→onder
    return pixel_base + row;
  }
}

void lightbar(int col, float v, uint32_t cf, uint32_t cd = 0x070707) {
  int n = (int)round(constrain(v, 0.0f, 1.0f) * MATRIX_ROWS);
  for (int r = 0; r < MATRIX_ROWS; r++) {
    int pos = MATRIX_ROWS - 1 - r;
    strip.setPixelColor(pxIdx(col, r), (pos < n) ? cf : cd);
  }
}

uint32_t epexKleur(float ct) {
  if (ct < 0)   return strip.Color(0,  80,  80);
  if (ct < 15)  return strip.Color(0, 110,   0);
  if (ct < 25)  return strip.Color(100, 95,   0);
  if (ct < 35)  return strip.Color(120,  45,  0);
  return               strip.Color(140,   0,  0);
}

// SIM-indicator col 11: BOVENAAN 2 pixels (rij 0+1)
// Rij 0 (bovenaan) = S0 status, Rij 1 = P1 status
void simIndicatorPulse() {
  static bool tog = false; tog = !tog;
  uint32_t rood  = tog ? strip.Color(100, 0, 0) : strip.Color(30, 0, 0);
  uint32_t groen = strip.Color(0, 100, 0);
  
  // S0: rij 0, col 11
  strip.setPixelColor(pxIdx(11, 0), SIM_S0 ? rood : groen);
  // P1: rij 1, col 11
  strip.setPixelColor(pxIdx(11, 1), SIM_P1 ? rood : groen);
}

// ── MATRIX UPDATE ───────────────────────────────────────────
// v1.27: Aangepast naar definitief EPEX-label ontwerp
void updateMatrix() {
  // Col 0: ☀️ SOL (solar vermogen 0-6kW, groen)
  lightbar(0, w_sol / MAX_SOL_W, strip.Color(0, 150, 0));
  
  // Col 1: SCH↓ (SCH afname 0-10kW, rood)
  lightbar(1, w_schf / MAX_SCH_W, strip.Color(180, 0, 0));
  
  // Col 2: SCH↑ (SCH injectie 0-6kW, groen)
  lightbar(2, w_schr / MAX_SOL_W, strip.Color(0, 150, 0));
  
  // Col 3: WON↓ (WON afname 0-10kW, rood)
  // In SIM_P1: toon gesimuleerd verbruik, anders live P1-data
  float won_afname = (w_won > 0) ? w_won : 0;
  lightbar(3, won_afname / MAX_SCH_W, strip.Color(180, 0, 0));
  
  // Col 4: WON↑ (WON injectie 0-6kW, groen)
  // Nog niet beschikbaar tot P1-dongle (~2028)
  float won_injectie = (w_won < 0) ? -w_won : 0;
  if (SIM_P1) {
    // Simulatie: nog geen WON injectie
    lightbar(4, 0, strip.Color(0, 0, 0), strip.Color(20, 10, 0)); // Dim amber = ~2028
  } else {
    lightbar(4, won_injectie / MAX_SOL_W, strip.Color(0, 150, 0));
  }
  
  // Col 5: PIEK (status max 15kW)
  // Groen = onder limiet, Rood gradient = boven limiet
  float piek_pct = piek_w / (float)max_piek_w;
  if (piek_pct <= 1.0f) {
    // Onder limiet: 1 groene pixel onderaan
    strip.setPixelColor(pxIdx(5, 3), strip.Color(0, 120, 0));
    strip.setPixelColor(pxIdx(5, 2), 0x070707);
    strip.setPixelColor(pxIdx(5, 1), 0x070707);
    strip.setPixelColor(pxIdx(5, 0), 0x070707);
  } else {
    // Boven limiet: rode lightbar naar boven
    float over_pct = constrain((piek_pct - 1.0f) / 0.84f, 0, 1); // 1.0-1.84 (max 40A*3*230V/15kW)
    lightbar(5, over_pct, strip.Color(180, 0, 0));
  }
  
  // Col 6: ct/kWh (all-in prijs 0-40ct, kleurgradiënt)
  lightbar(6, constrain(epex_nu / 40.0f, 0, 1), epexKleur(epex_nu));
  
  // Col 7: 🏠 HUIS (huishoudadvies)
  // Groen = goed moment (EPEX < 15ct OF solar > 1.5kW), Rood = duur
  bool goed_moment = (epex_nu < 15.0f) || (w_sol > 1500.0f);
  uint32_t huis_kleur = goed_moment ? strip.Color(0, 140, 0) : strip.Color(160, 0, 0);
  lightbar(7, goed_moment ? 1.0f : 0.25f, huis_kleur);
  
  // Col 8: 🔋 BAT (batterij, toekomst)
  // Nog niet geïmplementeerd — dim paars = gereserveerd
  for (int r = 0; r < MATRIX_ROWS; r++) {
    strip.setPixelColor(pxIdx(8, r), strip.Color(20, 0, 30));
  }
  
  // Col 9: ❤️ HEAP (ESP32 geheugen)
  float heap_pct = constrain((float)ESP.getMaxAllocHeap() / 60000.0f, 0, 1);
  uint32_t heap_kleur = (heap_pct > 0.50f) ? strip.Color(0, 100, 0)
                      : (heap_pct > 0.30f) ? strip.Color(140, 100, 0)
                                           : strip.Color(160, 0, 0);
  lightbar(9, heap_pct, heap_kleur);
  
  // Col 10: WiFi (RSSI -90 tot -60 dBm)
  int rssi = ap_mode ? -99 : WiFi.RSSI();
  float rssi_pct = constrain((rssi + 90.0f) / 30.0f, 0, 1);
  uint32_t wifi_kleur = (rssi >= -60) ? strip.Color(0, 100, 0)
                      : (rssi >= -75) ? strip.Color(140, 100, 0)
                                      : strip.Color(160, 0, 0);
  lightbar(10, rssi_pct, wifi_kleur);
  
  // Col 11: SIM (S0/P1 status, 2 pixels bovenaan)
  // Onderste 2 pixels zwart
  strip.setPixelColor(pxIdx(11, 2), 0);
  strip.setPixelColor(pxIdx(11, 3), 0);
  // Bovenste 2 pixels: sim-indicator
  simIndicatorPulse();
  
  strip.show();
}

// ── VERMOGEN (live S0) ───────────────────────────────────────
float calcW(unsigned long iv, unsigned long last_ms) {
  if (!last_ms || (millis() - last_ms) > POWER_TO_MS || !iv) return 0.0f;
  return WATT_FACTOR / (float)iv;
}

// ── S0 SIMULATIE TICK ────────────────────────────────────────
// Realistisch april-profiel België
// ⚠️ Schakel SIM_S0 UIT zodra echte S0-bekabeling aanwezig is!
void simTickS0() {
  struct tm ti;
  if (!getLocalTime(&ti, 100)) {
    w_sol = 2000.0f; w_schf = 700.0f;
  } else {
    float hour = ti.tm_hour + ti.tm_min / 60.0f + ti.tm_sec / 3600.0f;
    float solar = 0.0f;
    if (hour >= 7.0f && hour <= 19.0f) {
      float fase = (hour - 7.0f) / 12.0f;
      solar = SIM_SOLAR_PEAK_W * sinf(fase * M_PI);
      solar *= 0.90f + (float)(random(0, 200)) / 1000.0f;
      solar = max(0.0f, solar);
    }
    float schf = SIM_BASE_LOAD_W + (float)(random(0, 200));
    if (hour >= 6.5f  && hour <  9.0f)  schf += 1800.0f;
    if (hour >= 11.5f && hour < 13.5f)  schf +=  600.0f;
    if (hour >= 17.0f && hour < 21.0f)  schf += 2200.0f;
    if (hour <  6.0f  || hour >= 23.0f) schf +=  800.0f;
    w_sol  = solar;
    w_schf = schf;
  }
  w_schr  = max(0.0f, w_sol - w_schf);
  w_netto = w_sol - w_schf + w_schr;

  const float DT_H = SIM_TICK_S / 3600.0f;
  wh_sol  += w_sol  * DT_H;
  wh_schf += w_schf * DT_H;
  wh_schr += w_schr * DT_H;

  float afname = w_schf - w_schr;
  if (afname > piek_w) piek_w = afname;
}

// ── P1 SIMULATIE TICK ────────────────────────────────────────
// Realistisch WON-verbruiksprofiel (april, België)
// ⚠️ Schakel SIM_P1 UIT zodra HomeWizard P1-dongle beschikbaar is (~2028)!
void simTickP1() {
  struct tm ti;
  float won = SIM_WON_BASE_W + (float)(random(0, 150));
  if (getLocalTime(&ti, 100)) {
    float hour = ti.tm_hour + ti.tm_min / 60.0f;
    if (hour >= 7.0f  && hour <  9.0f)  won += 1200.0f;
    if (hour >= 12.0f && hour < 13.5f)  won +=  500.0f;
    if (hour >= 17.5f && hour < 21.5f)  won += 1800.0f;
    if (hour <  6.5f  || hour >= 23.0f) won +=  600.0f;
  }
  w_won = won;  // Altijd afname in simulatie (geen eigen productie WON in fase 1)

  const float DT_H = SIM_TICK_S / 3600.0f;
  wh_won_imp += w_won * DT_H;
  // Geen export in WON simulatie (geen solar)
}

// ── S0 LIVE TICK ────────────────────────────────────────────
void liveTickS0() {
  noInterrupts();
  uint32_t      cs  = isr_sol_cnt,  cf  = isr_schf_cnt, cr  = isr_schr_cnt;
  unsigned long ivs = isr_sol_iv,   ivf = isr_schf_iv,  ivr = isr_schr_iv;
  unsigned long ls  = isr_sol_last, lf  = isr_schf_last,lr  = isr_schr_last;
  interrupts();

  wh_sol  += (cs - prev_sol)  * WH_PER_PULS;
  wh_schf += (cf - prev_schf) * WH_PER_PULS;
  wh_schr += (cr - prev_schr) * WH_PER_PULS;
  prev_sol = cs; prev_schf = cf; prev_schr = cr;

  w_sol  = calcW(ivs, ls);
  w_schf = calcW(ivf, lf);
  w_schr = calcW(ivr, lr);
  w_netto = w_sol - w_schf + w_schr;

  float afname = w_schf - w_schr;
  if (afname > piek_w) piek_w = afname;
}

// ── P1 LIVE FETCH ────────────────────────────────────────────
// Pollt HomeWizard P1 Meter HWE-P1-RJ12 via lokale REST API
// API v1: GET http://<P1_IP>/api/v1/data (plain HTTP, geen auth nodig)
// Documentatie: https://api-documentation.homewizard.com/docs/introduction/
// Activatie: HomeWizard Energy app → Settings → Meters → Local API AAN
//
// JSON response (relevante velden):
//   active_power_w          → momentaan nettovermogen (+ = afname, − = injectie)
//   total_power_import_t1_kwh + t2_kwh → totale import (dag via delta berekening)
//   total_power_export_t1_kwh + t2_kwh → totale export (dag via delta berekening)
//
// NB: total_power_*_kwh zijn CUMULATIEVE tellers (niet dag-reset).
//     Dagcumulatief = huidige waarde − snapshot bij midnight (zie checkMidnight)
void fetchP1() {
  if (SIM_P1) return;               // simulatie actief — niet ophalen
  if (strlen(p1_ip) == 0) return;   // geen IP ingesteld
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(4000);
  String url = "http://";
  url += p1_ip;
  url += "/api/v1/data";
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[P1] HTTP fout: %d\n", code);
    http.end(); return;
  }

  // Minimale JsonDocument — alleen de keys die wij nodig hebben
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { Serial.println(F("[P1] JSON fout")); return; }

  // active_power_w: positief = afname, negatief = injectie (DSMR convention)
  float pwr = doc["active_power_w"] | 0.0f;
  w_won = pwr;  // positief = afname WON, negatief = injectie WON

  // Dag-cumulatieven via cumulatieve tellers (delta tov midnight snapshot — zie checkMidnight)
  float imp = (doc["total_power_import_t1_kwh"] | 0.0f)
            + (doc["total_power_import_t2_kwh"] | 0.0f);
  float exp = (doc["total_power_export_t1_kwh"] | 0.0f)
            + (doc["total_power_export_t2_kwh"] | 0.0f);

  // wh_won_imp/exp zijn dag-cumulatieven bijgehouden als delta tov midnight
  // (worden gereset in checkMidnight via NVS snapshot)
  static float imp_midnight = -1, exp_midnight = -1;
  if (imp_midnight < 0) { imp_midnight = imp; exp_midnight = exp; }  // eerste meting na boot
  wh_won_imp = (imp - imp_midnight) * 1000.0f;  // kWh → Wh
  wh_won_exp = (exp - exp_midnight) * 1000.0f;

  Serial.printf("[P1] %.0fW  imp:%.3f exp:%.3f kWh\n", pwr,
    wh_won_imp / 1000.0f, wh_won_exp / 1000.0f);
}

// ── EPEX OPHALEN VIA RPI ─────────────────────────────────────
void fetchEpex() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(6000);
  http.begin(String(RPI_BASE) + "/api/epex");
  int code = http.GET();
  if (code != 200) { Serial.printf("[EPEX] HTTP fout: %d\n", code); http.end(); return; }
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { Serial.println(F("[EPEX] JSON fout")); return; }

  JsonArray unix_arr  = doc["unix_seconds"].as<JsonArray>();
  JsonArray price_arr = doc["price"].as<JsonArray>();
  if (!unix_arr || !price_arr || unix_arr.size() == 0) return;

  time_t now_ts = time(nullptr);
  int idx_nu = 0;
  for (int i = (int)unix_arr.size() - 1; i >= 0; i--) {
    if ((time_t)unix_arr[i].as<long>() <= now_ts) { idx_nu = i; break; }
  }
  int idx_p1 = min(idx_nu + 1, (int)price_arr.size() - 1);
  epex_nu  = price_arr[idx_nu].as<float>() * 0.1f + VAST_CT_KWH;
  epex_p1h = price_arr[idx_p1].as<float>() * 0.1f + VAST_CT_KWH;
  Serial.printf("[EPEX] nu=%.1f ct  +1u=%.1f ct\n", epex_nu, epex_p1h);
}

// ═════════════════════════════════════════════════════════════
// TESLA1 — data volgen + handmatig bedienen (v1.28)
// Via proxy tesla-key-esp32 (BLE, Charging Manager) — REST op poort 80.
// Geen automatische sturing. Heap-zuinig:
//   - eigen minimale HTTP/1.0-client op WiFiClient (geen HTTPClient, geen String)
//   - stream-parse met ArduinoJson-filter: enkel charge_state, ~1,5 KB werkgeheugen op de stack (tijdelijk)
//   - vaste buffers, statische pagina in PROGMEM (send_P)
// Blokkerend in loop() met deadline: S0-ISR blijft pulsen tellen, webserver draait apart.
// ═════════════════════════════════════════════════════════════
const char* const T1_KEYS[] = {
  "battery_level", "usable_battery_level", "charging_state", "charge_limit_soc",
  "charge_amps", "charger_actual_current", "charge_current_request", "charger_power",
  "charger_voltage", "charger_phases", "charge_energy_added", "minutes_to_full_charge",
  "battery_range", "ideal_battery_range", "charge_port_door_open"
};
#define T1_NKEYS  15
static_assert(sizeof(T1_KEYS) / sizeof(T1_KEYS[0]) == T1_NKEYS, "T1_NKEYS komt niet overeen met T1_KEYS");
// Documentgroottes volgen automatisch de slot-grootte van het platform (ESP32: ~0,8 KB + ~0,7 KB, op de stack)
#define T1_FILTER_CAP (2 * JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(1) + 3 * JSON_OBJECT_SIZE(T1_NKEYS))
#define T1_DOC_CAP    (2 * JSON_OBJECT_SIZE(1) + JSON_OBJECT_SIZE(T1_NKEYS) + 448)   // + gekopieerde strings

const char* const T1_CMD_PATH[]  = { "", "charge_start", "charge_stop", "wake_up", "set_charging_amps" };
const char* const T1_CMD_LABEL[] = { "", "start", "stop", "wake-up", "ampere" };

struct Tesla1 {
  bool     ok         = false;   // laatste poll geslaagd
  bool     have       = false;   // ooit geldige data ontvangen
  int16_t  soc = -1, usable = -1, limit = -1;
  int16_t  a_act = -1, a_req = -1, a_set = -1;
  int16_t  volt = -1, phases = 0, mins = -1, pk = -1;
  int32_t  pw         = 0;       // berekend vermogen W (A x V x fasen)
  float    energy     = 0;       // kWh toegevoegd
  float    range_km   = -1;
  int8_t   port       = -1;      // -1 onbekend / 0 dicht / 1 open
  uint32_t last_ok_ms = 0;
  char     state[16]  = "";
  char     msg[40]    = "nog geen data";   // status laatste poll
  char     cmsg[40]   = "";                // resultaat laatste commando
};
Tesla1 t1;

char              t1_host[40]  = "";
char              t1_vin[20]   = "";
uint8_t           t1_maxa      = DEF_TESLA_MAXA;
volatile uint8_t  t1_cmd       = 0;    // 0 geen, 1 start, 2 stop, 3 wake, 4 ampere
volatile uint8_t  t1_cmd_amps  = 0;
volatile uint32_t t1_last_view = 0;    // laatste keer dat /tesla_json werd opgevraagd
uint32_t          t1_next      = 0;    // volgende poll (millis)

bool t1Enabled() { return !ap_mode && t1_host[0] && t1_vin[0]; }
bool t1Viewed(uint32_t now) { return t1_last_view && (uint32_t)(now - t1_last_view) < 120000UL; }

// Kopieert enkel veilige tekens (host/VIN/status komen ongeëscaped in JSON terecht)
void t1CopyClean(char* dst, size_t cap, const char* src, bool upper) {
  size_t n = 0;
  for (; src && *src && n + 1 < cap; src++) {
    char ch = *src;
    if (isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_')
      dst[n++] = upper ? (char)toupper((unsigned char)ch) : ch;
  }
  dst[n] = 0;
}

int t1Hex(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// Leest één regel (zonder CR/LF). Geeft lengte (0 = lege regel) of -1 bij timeout/einde.
int t1ReadLine(WiFiClient& c, char* buf, int cap, uint32_t dl) {
  int n = 0;
  while ((int32_t)(dl - millis()) > 0) {
    if (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') { buf[n] = 0; return n; }
      if (ch != '\r' && n < cap - 1) buf[n++] = ch;
    } else if (!c.connected()) {
      break;
    } else {
      delay(2);
    }
  }
  buf[n] = 0;
  return -1;
}

// Body-stream voor ArduinoJson: decodeert 'Transfer-Encoding: chunked' onderweg
// (geen buffer van de volledige respons nodig).
class T1Body : public Stream {
  WiFiClient& c_;
  bool        chunked_;
  uint32_t    dl_;
  int32_t     left_;
  bool        eof_;
  int         pk_;      // -2 = niets in peek-buffer
  int rawRead() {
    while ((int32_t)(dl_ - millis()) > 0) {
      if (c_.available()) return c_.read();
      if (!c_.connected()) return -1;
      delay(1);
    }
    return -1;
  }
  int nextByte() {
    if (eof_) return -1;
    if (!chunked_) {
      int b = rawRead();
      if (b < 0) eof_ = true;
      return b;
    }
    if (left_ <= 0) {
      int32_t v = -1; bool ext = false;
      for (;;) {
        int b = rawRead();
        if (b < 0) { eof_ = true; return -1; }
        if (b == '\n') { if (v >= 0) break; ext = false; continue; }
        if (ext || b == '\r') continue;
        if (b == ';') { ext = true; continue; }
        int h = t1Hex(b);
        if (h >= 0) v = (v < 0 ? 0 : v) * 16 + h;
      }
      if (v == 0) { eof_ = true; return -1; }
      left_ = v;
    }
    int b = rawRead();
    if (b < 0) { eof_ = true; return -1; }
    left_--;
    return b;
  }
public:
  T1Body(WiFiClient& c, bool chunked, uint32_t dl)
    : c_(c), chunked_(chunked), dl_(dl), left_(0), eof_(false), pk_(-2) { setTimeout(20); }
  int available() override { return (pk_ != -2 || !eof_) ? 1 : 0; }
  int read() override {
    if (pk_ != -2) { int b = pk_; pk_ = -2; return b; }
    return nextByte();
  }
  int peek() override {
    if (pk_ == -2) pk_ = nextByte();
    return pk_;
  }
  size_t write(uint8_t) override { return 0; }
};

// Stuurt het verzoek en leest statusregel + headers.
// Return: HTTP-code, 0 = geen verbinding, -1 = ongeldig/geen antwoord binnen deadline.
// De client blijft open voor het lezen van de body.
int t1Open(WiFiClient& c, const char* method, const char* path, const char* body,
           uint32_t to_ms, bool& chunked, uint32_t& dl) {
  chunked = false;
  dl = millis() + to_ms;
  if (!c.connect(t1_host, 80, 1500)) return 0;

  char hdr[72] = "";
  if (body) snprintf(hdr, sizeof(hdr), "Content-Type: application/json\r\nContent-Length: %d\r\n", (int)strlen(body));
  char req[320];
  int rl = snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n%s\r\n%s",
                    method, path, t1_host, hdr, body ? body : "");
  if (rl <= 0 || rl >= (int)sizeof(req)) return -1;
  c.write((const uint8_t*)req, (size_t)rl);   // één segment

  char line[80];
  int n = t1ReadLine(c, line, sizeof(line), dl);
  if (n < 12 || strncmp(line, "HTTP/", 5) != 0) return -1;
  int code = atoi(line + 9);
  for (;;) {
    n = t1ReadLine(c, line, sizeof(line), dl);
    if (n < 0) return -1;
    if (n == 0) break;
    for (char* q = line; *q; ++q) *q = (char)tolower((unsigned char)*q);
    if (strncmp(line, "transfer-encoding:", 18) == 0 && strstr(line, "chunked")) chunked = true;
  }
  return code;
}

// Waarden uit charge_state naar de vaste struct
void t1Extract(JsonObject cs) {
  t1.soc    = cs["battery_level"] | -1;
  t1.usable = cs["usable_battery_level"] | -1;
  t1.limit  = cs["charge_limit_soc"] | -1;
  t1CopyClean(t1.state, sizeof(t1.state), cs["charging_state"] | "?", false);
  int aset  = cs["charge_amps"] | -1;
  t1.a_set  = aset;
  t1.a_req  = cs["charge_current_request"] | -1;
  t1.a_act  = cs["charger_actual_current"] | aset;
  t1.volt   = cs["charger_voltage"] | -1;
  t1.phases = cs["charger_phases"] | 0;
  t1.pk     = cs["charger_power"] | -1;          // kW zoals de auto het meldt (geheel getal)
  t1.energy = cs["charge_energy_added"] | 0.0f;
  t1.mins   = cs["minutes_to_full_charge"] | -1;
  float mi  = cs["battery_range"] | (cs["ideal_battery_range"] | -1.0f);
  t1.range_km = (mi >= 0) ? mi * 1.60934f : -1.0f;
  JsonVariant pv = cs["charge_port_door_open"];
  t1.port   = pv.is<bool>() ? (pv.as<bool>() ? 1 : 0) : -1;
  // Berekend vermogen — fasen-interpretatie (>=2 => 3-fase) nog te verifiëren tegen echte meting
  int ph = (t1.phases >= 2) ? 3 : 1;
  bool laadt = (strcmp(t1.state, "Charging") == 0);
  t1.pw = (laadt && t1.a_act > 0 && t1.volt > 0) ? (int32_t)t1.a_act * t1.volt * ph : 0;
}

void t1Poll() {
  char path[96];
  snprintf(path, sizeof(path), "/api/1/vehicles/%s/vehicle_data", t1_vin);
  WiFiClient c;
  bool chunked; uint32_t dl;
  int code = t1Open(c, "GET", path, nullptr, TESLA_GET_TIMEOUT, chunked, dl);
  bool okp = false;
  if (code == 200) {
    T1Body body(c, chunked, dl);
    StaticJsonDocument<T1_FILTER_CAP> filter;
    for (int i = 0; i < T1_NKEYS; i++) {       // geneste variant van de proxy-respons opvangen
      filter["response"]["response"]["charge_state"][T1_KEYS[i]] = true;
      filter["response"]["charge_state"][T1_KEYS[i]] = true;
      filter["charge_state"][T1_KEYS[i]] = true;
    }
    StaticJsonDocument<T1_DOC_CAP> doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
      snprintf(t1.msg, sizeof(t1.msg), "JSON: %s", err.c_str());
    } else {
      JsonObject cs = doc["response"]["response"]["charge_state"].as<JsonObject>();
      if (cs.isNull()) cs = doc["response"]["charge_state"].as<JsonObject>();
      if (cs.isNull()) cs = doc["charge_state"].as<JsonObject>();
      if (cs.isNull()) {
        strlcpy(t1.msg, "geen laaddata (auto slaapt / BLE?)", sizeof(t1.msg));
      } else {
        t1Extract(cs);
        okp = true;
      }
    }
  } else if (code == 0) {
    strlcpy(t1.msg, "proxy onbereikbaar", sizeof(t1.msg));
  } else if (code < 0) {
    strlcpy(t1.msg, "geen geldig antwoord (timeout)", sizeof(t1.msg));
  } else {
    snprintf(t1.msg, sizeof(t1.msg), "proxy HTTP %d", code);
  }
  c.stop();

  t1.ok = okp;
  uint32_t now = millis();
  if (okp) {
    t1.have = true; t1.last_ok_ms = now;
    strlcpy(t1.msg, "OK", sizeof(t1.msg));
    Serial.printf("[T1] SoC:%d%% %s %dA %ldW\n", t1.soc, t1.state, t1.a_act, (long)t1.pw);
  } else {
    Serial.printf("[T1] %s\n", t1.msg);
  }
  t1_next = now + (okp ? (t1Viewed(now) ? TESLA_POLL_VIEW_MS : TESLA_POLL_IDLE_MS)
                       : TESLA_POLL_FAIL_MS);
}

void t1RunCmd() {
  uint8_t id  = t1_cmd;
  int     amp = t1_cmd_amps;
  if (id < 1 || id > 4) { t1_cmd = 0; return; }
  char path[100], body[32] = "", lbl[20];
  snprintf(path, sizeof(path), "/api/1/vehicles/%s/command/%s", t1_vin, T1_CMD_PATH[id]);
  if (id == 4) {
    snprintf(body, sizeof(body), "{\"charging_amps\":%d}", amp);
    snprintf(lbl, sizeof(lbl), "ampere %dA", amp);
  } else {
    strlcpy(lbl, T1_CMD_LABEL[id], sizeof(lbl));
  }
  WiFiClient c;
  bool chunked; uint32_t dl;
  int code = t1Open(c, "POST", path, body, TESLA_CMD_TIMEOUT, chunked, dl);
  c.stop();
  if (code == 200)     snprintf(t1.cmsg, sizeof(t1.cmsg), "%s: OK", lbl);
  else if (code == 0)  snprintf(t1.cmsg, sizeof(t1.cmsg), "%s: proxy onbereikbaar", lbl);
  else if (code < 0)   snprintf(t1.cmsg, sizeof(t1.cmsg), "%s: geen antwoord", lbl);
  else                 snprintf(t1.cmsg, sizeof(t1.cmsg), "%s: HTTP %d", lbl, code);
  Serial.printf("[T1] cmd %s\n", t1.cmsg);
  t1_cmd  = 0;
  t1_next = millis() + 2500UL;     // snel verversen om het effect te tonen
}

// ── Tesla1 endpoints ─────────────────────────────────────────
void serveTeslaJson(AsyncWebServerRequest *req) {
  uint32_t now = millis();
  if (!t1Viewed(now)) t1_next = now;          // pagina net geopend → direct verversen
  t1_last_view = now ? now : 1;
  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"en\":%d,\"ok\":%d,\"age\":%ld,\"soc\":%d,\"us\":%d,\"lim\":%d,\"st\":\"%s\","
    "\"a\":%d,\"ar\":%d,\"as\":%d,\"v\":%d,\"ph\":%d,\"pw\":%ld,\"pk\":%d,"
    "\"e\":%.2f,\"m\":%d,\"r\":%.0f,\"p\":%d,\"maxa\":%d,\"busy\":%d,"
    "\"msg\":\"%s\",\"cm\":\"%s\"}",
    t1Enabled() ? 1 : 0, t1.ok ? 1 : 0,
    t1.have ? (long)((now - t1.last_ok_ms) / 1000UL) : -1L,
    t1.soc, t1.usable, t1.limit, t1.state,
    t1.a_act, t1.a_req, t1.a_set, t1.volt, t1.phases, (long)t1.pw, t1.pk,
    t1.energy, t1.mins, t1.range_km, t1.port, t1_maxa, t1_cmd ? 1 : 0,
    t1.msg, t1.cmsg);
  req->send(200, "application/json", buf);
}

void serveTeslaCfg(AsyncWebServerRequest *req) {
  char buf[120];
  snprintf(buf, sizeof(buf), "{\"host\":\"%s\",\"vin\":\"%s\",\"maxa\":%d}", t1_host, t1_vin, t1_maxa);
  req->send(200, "application/json", buf);
}

void handleTeslaSave(AsyncWebServerRequest *req) {
  auto ph = req->getParam("host");
  auto pv = req->getParam("vin");
  auto pm = req->getParam("maxa");
  if (ph) { t1CopyClean(t1_host, sizeof(t1_host), ph->value().c_str(), false); prefs.putString(NVS_T_HOST, t1_host); }
  if (pv) { t1CopyClean(t1_vin,  sizeof(t1_vin),  pv->value().c_str(), true);  prefs.putString(NVS_T_VIN,  t1_vin);  }
  if (pm) {
    int m = (int)pm->value().toInt();
    t1_maxa = (uint8_t)constrain(m, TESLA_AMPS_MIN, TESLA_AMPS_CAP);
    prefs.putUChar(NVS_T_MAXA, t1_maxa);
  }
  t1.ok = false; t1.have = false;
  strlcpy(t1.msg, "instellingen gewijzigd", sizeof(t1.msg));
  t1_next = millis();
  req->send(200, "text/plain", "OK");
}

void handleTeslaCmd(AsyncWebServerRequest *req) {
  if (!t1Enabled()) { req->send(200, "text/plain", "Tesla niet ingesteld"); return; }
  auto pc = req->getParam("c");
  if (!pc) { req->send(400, "text/plain", "geen commando"); return; }
  if (t1_cmd) { req->send(200, "text/plain", "vorig commando nog bezig"); return; }
  const String& c = pc->value();
  uint8_t id = 0;
  if      (c == "start") id = 1;
  else if (c == "stop")  id = 2;
  else if (c == "wake")  id = 3;
  else if (c == "amps")  id = 4;
  if (!id) { req->send(400, "text/plain", "onbekend commando"); return; }
  if (id == 4) {
    auto pa = req->getParam("a");
    if (!pa) { req->send(400, "text/plain", "geen ampere"); return; }
    int a = (int)pa->value().toInt();
    t1_cmd_amps = (uint8_t)constrain(a, TESLA_AMPS_MIN, (int)t1_maxa);
  }
  t1_cmd = id;
  req->send(200, "text/plain", "gepland");
}

const char T1_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Tesla1</title><style>
body{font-family:Arial,sans-serif;margin:0;background:#f4f4f4;}
.hdr{background:#ffcc00;padding:10px 15px;font-weight:bold;font-size:17px;display:flex;justify-content:space-between;align-items:center;}
.nav{display:flex;flex-wrap:wrap;gap:5px;padding:7px 12px;background:#fff;border-bottom:2px solid #ddd;}
.nav a{background:#369;color:#fff;padding:5px 11px;border-radius:4px;text-decoration:none;font-size:13px;}
.nav a:hover{background:#036;}.nav a.act{background:#c00;}
.banner{padding:9px 14px;margin:8px 14px;border-radius:6px;font-weight:bold;font-size:13px;text-align:center;background:#888;color:#fff;}
.red{background:#c00;}.ok{background:#2a8a3e;}
table{margin:8px 14px;border-collapse:collapse;width:calc(100% - 28px);max-width:500px;}
td{padding:6px 8px;border-bottom:1px solid #ddd;font-size:14px;}
td:first-child{font-weight:bold;color:#369;width:44%;}
.box{margin:8px 14px;max-width:500px;}
.btn{background:#369;color:#fff;padding:9px 16px;border:none;border-radius:5px;font-size:14px;cursor:pointer;margin:4px 6px 4px 0;}
.btn:hover{background:#036;}.g{background:#2a8a3e;}.r{background:#c00;}
input[type=text],input[type=number]{width:100%;padding:6px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;font-size:14px;}
input[type=range]{width:100%;}
small{color:#666;}
</style></head><body>
<div class='hdr'><span>🚗 Tesla1</span><span id='ts' style='font-size:12px;font-weight:normal'></span></div>
<div class='nav'><a href='/'>Status</a><a href='/json'>JSON</a><a href='/tesla' class='act'>Tesla</a><a href='/update'>OTA</a><a href='/settings'>Settings</a></div>
<div id='ban' class='banner'>Laden…</div>
<table>
<tr><td>🔋 Batterij</td><td id='soc'>—</td></tr>
<tr><td>Laadstatus</td><td id='st'>—</td></tr>
<tr><td>Charge limit</td><td id='lim'>—</td></tr>
<tr><td>Huidige A</td><td id='a'>—</td></tr>
<tr><td>Ingesteld / gevraagd A</td><td id='ar'>—</td></tr>
<tr><td>Vermogen</td><td id='pw'>—</td></tr>
<tr><td>Spanning / fasen</td><td id='vph'>—</td></tr>
<tr><td>Energie toegevoegd</td><td id='e'>—</td></tr>
<tr><td>Tijd tot vol</td><td id='m'>—</td></tr>
<tr><td>Range</td><td id='r'>—</td></tr>
<tr><td>Laadpoort</td><td id='p'>—</td></tr>
<tr><td>Data-leeftijd</td><td id='age'>—</td></tr>
</table>
<div class='box'>
<button class='btn g' onclick="cmd('start','Laden starten?')">▶ Start</button>
<button class='btn r' onclick="cmd('stop','Laden stoppen?')">■ Stop</button>
<button class='btn' onclick="cmd('wake','')">☀ Wake-up</button><br><br>
<b>Laadstroom: <span id='av'>16</span> A</b>
<input type='range' id='sl' min='5' max='16' value='16' oninput="$('av').textContent=this.value">
<button class='btn' onclick="cmd('amps&a='+$('sl').value,'')">Zet ampère</button>
<div id='cmsg' style='font-size:12px;color:#666;margin-top:6px'></div>
</div>
<div class='box'><hr>
<b>Instellingen</b> <small>(worden direct bewaard, geen reboot)</small>
<table style='margin:6px 0;width:100%'>
<tr><td>Proxy host / IP</td><td><input type='text' id='cfh' maxlength='39' placeholder='192.168.0.xx (leeg = Tesla uit)'></td></tr>
<tr><td>VIN</td><td><input type='text' id='cfv' maxlength='17'></td></tr>
<tr><td>Max ampère</td><td><input type='number' id='cfa' min='5' max='32'></td></tr>
</table>
<button class='btn' onclick='save()'>Opslaan</button>
</div>
<script>
var $=function(i){return document.getElementById(i)},first=1,
NL={Charging:'Laden',Stopped:'Gestopt',Complete:'Klaar',Disconnected:'Niet verbonden',NoPower:'Geen stroom',Starting:'Start…'};
function n(v,d,u){return(v==null||v<0)?'—':v.toFixed(d)+u}
function upd(){fetch('/tesla_json').then(function(r){return r.json()}).then(function(d){
 var b=$('ban');
 if(!d.en){b.className='banner red';b.textContent='Tesla niet ingesteld — vul host en VIN in';}
 else if(!d.ok){b.className='banner red';b.textContent='⚠️ '+d.msg;}
 else{b.className='banner ok';b.textContent='✅ Proxy OK';}
 $('soc').textContent=n(d.soc,0,' %')+(d.us>=0?' (bruikbaar '+d.us+' %)':'');
 $('st').textContent=d.st?(NL[d.st]||d.st):'—';
 $('lim').textContent=n(d.lim,0,' %');
 $('a').textContent=n(d.a,0,' A');
 $('ar').textContent=n(d.as,0,' A')+' / '+n(d.ar,0,' A');
 $('pw').textContent=(d.pw>0?(d.pw/1000).toFixed(1):'0')+' kW'+(d.pk>=0?' (auto meldt: '+d.pk+' kW)':'');
 $('vph').textContent=n(d.v,0,' V')+' / '+(d.ph>0?d.ph:'—');
 $('e').textContent=n(d.e,2,' kWh');
 $('m').textContent=d.m>=0?Math.floor(d.m/60)+' u '+(d.m%60)+' min':'—';
 $('r').textContent=n(d.r,0,' km');
 $('p').textContent=d.p==1?'open':d.p==0?'dicht':'—';
 $('age').textContent=d.age>=0?d.age+' s':'—';
 $('cmsg').textContent=d.busy?'⏳ commando wordt uitgevoerd…':(d.cm?'Laatste commando: '+d.cm:'');
 $('sl').max=d.maxa;
 if(first&&d.as>=5&&d.as<=d.maxa){$('sl').value=d.as;$('av').textContent=d.as;first=0;}
 $('ts').textContent=new Date().toLocaleTimeString('nl-BE');
}).catch(function(){var b=$('ban');b.className='banner red';b.textContent='ESP niet bereikbaar';});}
function cmd(c,q){
 if(q&&!confirm(q))return;
 fetch('/tesla_cmd?c='+c,{method:'POST'}).then(function(r){return r.text()}).then(function(t){
  $('cmsg').textContent=t;setTimeout(upd,1500);});
}
function save(){
 fetch('/tesla_save?host='+encodeURIComponent($('cfh').value.trim())+'&vin='+encodeURIComponent($('cfv').value.trim())+'&maxa='+encodeURIComponent($('cfa').value))
 .then(function(){location.reload();});
}
fetch('/tesla_cfg').then(function(r){return r.json()}).then(function(c){
 $('cfh').value=c.host;$('cfv').value=c.vin;$('cfa').value=c.maxa;$('sl').max=c.maxa;});
upd();setInterval(upd,5000);
</script></body></html>)rawliteral";

// ── NVS OPSLAAN ─────────────────────────────────────────────
void saveEnergy() {
  prefs.putFloat(NVS_SOL,  wh_sol);
  prefs.putFloat(NVS_SCHF, wh_schf);
  prefs.putFloat(NVS_SCHR, wh_schr);
  prefs.putFloat(NVS_PIEK, piek_w);
  Serial.printf("[NVS] Sol:%.0f SchF:%.0f SchR:%.0f Piek:%.0fW\n",
    wh_sol, wh_schf, wh_schr, piek_w);
}

// ── MIDNIGHT RESET ───────────────────────────────────────────
// v1.28: elke 5s-tick aangeroepen; dag (YYYYMMDD) staat in NVS zodat ook een reboot
// over middernacht heen correct resetten. Blokkeert niet als NTP nog niet sync is.
void checkMidnight() {
  time_t t = time(nullptr);
  if (t < 1700000000L) return;                       // NTP nog niet gesynchroniseerd
  struct tm ti; localtime_r(&t, &ti);
  int32_t today = (ti.tm_year + 1900) * 10000 + (ti.tm_mon + 1) * 100 + ti.tm_mday;
  if (last_day < 0) {                                // eerste start met v1.28: dag onthouden, niets wissen
    last_day = today; prefs.putInt(NVS_DAY, last_day); return;
  }
  if (today == last_day) return;
  bool nieuwe_maand = (today / 100 != last_day / 100);
  last_day = today;
  wh_sol = wh_schf = wh_schr = 0;
  wh_won_imp = wh_won_exp = 0;
  if (nieuwe_maand) piek_w = 0;
  prefs.putInt(NVS_DAY, last_day);
  saveEnergy();
  Serial.printf("[RESET] Dag %d/%d%s\n", ti.tm_mday, ti.tm_mon + 1, nieuwe_maand ? " (nieuwe maand: piek reset)" : "");
}

// ── SERIAL COMMANDO'S ────────────────────────────────────────
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n'); cmd.trim();

  if (cmd.equalsIgnoreCase("reset_nvs")) {
    prefs.clear(); delay(200); ESP.restart();
  } else if (cmd.equalsIgnoreCase("status")) {
    Serial.printf("Sol:%.0fW SchF:%.0fW SchR:%.0fW Netto:%.0fW WON:%.0fW EPEX:%.1fct\n",
      w_sol, w_schf, w_schr, w_netto, w_won, epex_nu);
    Serial.printf("Dag: Sol:%.0f SchF:%.0f SchR:%.0f WON-imp:%.0f WON-exp:%.0f Piek:%.0fW\n",
      wh_sol, wh_schf, wh_schr, wh_won_imp, wh_won_exp, piek_w);
    Serial.printf("SIM_S0: %s  SIM_P1: %s  P1_IP: %s\n",
      SIM_S0 ? "AAN" : "UIT", SIM_P1 ? "AAN" : "UIT",
      strlen(p1_ip) > 0 ? p1_ip : "(niet ingesteld)");
    Serial.printf("Tesla1: %s host=%s ok=%d SoC=%d%% %s %dA msg=%s\n",
      t1Enabled() ? "AAN" : "UIT", t1_host, t1.ok ? 1 : 0, t1.soc, t1.state, t1.a_act, t1.msg);

  // S0 simulatie — BEWUST handmatig omschakelen
  } else if (cmd.equalsIgnoreCase("sim s0 on")) {
    SIM_S0 = true;  prefs.putBool(NVS_SIM_S0, true);
    Serial.println(F("[SIM_S0] AAN — S0 pulsen worden gesimuleerd"));
  } else if (cmd.equalsIgnoreCase("sim s0 off")) {
    SIM_S0 = false; prefs.putBool(NVS_SIM_S0, false);
    Serial.println(F("[SIM_S0] UIT — live S0 ISR actief ⚠️  Controleer bekabeling!"));

  // P1 simulatie — BEWUST handmatig omschakelen
  } else if (cmd.equalsIgnoreCase("sim p1 on")) {
    SIM_P1 = true;  prefs.putBool(NVS_SIM_P1, true);
    Serial.println(F("[SIM_P1] AAN — P1 dongle wordt gesimuleerd"));
  } else if (cmd.equalsIgnoreCase("sim p1 off")) {
    SIM_P1 = false; prefs.putBool(NVS_SIM_P1, false);
    Serial.printf("[SIM_P1] UIT — live HomeWizard P1 actief (IP: %s)\n",
      strlen(p1_ip) > 0 ? p1_ip : "⚠️  NIET INGESTELD!");

  } else if (cmd.equalsIgnoreCase("help")) {
    Serial.println(F("Commando's: status | reset_nvs | "
      "sim s0 on/off | sim p1 on/off"));
  }
}

// ── /json ENDPOINT ──────────────────────────────────────────
// Conform §6.4 Master Overnamedocument + v1.26 uitbreidingen:
//   sim_s0: 1 = S0 gesimuleerd, 0 = live hardware
//   sim_p1: 1 = P1 gesimuleerd, 0 = live HomeWizard dongle
//   b:      WON momentaan vermogen W (+ = afname)
//   i:      WON dag afname Wh
//   vw:     WON dag injectie Wh (nieuw v1.26)
void serveJson(AsyncWebServerRequest *req) {
  char buf[560];
  snprintf(buf, sizeof(buf),
    "{"
    "\"a\":%d,"     // Solar W
    "\"b\":%d,"     // WON W (P1)
    "\"c\":%d,"     // SCH afname W
    "\"d\":%d,"     // SCH injectie W
    "\"e\":%d,"     // Netto W SCH (+ = injectie)
    "\"h\":%d,"     // Solar dag Wh
    "\"i\":%d,"     // WON dag afname Wh
    "\"j\":%d,"     // SCH afname dag Wh
    "\"k\":%d,"     // SCH injectie dag Wh
    "\"vw\":%d,"    // WON dag injectie Wh (nieuw v1.26)
    "\"n\":%d,"     // EPEX nu ct/kWh × 100
    "\"n2\":%d,"    // EPEX +1u ct/kWh × 100
    "\"pt\":%d,"    // Piek maand W
    "\"ac\":%d,"    // RSSI dBm
    "\"ae\":%d,"    // Heap largest block bytes
    "\"sim_s0\":%d,"// 1 = S0 gesimuleerd
    "\"sim_p1\":%d,"// 1 = P1 gesimuleerd
    "\"ver\":\"%s\""
    "}",
    (int)w_sol,
    (int)w_won,
    (int)w_schf, (int)w_schr, (int)w_netto,
    (int)wh_sol,
    (int)wh_won_imp,
    (int)wh_schf, (int)wh_schr,
    (int)wh_won_exp,
    (int)(epex_nu  * 100), (int)(epex_p1h * 100),
    (int)piek_w,
    WiFi.RSSI(),
    (int)ESP.getMaxAllocHeap(),
    SIM_S0 ? 1 : 0,
    SIM_P1 ? 1 : 0,
    FW_VERSION
  );
  req->send(200, "application/json", buf);
}

// ── STATUS PAGINA ────────────────────────────────────────────
void serveStatus(AsyncWebServerRequest *req) {
  AsyncResponseStream *p = req->beginResponseStream("text/html;charset=utf-8");
  p->print(F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>S-ENERGY</title><style>"
    "body{font-family:Arial,sans-serif;margin:0;background:#f4f4f4;}"
    ".hdr{background:#ffcc00;padding:10px 15px;font-weight:bold;font-size:17px;"
    "display:flex;justify-content:space-between;align-items:center;}"
    ".nav{display:flex;flex-wrap:wrap;gap:5px;padding:7px 12px;background:#fff;"
    "border-bottom:2px solid #ddd;}"
    ".nav a{background:#369;color:#fff;padding:5px 11px;border-radius:4px;"
    "text-decoration:none;font-size:13px;}"
    ".nav a:hover{background:#036;}.nav a.act{background:#c00;}"
    ".banner{padding:9px 14px;margin:8px 14px;border-radius:6px;"
    "font-weight:bold;font-size:13px;text-align:center;}"
    ".banner-red{background:#c00;color:#fff;}"
    ".banner-ok{background:#2a8a3e;color:#fff;}"
    "table{margin:8px 14px;border-collapse:collapse;width:calc(100% - 28px);max-width:500px;}"
    "td{padding:6px 8px;border-bottom:1px solid #ddd;font-size:14px;}"
    "td:first-child{font-weight:bold;color:#369;width:44%;}"
    ".lbl-sim{background:#c00;color:#fff;font-size:10px;padding:1px 5px;"
    "border-radius:3px;margin-left:5px;}"
    ".lbl-live{background:#2a8a3e;color:#fff;font-size:10px;padding:1px 5px;"
    "border-radius:3px;margin-left:5px;}"
    "</style></head><body>"
    "<div class='hdr'><span>" CTRL_ID " v" FW_VERSION "</span>"
    "<span id='ts' style='font-size:12px;font-weight:normal'></span></div>"
    "<div class='nav'>"
    "<a href='/' class='act'>Status</a><a href='/json'>JSON</a><a href='/tesla'>Tesla</a>"
    "<a href='/update'>OTA</a><a href='/settings'>Settings</a>"
    "</div>"));

  // Statusbanners per kanaal
  if (SIM_S0) p->print(F("<div class='banner banner-red'>"
    "⚠️ S0 SIMULATIE ACTIEF — S0 kanalen genereren neppe data!</div>"));
  else        p->print(F("<div class='banner banner-ok'>"
    "✅ S0 LIVE — hardware S0-pulsen (Inepro PRO380-S)</div>"));
  if (SIM_P1) p->print(F("<div class='banner banner-red'>"
    "⚠️ P1 SIMULATIE ACTIEF — WON data is nep!</div>"));
  else        p->print(F("<div class='banner banner-ok'>"
    "✅ P1 LIVE — HomeWizard P1 Meter (HWE-P1-RJ12)</div>"));

  p->print(F("<table>"
    "<tr><td>☀️ Solar</td><td id='sol'>—</td></tr>"
    "<tr><td>⚡ SCH afname</td><td id='schf'>—</td></tr>"
    "<tr><td>🔄 SCH injectie</td><td id='schr'>—</td></tr>"
    "<tr><td>⚖️ Netto SCH</td><td id='net'>—</td></tr>"
    "<tr><td>🏠 WON vermogen</td><td id='won'>—</td></tr>"
    "<tr><td>☀️ Solar dag</td><td id='sold'>—</td></tr>"
    "<tr><td>⚡ SCH afname dag</td><td id='schfd'>—</td></tr>"
    "<tr><td>🔄 SCH injectie dag</td><td id='schrd'>—</td></tr>"
    "<tr><td>🏠 WON afname dag</td><td id='wond'>—</td></tr>"
    "<tr><td>🔄 WON injectie dag</td><td id='wonx'>—</td></tr>"
    "<tr><td>📊 Maandpiek</td><td id='piek'>—</td></tr>"
    "<tr><td>💰 EPEX nu</td><td id='epn'>—</td></tr>"
    "<tr><td>💰 EPEX +1u</td><td id='ep1'>—</td></tr>"
    "<tr><td>📶 WiFi RSSI</td><td id='rssi'>—</td></tr>"
    "<tr><td>💾 Heap</td><td id='heap'>—</td></tr>"
    "</table>"
    "<script>function upd(){fetch('/json').then(r=>r.json()).then(d=>{"
    "var f=v=>typeof v==='number'?v.toFixed(2):v;"
    "document.getElementById('sol').textContent=d.a+' W';"
    "document.getElementById('schf').textContent=d.c+' W';"
    "document.getElementById('schr').textContent=d.d+' W';"
    "document.getElementById('net').textContent=(d.e>=0?'+':'')+d.e+' W';"
    "document.getElementById('won').textContent=(d.b>=0?'+':'')+d.b+' W';"
    "document.getElementById('sold').textContent=f(d.h/1000)+' kWh';"
    "document.getElementById('schfd').textContent=f(d.j/1000)+' kWh';"
    "document.getElementById('schrd').textContent=f(d.k/1000)+' kWh';"
    "document.getElementById('wond').textContent=f(d.i/1000)+' kWh';"
    "document.getElementById('wonx').textContent=f((d.vw||0)/1000)+' kWh';"
    "document.getElementById('piek').textContent=d.pt+' W';"
    "document.getElementById('epn').textContent=f(d.n/100)+' ct/kWh';"
    "document.getElementById('ep1').textContent=f(d.n2/100)+' ct/kWh';"
    "document.getElementById('rssi').textContent=d.ac+' dBm';"
    "document.getElementById('heap').textContent=Math.round(d.ae/1024)+' KB';"
    "var n=new Date();"
    "document.getElementById('ts').textContent="
    "n.toLocaleDateString('nl-BE')+' '+n.toLocaleTimeString('nl-BE');"
    "}).catch(()=>{});}"
    "upd();setInterval(upd,5000);</script></body></html>"));
  req->send(p);
}

// ── SETTINGS PAGINA ──────────────────────────────────────────
void serveSettings(AsyncWebServerRequest *req) {
  AsyncResponseStream *p = req->beginResponseStream("text/html;charset=utf-8");
  p->print(F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>S-ENERGY Settings</title><style>"
    "body{font-family:Arial,sans-serif;margin:0;background:#f4f4f4;}"
    ".hdr{background:#ffcc00;padding:10px 15px;font-weight:bold;font-size:17px;}"
    ".nav{display:flex;flex-wrap:wrap;gap:5px;padding:7px 12px;background:#fff;"
    "border-bottom:2px solid #ddd;}"
    ".nav a{background:#369;color:#fff;padding:5px 11px;border-radius:4px;"
    "text-decoration:none;font-size:13px;}"
    ".nav a:hover{background:#036;}.nav a.act{background:#c00;}"
    ".form{margin:14px;max-width:520px;}"
    "table{width:100%;border-collapse:collapse;}"
    "td{padding:7px 6px;border-bottom:1px solid #ddd;font-size:14px;}"
    "td:first-child{font-weight:bold;color:#369;width:42%;}"
    "input[type=text],input[type=number],input[type=password]"
    "{width:100%;padding:6px;border:1px solid #ccc;border-radius:4px;"
    "box-sizing:border-box;font-size:14px;}"
    ".btn{background:#369;color:#fff;padding:9px 20px;border:none;border-radius:5px;"
    "font-size:14px;cursor:pointer;margin:8px 6px 0 0;}"
    ".btn:hover{background:#036;}.btn-red{background:#c00;}.btn-red:hover{background:#900;}"
    ".sim-block{border:2px solid #c00;border-radius:8px;padding:12px 14px;margin:14px 0;"
    "background:#fff8f8;}"
    ".sim-ok{border-color:#2a8a3e;background:#f0fff4;}"
    ".sim-title{font-weight:bold;font-size:14px;margin-bottom:8px;}"
    ".sim-warn{color:#c00;font-size:12px;margin-top:6px;}"
    ".sim-ok-txt{color:#2a8a3e;font-size:12px;margin-top:4px;}"
    "label{cursor:pointer;font-size:14px;}"
    "</style></head><body>"
    "<div class='hdr'>S-ENERGY v" FW_VERSION " Settings</div>"
    "<div class='nav'>"
    "<a href='/'>Status</a><a href='/json'>JSON</a><a href='/tesla'>Tesla</a>"
    "<a href='/update'>OTA</a><a href='/settings' class='act'>Settings</a>"
    "</div><div class='form'>"
    "<form action='/save_settings' method='get' id='sf'><table>"));

  p->printf("<tr><td>MAC adres</td><td><code>%s</code></td></tr>",
    WiFi.macAddress().c_str());
  p->printf("<tr><td>WiFi SSID</td><td>"
    "<input name='ssid' value='%s' required></td></tr>", wifi_ssid);
  p->print(F("<tr><td>WiFi wachtwoord</td><td>"
    "<input name='pass' type='password' placeholder='leeg = ongewijzigd'></td></tr>"));
  p->printf("<tr><td>Statisch IP</td><td>"
    "<input name='ip' value='%s' placeholder='leeg = DHCP'></td></tr>", static_ip);
  p->printf("<tr><td>LED helderheid (0–255)</td><td>"
    "<input name='bri' type='number' min='5' max='255' value='%d'></td></tr>",
    (int)prefs.getUChar(NVS_BRI, DEF_BRIGHT));
  p->printf("<tr><td>Max piek (W)</td><td>"
    "<input name='mpiek' type='number' min='1000' max='20000' value='%d'></td></tr>",
    (int)max_piek_w);
  p->print(F("</table>"));

  // ── S0 Simulatie sectie ──────────────────────────────────
  p->printf("<div class='sim-block%s'>"
    "<div class='sim-title'>📡 S0 kanalen — Inepro PRO380-S</div>"
    "<label><input type='checkbox' name='sim_s0' value='1' style='width:auto;margin-right:8px'%s>"
    " Simuleer S0-pulsen (solar, SCH afname, SCH injectie)</label>"
    "<div class='%s'>%s</div>"
    "</div>",
    SIM_S0 ? "" : " sim-ok",
    SIM_S0 ? " checked" : "",
    SIM_S0 ? "sim-warn" : "sim-ok-txt",
    SIM_S0
      ? "⚠️ SIMULATIE AAN — zet UIT na S0-bekabeling (IO5/IO6/IO7 via Roomsense RJ45)"
      : "✅ LIVE — hardware S0-pulsen actief (IO5=solar, IO6=SCH afname, IO7=SCH injectie)");

  // ── P1 Simulatie sectie ──────────────────────────────────
  p->printf("<div class='sim-block%s'>"
    "<div class='sim-title'>🔌 P1-dongle — HomeWizard HWE-P1-RJ12 (WON, ~2028)</div>"
    "<label><input type='checkbox' name='sim_p1' value='1' style='width:auto;margin-right:8px'%s>"
    " Simuleer P1-dongle data (WON afname + injectie)</label><br><br>"
    "<b style='font-size:13px'>IP-adres HomeWizard P1 Meter:</b><br>"
    "<input name='p1_ip' value='%s' placeholder='bv. 192.168.0.80 (leeg = niet actief)'"
    " style='margin-top:4px'><br>"
    "<div style='font-size:11px;color:#666;margin-top:4px'>"
    "Activeer Local API via HomeWizard Energy app: Settings → Meters → Local API<br>"
    "API endpoint: GET http://&lt;IP&gt;/api/v1/data (geen auth, plain HTTP)<br>"
    "Docs: <a href='https://api-documentation.homewizard.com/docs/introduction/' target='_blank'>"
    "api-documentation.homewizard.com</a></div>"
    "<div class='%s'>%s</div>"
    "</div>",
    SIM_P1 ? "" : " sim-ok",
    SIM_P1 ? " checked" : "",
    p1_ip,
    SIM_P1 ? "sim-warn" : "sim-ok-txt",
    SIM_P1
      ? "⚠️ SIMULATIE AAN — zet UIT na plaatsing HomeWizard P1 Meter (~2028)"
      : (strlen(p1_ip) > 0
          ? "✅ LIVE — pollt HomeWizard P1 Meter elke 5s"
          : "⚠️ LIVE modus maar geen IP ingesteld — stel IP in!"));

  p->print(F("<button class='btn' type='submit'>Opslaan &amp; Reboot</button>"
    "<button class='btn btn-red' type='button' "
    "onclick=\"if(confirm('Factory reset?')) location.href='/factory_reset';\">"
    "Factory Reset</button></form>"
    "<hr style='margin:14px 0;border:none;border-top:1px solid #ddd;'>"
    "<form action='/reset_dag' method='get'>"
    "<button class='btn' onclick=\"return confirm('Dagcumulatieven resetten?');\""
    " type='submit'>Reset dag Wh</button></form>"
    "<form action='/reset_piek' method='get' style='margin-top:6px;'>"
    "<button class='btn' onclick=\"return confirm('Maandpiek resetten?');\""
    " type='submit'>Reset maandpiek</button></form>"
    "<script>document.getElementById('sf').onsubmit=function(e){"
    "const ip=this.ip.value.trim();"
    "if(ip&&!/^(\\d{1,3}\\.){3}\\d{1,3}$/.test(ip)){alert('Ongeldig statisch IP!');e.preventDefault();return false;}"
    "const p1=this.p1_ip.value.trim();"
    "if(p1&&!/^(\\d{1,3}\\.){3}\\d{1,3}$/.test(p1)){alert('Ongeldig P1 IP-adres!');e.preventDefault();return false;}"
    "if(!this.ssid.value.trim()){alert('SSID verplicht!');e.preventDefault();return false;}"
    "return true;};</script>"
    "</div></body></html>"));
  req->send(p);
}

// ── OTA PAGINA ───────────────────────────────────────────────
void serveOTA(AsyncWebServerRequest *req) {
  AsyncResponseStream *p = req->beginResponseStream("text/html;charset=utf-8");
  p->print(F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OTA</title><style>"
    "body{font-family:Arial,sans-serif;margin:0;background:#f4f4f4;}"
    ".hdr{background:#ffcc00;padding:10px 15px;font-weight:bold;font-size:17px;}"
    ".nav{display:flex;flex-wrap:wrap;gap:5px;padding:7px 12px;background:#fff;"
    "border-bottom:2px solid #ddd;}"
    ".nav a{background:#369;color:#fff;padding:5px 11px;border-radius:4px;"
    "text-decoration:none;font-size:13px;}"
    ".nav a:hover{background:#036;}.nav a.act{background:#c00;}"
    ".main{padding:18px;max-width:400px;}"
    ".btn{background:#369;color:#fff;padding:9px 20px;border:none;border-radius:5px;"
    "font-size:14px;cursor:pointer;margin-top:10px;}"
    ".btn:hover{background:#036;}.btn-red{background:#c00;}"
    "</style></head><body>"
    "<div class='hdr'>OTA Firmware Update</div>"
    "<div class='nav'><a href='/'>Status</a><a href='/json'>JSON</a><a href='/tesla'>Tesla</a>"
    "<a href='/update' class='act'>OTA</a><a href='/settings'>Settings</a>"
    "</div><div class='main'>"
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<p>Selecteer .bin bestand:</p>"
    "<input type='file' name='update' accept='.bin'><br><br>"
    "<button class='btn' type='submit'>Upload firmware</button></form><br>"
    "<button class='btn btn-red' onclick=\"location.href='/reboot'\">Reboot</button>"
    "</div></body></html>"));
  req->send(p);
}

// ── BOOT ANIMATIE ───────────────────────────────────────────
void bootAnim() {
  for (int c = 0; c < MATRIX_COLS; c++) {
    for (int r = 0; r < MATRIX_ROWS; r++)
      strip.setPixelColor(pxIdx(c, r), strip.Color(0, 40, 0));
    strip.show(); delay(40);
  }
  // Rode knippering als één of beide kanalen in simulatie staan
  if (SIM_S0 || SIM_P1) {
    for (int i = 0; i < 4; i++) {
      strip.fill(strip.Color(30, 0, 0)); strip.show(); delay(150);
      strip.clear(); strip.show(); delay(100);
    }
  } else {
    delay(200);
    strip.fill(strip.Color(25, 25, 25)); strip.show(); delay(100);
  }
  strip.clear(); strip.show();
}

// ── WIFI STARTEN ────────────────────────────────────────────
void startWiFi() {
  if (strlen(wifi_ssid) == 0) goto start_ap;
  {
    IPAddress sip, gw, sn;
    if (strlen(static_ip) > 0 && sip.fromString(static_ip)) {
      gw.fromString("192.168.0.1"); sn.fromString("255.255.255.0");
      WiFi.config(sip, gw, sn);
    }
    WiFi.begin(wifi_ssid, wifi_pass);
    Serial.print(F("[WiFi] Verbinden"));
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) {
      delay(250); Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nVerbonden: " + WiFi.localIP().toString());
      ap_mode = false; configTzTime(TZ_INFO, NTP_SRV); return;
    }
  }
start_ap:
  Serial.println(F("\n[WiFi] Geen verbinding — AP modus"));
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID);
  dnsServer.start(53, "*", WiFi.softAPIP());
  ap_mode = true;
  Serial.printf("[AP] %s  %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

// ── SETUP ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(300);
  Serial.println(F("\n=== S-ENERGY v" FW_VERSION " boot ==="));
  Serial.println(F("Commando's: status | sim s0 on/off | sim p1 on/off | reset_nvs | help"));

  prefs.begin(NVS_NS, false);
  wh_sol     = prefs.getFloat(NVS_SOL,  0.0f);
  wh_schf    = prefs.getFloat(NVS_SCHF, 0.0f);
  wh_schr    = prefs.getFloat(NVS_SCHR, 0.0f);
  piek_w     = prefs.getFloat(NVS_PIEK, 0.0f);
  max_piek_w = prefs.getUInt(NVS_MPIEK, 10000);
  SIM_S0     = prefs.getBool(NVS_SIM_S0, true);   // default: simulatie AAN
  SIM_P1     = prefs.getBool(NVS_SIM_P1, true);   // default: simulatie AAN
  strlcpy(wifi_ssid,  prefs.getString(NVS_SSID, DEF_SSID).c_str(), sizeof(wifi_ssid));
  strlcpy(wifi_pass,  prefs.getString(NVS_PASS, DEF_PASS).c_str(), sizeof(wifi_pass));
  strlcpy(static_ip,  prefs.getString(NVS_IP,   DEF_IP).c_str(),   sizeof(static_ip));
  strlcpy(p1_ip,      prefs.getString(NVS_P1_IP, "").c_str(),       sizeof(p1_ip));
  last_day   = prefs.getInt(NVS_DAY, -1);
  prefs.getString(NVS_T_HOST, t1_host, sizeof(t1_host));
  prefs.getString(NVS_T_VIN,  t1_vin,  sizeof(t1_vin));
  t1_maxa    = (uint8_t)constrain((int)prefs.getUChar(NVS_T_MAXA, DEF_TESLA_MAXA), TESLA_AMPS_MIN, TESLA_AMPS_CAP);
  uint8_t bri = prefs.getUChar(NVS_BRI, DEF_BRIGHT);

  Serial.printf("[NVS] Sol:%.0f SchF:%.0f SchR:%.0f Piek:%.0fW\n",
    wh_sol, wh_schf, wh_schr, piek_w);
  Serial.printf("[SIM] S0=%s  P1=%s  P1-IP=%s\n",
    SIM_S0 ? "SIM" : "LIVE", SIM_P1 ? "SIM" : "LIVE",
    strlen(p1_ip) > 0 ? p1_ip : "(geen)");

  strip.begin(); strip.setBrightness(bri); strip.clear(); strip.show();
  bootAnim();

  // S0 interrupts — altijd registreren (in sim-modus pulseren de pinnen niet)
  pinMode(S0_SOL_PIN,  INPUT);
  pinMode(S0_SCHF_PIN, INPUT);
  pinMode(S0_SCHR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(S0_SOL_PIN),  isrSol,  FALLING);
  attachInterrupt(digitalPinToInterrupt(S0_SCHF_PIN), isrSchF, FALLING);
  attachInterrupt(digitalPinToInterrupt(S0_SCHR_PIN), isrSchR, FALLING);
  Serial.println(F("[S0] Interrupts geregistreerd IO5/IO6/IO7"));

  startWiFi();

  server.on("/",         HTTP_GET, serveStatus);
  server.on("/json",     HTTP_GET, serveJson);
  server.on("/update",   HTTP_GET, serveOTA);
  server.on("/settings", HTTP_GET, serveSettings);
  server.on("/tesla",      HTTP_GET,  [](AsyncWebServerRequest *req) { req->send_P(200, "text/html", T1_HTML); });
  server.on("/tesla_json", HTTP_GET,  serveTeslaJson);
  server.on("/tesla_cfg",  HTTP_GET,  serveTeslaCfg);
  server.on("/tesla_save", HTTP_GET,  handleTeslaSave);
  server.on("/tesla_cmd",  HTTP_POST, handleTeslaCmd);

  server.on("/save_settings", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasArg("ssid")) { strlcpy(wifi_ssid, req->arg("ssid").c_str(), sizeof(wifi_ssid)); prefs.putString(NVS_SSID, wifi_ssid); }
    if (req->hasArg("pass") && req->arg("pass").length() > 0) { strlcpy(wifi_pass, req->arg("pass").c_str(), sizeof(wifi_pass)); prefs.putString(NVS_PASS, wifi_pass); }
    if (req->hasArg("ip"))    { strlcpy(static_ip, req->arg("ip").c_str(), sizeof(static_ip)); prefs.putString(NVS_IP, static_ip); }
    if (req->hasArg("p1_ip")) { strlcpy(p1_ip, req->arg("p1_ip").c_str(), sizeof(p1_ip)); prefs.putString(NVS_P1_IP, p1_ip); }
    if (req->hasArg("bri"))   prefs.putUChar(NVS_BRI, (uint8_t)req->arg("bri").toInt());
    if (req->hasArg("mpiek")) { max_piek_w = req->arg("mpiek").toInt(); prefs.putUInt(NVS_MPIEK, max_piek_w); }

    // ⚠️ BEWUST handmatig — nooit automatisch omschakelen!
    SIM_S0 = req->hasArg("sim_s0"); prefs.putBool(NVS_SIM_S0, SIM_S0);
    SIM_P1 = req->hasArg("sim_p1"); prefs.putBool(NVS_SIM_P1, SIM_P1);
    Serial.printf("[SIM] Opgeslagen — S0=%s P1=%s P1-IP=%s\n",
      SIM_S0 ? "SIM" : "LIVE", SIM_P1 ? "SIM" : "LIVE", p1_ip);

    req->send(200, "text/html",
      "<h2 style='text-align:center;padding:30px;color:#369;'>"
      "Opgeslagen &mdash; Rebooting...</h2>"
      "<script>setTimeout(()=>location.href='/',2500);</script>");
    delay(500); ESP.restart();
  });

  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *req) {
      bool ok = !Update.hasError();
      req->send(200, "text/html", ok
        ? "<h2 style='color:green;text-align:center;padding:30px'>Update OK — Rebooting...</h2>"
        : "<h2 style='color:red;text-align:center;padding:30px'>MISLUKT</h2><a href='/update'>Opnieuw</a>");
      if (ok) { delay(1000); ESP.restart(); }
    },
    [](AsyncWebServerRequest *req, String fn, size_t idx, uint8_t *data, size_t len, bool fin) {
      if (!idx) { Update.begin(UPDATE_SIZE_UNKNOWN); }
      Update.write(data, len);
      if (fin && Update.end(true)) Serial.println(F("[OTA] OK"));
    });

  server.on("/factory_reset", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html", "<h2 style='text-align:center;padding:30px;color:#c00'>Factory reset — Rebooting...</h2>");
    delay(300); prefs.clear(); delay(200); ESP.restart();
  });
  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html", "<h2 style='text-align:center;padding:30px'>Rebooting...</h2>");
    delay(500); ESP.restart();
  });
  server.on("/reset_dag", HTTP_GET, [](AsyncWebServerRequest *req) {
    wh_sol = wh_schf = wh_schr = wh_won_imp = wh_won_exp = 0; saveEnergy();
    req->send(200, "text/plain", "Dagcumulatieven gereset");
  });
  server.on("/reset_piek", HTTP_GET, [](AsyncWebServerRequest *req) {
    piek_w = 0; saveEnergy();
    req->send(200, "text/plain", "Maandpiek gereset");
  });

  if (ap_mode) {
    server.onNotFound([](AsyncWebServerRequest *r) { r->redirect("/settings"); });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/settings"); });
    server.on("/generate_204",        HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/settings"); });
    server.on("/ncsi.txt",            HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/settings"); });
  }

  server.begin();
  fetchEpex();
  t_epex = t_15m = t_p1 = millis();
  t1_next = millis() + 5000UL;
  Serial.printf("[T1] %s host=%s maxA=%d\n", t1Enabled() ? "AAN" : "UIT (host/VIN leeg)", t1_host, t1_maxa);

  Serial.printf("[HEAP] %d bytes  %dKB largest\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap()/1024);
  Serial.printf("HTTP: http://%s\n",
    ap_mode ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str());
  Serial.println(F("=== Setup klaar ===\n"));
}

// ── LOOP ────────────────────────────────────────────────────
void loop() {
  if (ap_mode) dnsServer.processNextRequest();
  handleSerialCommands();

  if (!ap_mode) {
    wl_status_t ws = WiFi.status();
    if (ws != last_wifi_st) {
      if (ws != WL_CONNECTED) WiFi.reconnect();
      last_wifi_st = ws;
    }
  }

  unsigned long now = millis();

  // ── 5s tick ──────────────────────────────────────────────
  if (now - t_5s >= 5000) {
    t_5s = now;
    checkMidnight();

    // S0 kanalen: simulatie OF live — nooit automatisch
    if (SIM_S0) simTickS0();
    else        liveTickS0();

    // P1 dongle: simulatie OF live — nooit automatisch
    if (SIM_P1) simTickP1();
    else        fetchP1();   // HomeWizard API v1 — elke 5s pollen

    updateMatrix();

    Serial.printf("[%s|%s][%5lus] Sol:%4.0fW SCHf:%4.0fW SCHr:%4.0fW"
                  " WON:%+5.0fW EPEX:%.1fct Heap:%dKB\n",
      SIM_S0 ? "S0-SIM" : "S0-LIVE",
      SIM_P1 ? "P1-SIM" : "P1-LIVE",
      now / 1000, w_sol, w_schf, w_schr, w_won,
      epex_nu, ESP.getMaxAllocHeap() / 1024);
  }

  // ── 15 min: NVS opslaan ──────────────────────────────────
  if (now - t_15m >= 900000UL) {
    t_15m = now;
    saveEnergy();
  }

  // ── 15 min: EPEX herladen ────────────────────────────────
  if (now - t_epex >= 900000UL) {
    t_epex = now;
    fetchEpex();
  }

  // ── Tesla1: commando of poll (blokkerend met deadline) ────
  if (t1Enabled() && WiFi.status() == WL_CONNECTED) {
    if (t1_cmd) t1RunCmd();
    else if ((int32_t)(now - t1_next) >= 0) t1Poll();
  }
}
