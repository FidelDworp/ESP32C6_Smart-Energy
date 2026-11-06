/*
 * TeslaChargeTest_24sep26_1500.ino = Tesla Test Controller – ESP32-C6
 * Versie: 24 sep 2026 – range in km + Raw JSON
 * Libraries: ESPAsyncWebServer, AsyncTCP, ArduinoJson
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ========== EIGEN DATA ==========
const char* WIFI_SSID      = "Delannoy";
const char* WIFI_PASS      = "kampendaal,34";
const char* TESLA_KEY_HOST = "192.168.0.134";        // of tesla-key-esp32.local
const char* TESLA_VIN      = "LRW3E7EK5RC965673";   // jouw VIN
// ==============================

AsyncWebServer server(80);

String  lastJson   = "{}";
String  lastError  = "";
unsigned long lastFetch = 0;

// ---------- HTTP helpers ----------
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

// ---------- HTML ----------
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
    .ok{background:#d4edda;color:#155724}.warn{background:#fff3cd;color:#856404}.bad{background:#f8d7da;color:#721c24}
    pre.raw{background:#1e1e1e;color:#d4d4d4;padding:12px;border-radius:8px;font-size:11px;
            overflow:auto;max-height:420px;white-space:pre-wrap;word-break:break-all}
  </style>
</head>
<body>
  <div class="hdr">Tesla Test Controller <small style="font-weight:400;opacity:.8">– range km + raw JSON</small></div>

  <!-- CHARGE -->
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

  <!-- CLIMATE -->
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

  <!-- VEHICLE / CLOSURES -->
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

  <!-- TYRES -->
  <div class="card">
    <h2>Tyre pressure (bar)</h2>
    <div class="grid3">
      <div><div class="label">FL</div><div class="value" id="tpFL">—</div></div>
      <div><div class="label">FR</div><div class="value" id="tpFR">—</div></div>
      <div><div class="label">RL</div><div class="value" id="tpRL">—</div></div>
      <div><div class="label">RR</div><div class="value" id="tpRR">—</div></div>
    </div>
  </div>

  <!-- CONTROLS -->
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

  <!-- RAW JSON -->
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

    // Raw JSON altijd tonen
    document.getElementById('rawJson').textContent = JSON.stringify(d, null, 2);

    const cs = d.charge_state || {};
    const cl = d.climate_state || {};
    const vs = d.vehicle_state || d.closures_state || {};
    const tp = d.tire_pressure_state || d.tyre_pressure_state || {};

    // Charge
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

    // Range: Tesla levert miles → omrekenen naar km
    const rangeMi = cs.battery_range ?? cs.ideal_battery_range;
    document.getElementById('range').textContent = rangeMi != null
      ? num(rangeMi * 1.60934, 1, 'km')
      : '—';

    document.getElementById('port').innerHTML     = boolBadge(cs.charge_port_door_open);
    document.getElementById('latch').textContent  = cs.charge_port_latch || '—';

    // Climate
    document.getElementById('inside').textContent  = num(cl.inside_temp, 1, '°C');
    document.getElementById('outside').textContent = num(cl.outside_temp, 1, '°C');
    document.getElementById('drvTemp').textContent = num(cl.driver_temp_setting, 1, '°C');
    document.getElementById('pasTemp').textContent = num(cl.passenger_temp_setting, 1, '°C');
    document.getElementById('climateOn').innerHTML = boolBadge(cl.is_climate_on);
    document.getElementById('autoCond').innerHTML  = boolBadge(cl.is_auto_conditioning_on);

    // Vehicle
    document.getElementById('locked').innerHTML  = boolBadge(vs.locked);
    document.getElementById('user').innerHTML    = boolBadge(vs.is_user_present);
    document.getElementById('asleep').innerHTML  = boolBadge(d.is_asleep ?? vs.is_asleep);
    const odo = vs.odometer;
    document.getElementById('odo').textContent   = odo != null ? num(odo * 1.60934, 1, 'km') : '—';
    document.getElementById('frunk').innerHTML   = boolBadge(vs.ft ?? vs.front_trunk);
    document.getElementById('trunk').innerHTML   = boolBadge(vs.rt ?? vs.rear_trunk);

    // Tyres
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

// ---------- Setup ----------
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

    // tesla-key-esp32: vaak { response: { response: { ... } } }
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
