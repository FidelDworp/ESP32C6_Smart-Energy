/*
 * TeslaChargeTest.ino = Tesla Test Controller – ESP32-C6
 * Eenvoudige webinterface om data te bekijken en laden te sturen
 * via een tesla-key-esp32 (BLE proxy).
 *
 * Vereisten:
 *  - ESP32-C6 met WiFi
 *  - tesla-key-esp32 al geïnstalleerd + gepaard met de auto
 *  - Libraries: ESPAsyncWebServer, AsyncTCP, ArduinoJson
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ========== AANPASSEN ==========
const char* WIFI_SSID     = "jouw-wifi";
const char* WIFI_PASS     = "jouw-wachtwoord";

const char* TESLA_KEY_HOST = "192.168.0.xx";   // IP van tesla-key-esp32
// of:  "tesla-key-esp32.local"

const char* TESLA_VIN      = "5YJ3E7EA0JF000000";  // jouw VIN
// ==============================

AsyncWebServer server(80);

// Laatste opgehaalde data
String  lastJson     = "{}";
String  lastError    = "";
unsigned long lastFetch = 0;

// ---------- HTTP helpers ----------
String httpGet(const String& path) {
  HTTPClient http;
  String url = String("http://") + TESLA_KEY_HOST + path;
  http.begin(url);
  http.setTimeout(8000);
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
  http.setTimeout(10000);
  int code = http.POST(body);
  bool ok = (code == 200);
  if (!ok) lastError = "POST " + path + " → HTTP " + String(code);
  http.end();
  return ok;
}

// ---------- Data ophalen ----------
void fetchVehicleData() {
  String path = String("/api/1/vehicles/") + TESLA_VIN + "/vehicle_data";
  String body = httpGet(path);
  if (body.length() > 10) {
    lastJson = body;
    lastError = "";
  }
  lastFetch = millis();
}

// ---------- HTML pagina ----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Tesla Test Controller</title>
  <style>
    body { font-family: system-ui, Arial, sans-serif; margin: 0; background: #f0f2f5; color: #222; }
    .hdr { background: #cc0000; color: white; padding: 14px 18px; font-size: 18px; font-weight: 600; }
    .card { background: white; margin: 14px; border-radius: 10px; padding: 16px; box-shadow: 0 2px 8px rgba(0,0,0,.08); }
    h2 { margin: 0 0 12px; font-size: 15px; color: #555; text-transform: uppercase; letter-spacing: .5px; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px 16px; }
    .label { color: #666; font-size: 13px; }
    .value { font-size: 18px; font-weight: 600; }
    .btn { display: inline-block; padding: 10px 18px; margin: 4px 6px 4px 0; border: none; border-radius: 6px;
           font-size: 14px; font-weight: 600; cursor: pointer; color: white; }
    .btn-green { background: #2a8a3e; }
    .btn-red   { background: #c00; }
    .btn-blue  { background: #036; }
    .btn-gray  { background: #666; }
    .btn:active { opacity: .8; }
    input[type=range] { width: 100%; max-width: 280px; }
    .status { font-size: 13px; color: #666; margin-top: 8px; }
    .err { color: #c00; font-weight: 600; }
    #ampsVal { font-weight: 700; color: #036; }
  </style>
</head>
<body>
  <div class="hdr">Tesla Test Controller</div>

  <div class="card">
    <h2>Live data</h2>
    <div class="grid">
      <div><div class="label">Batterij</div><div class="value" id="soc">—</div></div>
      <div><div class="label">Laadstatus</div><div class="value" id="state">—</div></div>
      <div><div class="label">Huidige ampère</div><div class="value" id="amps">—</div></div>
      <div><div class="label">Vermogen</div><div class="value" id="power">—</div></div>
      <div><div class="label">Charge limit</div><div class="value" id="limit">—</div></div>
      <div><div class="label">Range</div><div class="value" id="range">—</div></div>
    </div>
    <div class="status" id="ts">Laatste update: —</div>
    <div class="status err" id="err"></div>
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
  </div>

  <div class="card">
    <h2>Extra</h2>
    <button class="btn btn-gray" onclick="cmd('charge_port_door_open')">Open laadpoort</button>
    <button class="btn btn-gray" onclick="cmd('charge_port_door_close')">Sluit laadpoort</button>
  </div>

<script>
function refresh() {
  fetch('/data').then(r => r.json()).then(d => {
    if (d.error) {
      document.getElementById('err').textContent = d.error;
      return;
    }
    document.getElementById('err').textContent = '';
    const cs = d.charge_state || {};
    document.getElementById('soc').textContent   = (cs.battery_level ?? '—') + ' %';
    document.getElementById('state').textContent = cs.charging_state || '—';
    document.getElementById('amps').textContent  = (cs.charge_amps ?? '—') + ' A';
    document.getElementById('power').textContent = (cs.charger_power ?? '—') + ' kW';
    document.getElementById('limit').textContent = (cs.charge_limit_soc ?? '—') + ' %';
    document.getElementById('range').textContent = (cs.battery_range ?? '—') + ' km';
    document.getElementById('ts').textContent    = 'Laatste update: ' + new Date().toLocaleTimeString('nl-BE');
  }).catch(e => {
    document.getElementById('err').textContent = 'Kan data niet ophalen';
  });
}

function cmd(name) {
  fetch('/cmd?c=' + name, { method: 'POST' })
    .then(r => r.text())
    .then(t => { alert(t); setTimeout(refresh, 1500); })
    .catch(() => alert('Fout bij commando'));
}

function setAmps() {
  const a = document.getElementById('ampsSlider').value;
  fetch('/cmd?c=set_charging_amps&amps=' + a, { method: 'POST' })
    .then(r => r.text())
    .then(t => { alert(t); setTimeout(refresh, 1500); })
    .catch(() => alert('Fout bij ampère instellen'));
}

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>
)rawliteral";

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Tesla Test Controller ===");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi verbinden");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  // Hoofdpagina
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  // Data endpoint (proxy)
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (millis() - lastFetch > 4000) fetchVehicleData();

    // Eenvoudige extractie van charge_state
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, lastJson);
    if (err || lastError.length()) {
      String e = lastError.length() ? lastError : "JSON parse error";
      req->send(200, "application/json", "{\"error\":\"" + e + "\"}");
      return;
    }

    // Geef alleen het relevante deel door
    JsonObject resp = doc["response"]["response"];
    if (resp.isNull()) resp = doc["response"];   // fallback

    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
  });

  // Commando endpoint
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
  Serial.println("Klaar → open http://" + WiFi.localIP().toString());
}

void loop() {
  // niets – alles async
  delay(1000);
}
