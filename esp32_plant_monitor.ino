/*
 * ESP32 Smart Plant Monitor  (v2)
 * --------------------------------------------------------------
 * Sensors : Soil moisture (analog, GPIO34), BH1750 light (I2C)
 * Display : SSD1306 128x64 OLED (I2C, 0x3C), 2 auto-rotating pages
 * Web     : 100% offline dashboard (no CDN) at http://plantmonitor.local
 *
 * New in v2:
 *  - Wi-Fi fallback: if router not found, starts its own hotspot
 *  - mDNS name  (http://plantmonitor.local)
 *  - Non-blocking loop, smoothed (EMA) moisture reading
 *  - History kept on the ESP32 -> graph survives page refresh
 *  - Proper charts with axes; light chart uses log scale
 *  - Dry / Good / Wet status + light level label
 *  - Calibrate dry/wet from the web page (saved in flash)
 *  - Adjustable dry threshold (saved in flash)
 *  - Min / Max since boot, Wi-Fi signal, uptime
 *  - OPTIONAL automatic watering pump via relay (disabled by default)
 *
 * Libraries (Library Manager): Adafruit GFX, Adafruit SSD1306, BH1750 (Christopher Laws)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BH1750.h>

// ===================== USER SETTINGS =====================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* HOSTNAME      = "plantmonitor";    // -> http://plantmonitor.local
const char* AP_SSID       = "PlantMonitor";    // fallback hotspot
const char* AP_PASSWORD   = "12345678";        // min 8 chars

// Optional watering pump (relay). Set to 1 only after wiring it safely.
#define ENABLE_PUMP        0
#define PUMP_PIN           26
#define PUMP_ACTIVE_LEVEL  HIGH                // use LOW for active-low relay boards
const unsigned long PUMP_RUN_MS      = 5000UL;     // watering burst length
const unsigned long PUMP_COOLDOWN_MS = 600000UL;   // min 10 min between bursts
// =========================================================

// --- Display ---
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

// --- Pins ---
#define MOISTURE_PIN 34
#define LED_PIN      2          // onboard LED: blinks when soil is dry

// --- Timing ---
const unsigned long SENSOR_INTERVAL_MS  = 1000;
const unsigned long HISTORY_INTERVAL_MS = 10000;   // 1 point / 10 s
const unsigned long PAGE_INTERVAL_MS    = 4000;
#define HISTORY_SIZE 180                           // 180 x 10 s = 30 minutes

WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
BH1750 lightMeter;
Preferences prefs;

// --- Settings stored in flash ---
int  airVal   = 3500;   // raw reading in dry air
int  waterVal = 1500;   // raw reading in water
int  dryThr   = 30;     // % below which soil is "Dry"
bool autoPump = false;

// --- Live state ---
float smoothRaw   = 0;
bool  rawPrimed   = false;
int   moisturePct = 0;
float lux         = 0;
int   minMoist    = 100, maxMoist = 0;
float minLux      = 1e9, maxLux   = 0;
bool  apMode      = false;

// --- History ring buffer ---
uint8_t  histM[HISTORY_SIZE];
uint16_t histL[HISTORY_SIZE];     // lux capped at 65535
int      histCount = 0;
int      histHead  = 0;           // next write position

// --- Pump state ---
bool          pumpOn = false;
bool          pumpEverRun = false;
unsigned long pumpStartMs = 0, pumpEndMs = 0;

// ===================== WEB PAGE =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Plant Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#0b1120;color:#f8fafc;padding:16px}
.wrap{max-width:620px;margin:0 auto}
h1{font-size:1.4rem;text-align:center}
.sub{text-align:center;color:#64748b;font-size:.8rem;margin:4px 0 16px}
.cards{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.card,.box{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:14px}
.box{margin-bottom:12px}
.t{font-size:.7rem;text-transform:uppercase;letter-spacing:.05em;color:#94a3b8}
.v{font-size:1.8rem;font-weight:bold;margin-top:4px}
.s{font-size:.8rem;margin-top:2px;color:#94a3b8}
.m{color:#38bdf8}.l{color:#facc15}
.badge{display:inline-block;padding:2px 10px;border-radius:99px;font-size:.75rem;font-weight:bold;color:#0b1120}
.b-dry{background:#f87171}.b-good{background:#4ade80}.b-wet{background:#38bdf8}
svg{width:100%;height:170px;background:#0f172a;border-radius:6px;margin-top:8px}
.row{display:flex;justify-content:space-between;font-size:.8rem;color:#cbd5e1;padding:3px 0}
button{background:#334155;color:#f8fafc;border:0;border-radius:8px;padding:9px 12px;font-size:.8rem;cursor:pointer;margin:4px 4px 0 0}
button:active{background:#475569}
input[type=range]{width:100%;margin-top:8px}
.msg{font-size:.75rem;color:#4ade80;min-height:1em;margin-top:6px}
</style>
</head>
<body>
<div class="wrap">
  <h1>&#127793; Plant Monitor</h1>
  <div class="sub" id="sub">connecting...</div>

  <div class="cards">
    <div class="card">
      <div class="t">Soil Moisture</div>
      <div class="v m" id="vm">-- %</div>
      <div class="s"><span id="badge" class="badge b-good">--</span></div>
    </div>
    <div class="card">
      <div class="t">Ambient Light</div>
      <div class="v l" id="vl">-- lx</div>
      <div class="s" id="ll">--</div>
    </div>
  </div>

  <div class="box">
    <div class="t">Moisture &middot; last 30 min</div>
    <svg id="cm" viewBox="0 0 500 170"></svg>
  </div>
  <div class="box">
    <div class="t">Light (log scale) &middot; last 30 min</div>
    <svg id="cl" viewBox="0 0 500 170"></svg>
  </div>

  <div class="box">
    <div class="t" style="margin-bottom:6px">Stats</div>
    <div class="row"><span>Moisture min / max</span><span id="smm">--</span></div>
    <div class="row"><span>Light min / max</span><span id="sll">--</span></div>
    <div class="row"><span>Raw ADC (dry / wet cal)</span><span id="sraw">--</span></div>
    <div class="row"><span>Wi-Fi</span><span id="swifi">--</span></div>
    <div class="row"><span>Uptime</span><span id="sup">--</span></div>
  </div>

  <div class="box">
    <div class="t">Settings</div>
    <div class="s" style="margin-top:8px">Dry threshold: <b id="thrv">30</b> %</div>
    <input type="range" id="thr" min="5" max="80" value="30">
    <div style="margin-top:8px">
      <button onclick="cal('air')">Calibrate DRY (sensor in air)</button>
      <button onclick="cal('water')">Calibrate WET (sensor in water)</button>
    </div>
    <div id="pumpbox" style="display:none;margin-top:8px">
      <label class="s"><input type="checkbox" id="auto"> Auto-water when dry</label><br>
      <button onclick="pump()">Water now</button>
      <span class="s" id="pstate"></span>
    </div>
    <div class="msg" id="msg"></div>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
let dryThr = 30;

function luxLabel(x){
  if(x<10) return 'Dark';
  if(x<200) return 'Dim / indoor shade';
  if(x<1000) return 'Indoor bright';
  if(x<10000) return 'Bright / cloudy day';
  return 'Direct sunlight';
}
function fmtUp(s){
  const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return (d?d+'d ':'')+h+'h '+m+'m';
}
function msg(t){ $('msg').innerText=t; setTimeout(()=>$('msg').innerText='',3500); }

function chart(svgId, data, color, yFn, ticks, fmt){
  const W=500,H=170,L=38,R=8,T=8,B=8;
  let s='';
  ticks.forEach(tk=>{
    const y=T+(H-T-B)*(1-yFn(tk));
    s+='<line x1="'+L+'" y1="'+y+'" x2="'+(W-R)+'" y2="'+y+'" stroke="#1e293b"/>';
    s+='<text x="'+(L-4)+'" y="'+(y+3)+'" fill="#64748b" font-size="9" text-anchor="end">'+fmt(tk)+'</text>';
  });
  if(data.length>1){
    const step=(W-L-R)/(Math.max(data.length,60)-1);
    let p='';
    data.forEach((v,i)=>{ p+=(L+i*step).toFixed(1)+','+(T+(H-T-B)*(1-yFn(v))).toFixed(1)+' '; });
    s+='<polyline fill="none" stroke="'+color+'" stroke-width="2.5" stroke-linejoin="round" points="'+p+'"/>';
  }
  $(svgId).innerHTML=s;
}
const logY = v => Math.min(1, Math.log10(Math.max(v,0)+1)/5);   // 0..100000 lx
const luxFmt = v => v>=1000 ? (v/1000)+'k' : v;

async function loadHistory(){
  try{
    const h = await (await fetch('/api/history')).json();
    chart('cm', h.m, '#38bdf8', v=>v/100, [0,25,50,75,100], v=>v+'%');
    chart('cl', h.l, '#facc15', logY, [0,10,100,1000,10000,100000], luxFmt);
  }catch(e){}
}

async function loadData(){
  try{
    const d = await (await fetch('/api/data?t='+Date.now())).json();
    dryThr = d.dry;
    $('vm').innerText = d.moisture+' %';
    $('vl').innerText = d.lux.toFixed(1)+' lx';
    $('ll').innerText = luxLabel(d.lux);
    const b=$('badge');
    if(d.moisture<d.dry){ b.className='badge b-dry'; b.innerText='Dry - needs water'; }
    else if(d.moisture>80){ b.className='badge b-wet'; b.innerText='Wet'; }
    else { b.className='badge b-good'; b.innerText='Good'; }
    $('smm').innerText = d.minM+'% / '+d.maxM+'%';
    $('sll').innerText = d.minL.toFixed(0)+' / '+d.maxL.toFixed(0)+' lx';
    $('sraw').innerText = d.raw+'  ('+d.air+' / '+d.water+')';
    $('swifi').innerText = d.ap ? 'Hotspot mode' : (d.rssi+' dBm');
    $('sup').innerText = fmtUp(d.up);
    $('sub').innerText = d.ip+'  |  offline mode';
    if(document.activeElement!==$('thr')){ $('thr').value=d.dry; $('thrv').innerText=d.dry; }
    if(d.pumpEnabled){
      $('pumpbox').style.display='block';
      if(document.activeElement!==$('auto')) $('auto').checked=d.auto;
      $('pstate').innerText = d.pumpOn ? ' pump running...' : '';
    }
  }catch(e){ $('sub').innerText='connection lost...'; }
}

async function cal(t){
  const r = await (await fetch('/api/calibrate?type='+t)).json();
  msg(r.ok ? 'Saved '+t+' = '+r.value : 'Rejected: dry and wet values too close');
  loadData();
}
async function pump(){
  const r = await (await fetch('/api/pump')).json();
  msg(r.ok ? 'Watering...' : r.reason);
}
function saveCfg(){
  fetch('/api/config?dry='+$('thr').value+'&auto='+($('auto').checked?1:0));
}
$('thr').oninput = ()=>{ $('thrv').innerText=$('thr').value; };
$('thr').onchange = saveCfg;
$('auto').onchange = saveCfg;

setInterval(loadData,1500);
setInterval(loadHistory,10000);
loadData(); loadHistory();
</script>
</body>
</html>
)rawliteral";

// ===================== HELPERS =====================
void loadSettings() {
  prefs.begin("plant", true);
  airVal   = prefs.getInt("air", 3500);
  waterVal = prefs.getInt("water", 1500);
  dryThr   = prefs.getInt("dry", 30);
  autoPump = prefs.getBool("auto", false);
  prefs.end();
}

void saveSettings() {
  prefs.begin("plant", false);
  prefs.putInt("air", airVal);
  prefs.putInt("water", waterVal);
  prefs.putInt("dry", dryThr);
  prefs.putBool("auto", autoPump);
  prefs.end();
}

int rawToPercent(float raw) {
  if (airVal == waterVal) return 0;
  float p = (airVal - raw) * 100.0f / (float)(airVal - waterVal);
  return constrain((int)lroundf(p), 0, 100);
}

String ipString() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

// ===================== PUMP =====================
bool startPump(String &reason) {
#if ENABLE_PUMP
  if (pumpOn) { reason = "Already running"; return false; }
  if (pumpEverRun && millis() - pumpEndMs < PUMP_COOLDOWN_MS) {
    reason = "Cooldown active, try later";
    return false;
  }
  pumpOn = true;
  pumpStartMs = millis();
  digitalWrite(PUMP_PIN, PUMP_ACTIVE_LEVEL);
  return true;
#else
  reason = "Pump disabled in firmware";
  return false;
#endif
}

void handlePump() {
#if ENABLE_PUMP
  if (pumpOn && millis() - pumpStartMs >= PUMP_RUN_MS) {
    digitalWrite(PUMP_PIN, !PUMP_ACTIVE_LEVEL);
    pumpOn = false;
    pumpEverRun = true;
    pumpEndMs = millis();
  }
  if (autoPump && !pumpOn && moisturePct < dryThr) {
    String r;
    startPump(r);
  }
#endif
}

// ===================== HTTP HANDLERS =====================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
  String j;
  j.reserve(400);
  j += "{\"moisture\":" + String(moisturePct);
  j += ",\"raw\":" + String((int)smoothRaw);
  j += ",\"lux\":" + String(lux, 1);
  j += ",\"minM\":" + String(minMoist <= maxMoist ? minMoist : 0);
  j += ",\"maxM\":" + String(maxMoist);
  j += ",\"minL\":" + String(minLux > maxLux ? 0.0f : minLux, 1);
  j += ",\"maxL\":" + String(maxLux, 1);
  j += ",\"air\":" + String(airVal);
  j += ",\"water\":" + String(waterVal);
  j += ",\"dry\":" + String(dryThr);
  j += ",\"auto\":" + String(autoPump ? "true" : "false");
  j += ",\"pumpEnabled\":" + String(ENABLE_PUMP ? "true" : "false");
  j += ",\"pumpOn\":" + String(pumpOn ? "true" : "false");
  j += ",\"ap\":" + String(apMode ? "true" : "false");
  j += ",\"rssi\":" + String(apMode ? 0 : WiFi.RSSI());
  j += ",\"up\":" + String(millis() / 1000);
  j += ",\"ip\":\"" + ipString() + "\"}";
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", j);
}

void handleHistory() {
  String m = "[", l = "[";
  m.reserve(histCount * 4 + 4);
  l.reserve(histCount * 6 + 4);
  int start = (histCount < HISTORY_SIZE) ? 0 : histHead;   // oldest sample
  for (int i = 0; i < histCount; i++) {
    int idx = (start + i) % HISTORY_SIZE;
    if (i) { m += ","; l += ","; }
    m += String(histM[idx]);
    l += String(histL[idx]);
  }
  m += "]"; l += "]";
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", "{\"m\":" + m + ",\"l\":" + l + "}");
}

void handleCalibrate() {
  String type = server.arg("type");
  int value = (int)smoothRaw;
  int newAir = airVal, newWater = waterVal;
  if (type == "air") newAir = value;
  else if (type == "water") newWater = value;
  else { server.send(400, "application/json", "{\"ok\":false}"); return; }

  if (abs(newAir - newWater) < 300) {       // sanity check
    server.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  airVal = newAir; waterVal = newWater;
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true,\"value\":" + String(value) + "}");
}

void handleConfig() {
  if (server.hasArg("dry")) dryThr = constrain(server.arg("dry").toInt(), 5, 80);
  if (server.hasArg("auto")) autoPump = server.arg("auto") == "1";
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePumpApi() {
  String reason;
  bool ok = startPump(reason);
  server.send(200, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"reason\":\"" + reason + "\"}");
}

// ===================== SENSORS =====================
void readSensors() {
  // Moisture: average of 16 quick reads, then exponential smoothing
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogRead(MOISTURE_PIN); delayMicroseconds(200); }
  float raw = sum / 16.0f;
  smoothRaw = rawPrimed ? (0.3f * raw + 0.7f * smoothRaw) : raw;
  rawPrimed = true;
  moisturePct = rawToPercent(smoothRaw);

  // Light
  float l = lightMeter.readLightLevel();
  if (l >= 0) lux = l;       // ignore error values

  // Min / max
  minMoist = min(minMoist, moisturePct);
  maxMoist = max(maxMoist, moisturePct);
  minLux = min(minLux, lux);
  maxLux = max(maxLux, lux);

  // LED: blink when dry, off otherwise
  digitalWrite(LED_PIN, moisturePct < dryThr ? !digitalRead(LED_PIN) : LOW);
}

void pushHistory() {
  histM[histHead] = (uint8_t)moisturePct;
  histL[histHead] = (uint16_t)min(lux, 65535.0f);
  histHead = (histHead + 1) % HISTORY_SIZE;
  if (histCount < HISTORY_SIZE) histCount++;
}

// ===================== OLED =====================
void drawPage(int page) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print(apMode ? "AP: " : "IP: ");
  display.print(ipString());
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  if (page == 0) {
    display.setCursor(0, 15);
    display.print("Light: ");
    display.print(lux, 1);
    display.print(" lx");

    display.setCursor(0, 28);
    display.print("Moisture: ");
    display.print(moisturePct);
    display.print(" %");
    if (moisturePct < dryThr) display.print(" DRY");

    display.drawRect(0, 46, 128, 14, SSD1306_WHITE);
    int w = constrain(map(moisturePct, 0, 100, 0, 124), 0, 124);
    if (w > 0) display.fillRect(2, 48, w, 10, SSD1306_WHITE);
    // threshold marker
    int tx = 2 + map(dryThr, 0, 100, 0, 124);
    display.drawLine(tx, 44, tx, 62, SSD1306_WHITE);
  } else {
    display.setCursor(0, 15);
    display.print("Raw ADC: ");
    display.print((int)smoothRaw);
    display.setCursor(0, 26);
    display.print("Min/Max: ");
    display.print(minMoist);
    display.print("/");
    display.print(maxMoist);
    display.print("%");
    display.setCursor(0, 37);
    if (apMode) display.print("Wi-Fi: hotspot");
    else { display.print("Wi-Fi: "); display.print(WiFi.RSSI()); display.print(" dBm"); }
    display.setCursor(0, 48);
    unsigned long s = millis() / 1000;
    display.print("Up: ");
    display.print(s / 3600); display.print("h ");
    display.print((s % 3600) / 60); display.print("m");
#if ENABLE_PUMP
    if (pumpOn) display.print("  PUMP");
#endif
  }
  display.display();
}

void showMessage(const char* l1, const char* l2 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20); display.println(l1);
  display.setCursor(0, 34); display.println(l2);
  display.display();
}

// ===================== SETUP / LOOP =====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  showMessage("Connecting WiFi...", WIFI_SSID);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.print("\nConnected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.print("\nWi-Fi failed -> hotspot. IP: ");
    Serial.println(WiFi.softAPIP());
    showMessage("Hotspot mode", AP_SSID);
    delay(1500);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
#if ENABLE_PUMP
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, !PUMP_ACTIVE_LEVEL);   // pump OFF first
#endif

  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db);   // full 0-3.3V range
  loadSettings();

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 not found");
    for (;;) delay(1000);
  }
  display.setRotation(2);

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 not found - check wiring");
    showMessage("BH1750 not found!", "check SDA/SCL");
    delay(2000);
  }

  connectWiFi();

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/history", handleHistory);
  server.on("/api/calibrate", handleCalibrate);
  server.on("/api/config", handleConfig);
  server.on("/api/pump", handlePumpApi);
  server.begin();

  delay(300);           // let BH1750 finish its first measurement
  readSensors();        // prime values
  readSensors();
  minMoist = maxMoist = moisturePct;   // discard startup glitches
  minLux = maxLux = lux;
  pushHistory();
}

unsigned long lastSensor = 0, lastHistory = 0, lastPage = 0, lastWifiCheck = 0;
int page = 0;

void loop() {
  server.handleClient();
  unsigned long now = millis();

  if (now - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = now;
    readSensors();
    handlePump();
    drawPage(page);
  }

  if (now - lastHistory >= HISTORY_INTERVAL_MS) {
    lastHistory = now;
    pushHistory();
  }

  if (now - lastPage >= PAGE_INTERVAL_MS) {
    lastPage = now;
    page = (page + 1) % 2;
  }

  // Wi-Fi watchdog (station mode only)
  if (!apMode && now - lastWifiCheck >= 30000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi lost, reconnecting...");
      WiFi.reconnect();
    }
  }
}
