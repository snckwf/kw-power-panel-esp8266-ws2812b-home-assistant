/*
  ============================================================================
  Pannello_Kw  v2.0
  Wemos D1 mini v4 + matrice WS2812B 8x32 + Home Assistant
  ----------------------------------------------------------------------------
  - Mostra la potenza in W letta da Home Assistant (REST API)
  - Allarme buzzer NON bloccante sopra soglia configurabile
  - Interfaccia web di configurazione (colori, soglie, luminosita', HA, buzzer)
  - Configurazione persistente su LittleFS (/config.json)
  - OTA: ArduinoOTA (porta di rete nell'IDE) + pagina web /update
  - Ridisegno della matrice solo quando il valore cambia (niente sfarfallio)

  Librerie da installare:
  - ESP8266 by ESP8266 Community  (core, include LittleFS/WebServer/OTA/mDNS)
  - ArduinoJson        (compatibile v6 e v7)
  - Adafruit GFX Library
  - Adafruit NeoMatrix
  - Adafruit NeoPixel

  IDE - Impostazioni consigliate:
  - Scheda: LOLIN(WEMOS) D1 R2 & mini
  - Flash Size: 4MB (FS:1MB OTA:~1019KB)   <-- necessario per LittleFS + OTA
  - Upload Speed: 921600

  Primo avvio:
  1) Compila e carica via USB
  2) Il display mostra l'IP per alcuni secondi (scorrimento)
  3) Apri http://<IP>/  oppure  http://pannello-kw.local/
  4) Inserisci il Long-Lived Access Token di HA e salva
  ============================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// =========================
// WIFI  (unici dati non modificabili da web)
// =========================
const char* WIFI_SSID = "IL_TUO_SSID";
const char* WIFI_PASS = "LA_TUA_PASSWORD";

// =========================
// RETE / OTA
// =========================
const char* HOSTNAME      = "pannello-kw";   // -> http://pannello-kw.local
const char* OTA_PASSWORD  = "cambiami";      // password ArduinoOTA - CAMBIALA
const char* UPDATE_USER   = "admin";         // login pagina /update
const char* UPDATE_PASS   = "cambiami";      // password pagina /update - CAMBIALA

// =========================
// HARDWARE
// =========================
#define MATRIX_PIN  12   // D6 su Wemos D1 mini
#define BUZZER_PIN  14   // D5 su Wemos D1 mini

#define MATRIX_W 32
#define MATRIX_H 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_W, MATRIX_H, MATRIX_PIN,
  NEO_MATRIX_TOP  + NEO_MATRIX_LEFT +
  NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800
);

ESP8266WebServer        server(80);
ESP8266HTTPUpdateServer httpUpdater;

// =========================
// CONFIGURAZIONE PERSISTENTE
// =========================
#define CFG_PATH "/config.json"

struct Config {
  char     haHost[48];
  uint16_t haPort;
  char     haToken[220];
  char     haEntity[72];

  uint32_t colLow;        // 0xRRGGBB
  uint32_t colMid;
  uint32_t colHigh;

  float    thrMidW;       // sopra questa soglia -> colore medio
  float    thrHighW;      // sopra questa soglia -> colore alto
  float    alarmW;        // soglia allarme buzzer

  bool     buzzerOn;
  uint16_t alarmRepeatS;  // secondi tra due allarmi consecutivi
  uint8_t  brightness;    // 5..100
  uint16_t fetchS;        // secondi tra due letture da HA
  uint8_t  rotation;      // 0 = normale, 2 = ruotato 180 gradi
};

Config cfg;

void setDefaults() {
  strlcpy(cfg.haHost,   "192.168.1.7",                      sizeof(cfg.haHost));
  cfg.haPort = 8123;
  strlcpy(cfg.haToken,  "",                                 sizeof(cfg.haToken));
  strlcpy(cfg.haEntity, "sensor.potenza_totale_shelly_em",  sizeof(cfg.haEntity));

  cfg.colLow       = 0x007800;   // verde
  cfg.colMid       = 0xA06E00;   // giallo/ambra
  cfg.colHigh      = 0xAA0000;   // rosso

  cfg.thrMidW      = 1500.0;
  cfg.thrHighW     = 3000.0;
  cfg.alarmW       = 3000.0;

  cfg.buzzerOn     = true;
  cfg.alarmRepeatS = 15;
  cfg.brightness   = 35;
  cfg.fetchS       = 5;
  cfg.rotation     = 0;          // 0 = normale, 2 = 180 gradi
}

// =========================
// STATO
// =========================
float currentWatts  = 0.0;
bool  hasValidValue = false;
bool  overLimit     = false;

unsigned long lastFetchMs     = 0;
unsigned long lastWifiRetryMs = 0;
unsigned long lastAlarmMs     = 0;

// anti-sfarfallio: ridisegno solo su variazione
String   lastTxt      = "\x01";
uint16_t lastCol      = 0xFFFF;
bool     forceRedraw  = true;

// =========================
// COLORI
// =========================
uint16_t rgb565(uint32_t c) {
  return matrix.Color((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}
uint16_t colorBlue()  { return matrix.Color(0, 0, 120); }
uint16_t colorWhite() { return matrix.Color(120, 120, 120); }
uint16_t colorErr()   { return matrix.Color(170, 0, 0); }

String hex6(uint32_t c) {
  char b[10];
  snprintf(b, sizeof(b), "#%06X", (unsigned)(c & 0xFFFFFF));
  return String(b);
}
uint32_t parseHex6(const String& s) {
  String t = s;
  if (t.startsWith("#")) t = t.substring(1);
  return (uint32_t) strtoul(t.c_str(), nullptr, 16) & 0xFFFFFF;
}

// =========================
// DISPLAY
// =========================
void drawCenteredText(const String& txt, uint16_t color, bool force = false) {
  if (!force && !forceRedraw && txt == lastTxt && color == lastCol) return;

  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);

  int16_t  x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);

  int16_t x = (MATRIX_W - (int)w) / 2 - x1;
  int16_t y = (MATRIX_H - (int)h) / 2 - y1;
  if (x < 0) x = 0;
  if (y < 0) y = 0;

  matrix.setCursor(x, y);   // angolo alto-sx del testo, non la baseline
  matrix.setTextColor(color);
  matrix.print(txt);
  matrix.show();

  lastTxt     = txt;
  lastCol     = color;
  forceRedraw = false;
}

// scorrimento testo (usato solo per mostrare l'IP all'avvio)
void scrollText(const String& txt, uint16_t color, uint8_t speedMs = 60) {
  matrix.setTextWrap(false);
  matrix.setTextSize(1);
  matrix.setTextColor(color);

  int16_t  x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);

  int16_t ys = (MATRIX_H - (int)h) / 2 - y1;
  if (ys < 0) ys = 0;

  for (int16_t x = MATRIX_W; x > -(int16_t)w - 1; x--) {
    matrix.fillScreen(0);
    matrix.setCursor(x, ys);
    matrix.print(txt);
    matrix.show();
    delay(speedMs);
    yield();
  }
  forceRedraw = true;
}

uint16_t powerColor(float watts) {
  if (watts >= cfg.thrHighW) return rgb565(cfg.colHigh);
  if (watts >= cfg.thrMidW)  return rgb565(cfg.colMid);
  return rgb565(cfg.colLow);
}

String formatPower(float watts) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%.0f", watts);   // watt interi, es. 2450
  return String(buf);
}

void showPower(float watts) {
  drawCenteredText(formatPower(watts), powerColor(watts));
}

// =========================
// BUZZER NON BLOCCANTE
// Melodia: La4 - Do5 - Mi5 - Do5
// =========================
const uint16_t MEL_NOTES[] = {440, 523, 659, 523};
const uint16_t MEL_DUR[]   = {150, 150, 230, 220};
const uint8_t  MEL_LEN     = 4;
const uint16_t MEL_GAP     = 45;

int8_t        melStep   = -1;     // -1 = fermo
bool          melInNote = false;
unsigned long melTs     = 0;

void melodyStart() {
  if (!cfg.buzzerOn) return;
  melStep   = 0;
  melInNote = false;
  melTs     = millis() - MEL_GAP;
}

void melodyStop() {
  melStep   = -1;
  melInNote = false;
  noTone(BUZZER_PIN);
}

void melodyTask() {
  if (melStep < 0) return;
  unsigned long now = millis();

  if (!melInNote) {
    if (now - melTs >= MEL_GAP) {
      if (melStep >= MEL_LEN) { melodyStop(); return; }
      tone(BUZZER_PIN, MEL_NOTES[melStep]);
      melInNote = true;
      melTs     = now;
    }
  } else {
    if (now - melTs >= MEL_DUR[melStep]) {
      noTone(BUZZER_PIN);
      melInNote = false;
      melTs     = now;
      melStep++;
    }
  }
}

void handleAlarm(float watts) {
  bool isOver = (watts >= cfg.alarmW);
  unsigned long now = millis();

  if (isOver) {
    if (!overLimit || (now - lastAlarmMs >= (unsigned long)cfg.alarmRepeatS * 1000UL)) {
      melodyStart();
      lastAlarmMs = now;
    }
  }
  overLimit = isOver;
}

// =========================
// CONFIG: LOAD / SAVE
// =========================
bool saveConfig() {
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) { Serial.println(F("[FS] open write fallito")); return false; }

  f.print(F("{\"haHost\":\""));      f.print(cfg.haHost);
  f.print(F("\",\"haPort\":"));      f.print(cfg.haPort);
  f.print(F(",\"haToken\":\""));     f.print(cfg.haToken);
  f.print(F("\",\"haEntity\":\""));  f.print(cfg.haEntity);
  f.print(F("\",\"colLow\":"));      f.print(cfg.colLow);
  f.print(F(",\"colMid\":"));        f.print(cfg.colMid);
  f.print(F(",\"colHigh\":"));       f.print(cfg.colHigh);
  f.print(F(",\"thrMidW\":"));       f.print(cfg.thrMidW, 1);
  f.print(F(",\"thrHighW\":"));      f.print(cfg.thrHighW, 1);
  f.print(F(",\"alarmW\":"));        f.print(cfg.alarmW, 1);
  f.print(F(",\"buzzerOn\":"));      f.print(cfg.buzzerOn ? 1 : 0);
  f.print(F(",\"alarmRepeatS\":"));  f.print(cfg.alarmRepeatS);
  f.print(F(",\"brightness\":"));    f.print(cfg.brightness);
  f.print(F(",\"fetchS\":"));        f.print(cfg.fetchS);
  f.print(F(",\"rotation\":"));      f.print(cfg.rotation);
  f.print(F("}"));
  f.close();

  Serial.println(F("[FS] config salvata"));
  return true;
}

bool loadConfig() {
  if (!LittleFS.exists(CFG_PATH)) {
    Serial.println(F("[FS] nessuna config, uso i default"));
    return false;
  }
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return false;

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(1024);
#endif

  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.print(F("[FS] JSON err: ")); Serial.println(err.c_str()); return false; }

  strlcpy(cfg.haHost,   doc["haHost"]   | cfg.haHost,   sizeof(cfg.haHost));
  strlcpy(cfg.haToken,  doc["haToken"]  | cfg.haToken,  sizeof(cfg.haToken));
  strlcpy(cfg.haEntity, doc["haEntity"] | cfg.haEntity, sizeof(cfg.haEntity));

  cfg.haPort       = doc["haPort"]       | cfg.haPort;
  cfg.colLow       = doc["colLow"]       | cfg.colLow;
  cfg.colMid       = doc["colMid"]       | cfg.colMid;
  cfg.colHigh      = doc["colHigh"]      | cfg.colHigh;
  cfg.thrMidW      = doc["thrMidW"]      | cfg.thrMidW;
  cfg.thrHighW     = doc["thrHighW"]     | cfg.thrHighW;
  cfg.alarmW       = doc["alarmW"]       | cfg.alarmW;
  cfg.buzzerOn     = (bool)(doc["buzzerOn"] | (cfg.buzzerOn ? 1 : 0));
  cfg.alarmRepeatS = doc["alarmRepeatS"] | cfg.alarmRepeatS;
  cfg.brightness   = doc["brightness"]   | cfg.brightness;
  cfg.fetchS       = doc["fetchS"]       | cfg.fetchS;
  cfg.rotation     = doc["rotation"]     | cfg.rotation;

  Serial.println(F("[FS] config caricata"));
  return true;
}

void applyConfig() {
  if (cfg.brightness < 5)   cfg.brightness = 5;
  if (cfg.brightness > 100) cfg.brightness = 100;
  if (cfg.fetchS < 1)       cfg.fetchS = 1;
  if (cfg.alarmRepeatS < 3) cfg.alarmRepeatS = 3;

  if (cfg.rotation != 0 && cfg.rotation != 2) cfg.rotation = 0;

  matrix.setRotation(cfg.rotation);
  matrix.setBrightness(cfg.brightness);
  matrix.fillScreen(0);
  matrix.show();
  forceRedraw = true;
}

// =========================
// HOME ASSISTANT FETCH
// =========================
bool fetchPowerFromHA(float& wattsOut) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(cfg.haToken) < 20)      return false;

  WiFiClient client;
  HTTPClient http;

  String url = String("http://") + cfg.haHost + ":" + String(cfg.haPort) +
               "/api/states/" + cfg.haEntity;

  if (!http.begin(client, url)) {
    Serial.println(F("[HA] http.begin fallito"));
    return false;
  }

  http.setTimeout(4000);
  http.addHeader("Authorization", String("Bearer ") + cfg.haToken);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("[HA] HTTP code: ")); Serial.println(httpCode);
    http.end();
    return false;
  }

  // filtro: prendo solo i due campi utili, risparmio RAM
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument filter;
  JsonDocument doc;
#else
  StaticJsonDocument<128>  filter;
  DynamicJsonDocument      doc(512);
#endif
  filter["state"] = true;
  filter["attributes"]["unit_of_measurement"] = true;

  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) { Serial.print(F("[HA] JSON err: ")); Serial.println(err.c_str()); return false; }

  String state = doc["state"] | "";
  String unit  = doc["attributes"]["unit_of_measurement"] | "W";

  if (state.length() == 0 || state == "unknown" || state == "unavailable") return false;

  float value = state.toFloat();
  unit.toLowerCase();
  if (unit == "kw") value *= 1000.0;

  wattsOut = value;
  return true;
}

// =========================
// WIFI
// =========================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void waitWiFi(unsigned long timeoutMs) {
  drawCenteredText("WiFi", colorBlue(), true);
  Serial.print(F("Connessione WiFi"));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
    yield();
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP: ")); Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi NON connesso"));
  }
}

// =========================
// PAGINA WEB
// =========================
const char PAGE_INDEX[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html><html lang="it"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pannello kW</title>
<style>
:root{--bg:#12151a;--card:#1b1f27;--bd:#2a3038;--tx:#e8eaed;--mu:#9aa3af;--ac:#4da3ff}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--tx);
font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:680px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px}
.sub{color:var(--mu);font-size:13px;margin-bottom:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:16px;margin-bottom:14px}
.card h2{font-size:14px;text-transform:uppercase;letter-spacing:.06em;color:var(--mu);margin:0 0 12px}
.row{display:flex;gap:12px;flex-wrap:wrap}
.f{flex:1 1 180px;margin-bottom:12px}
label{display:block;font-size:12px;color:var(--mu);margin-bottom:4px}
input[type=text],input[type=number],input[type=password],select{width:100%;padding:8px 10px;border-radius:7px;
border:1px solid var(--bd);background:#0e1116;color:var(--tx);font-size:14px}
input[type=color]{width:100%;height:38px;padding:2px;border-radius:7px;border:1px solid var(--bd);background:#0e1116}
input[type=range]{width:100%}
.chk{display:flex;align-items:center;gap:8px;margin-top:20px}
button{padding:10px 16px;border-radius:8px;border:0;background:var(--ac);color:#06121f;
font-weight:600;font-size:14px;cursor:pointer}
button.sec{background:#2a3038;color:var(--tx)}
.btns{display:flex;gap:10px;flex-wrap:wrap;margin-top:6px}
.live{display:flex;gap:18px;flex-wrap:wrap;align-items:baseline}
.big{font-size:34px;font-weight:700}
.tag{font-size:12px;color:var(--mu)}
#msg{padding:10px;border-radius:8px;margin-bottom:12px;display:none;font-size:14px}
.ok{background:#12351f;color:#7ee2a8}.er{background:#3a1717;color:#ff9a9a}
a{color:var(--ac)}
</style></head><body><div class="wrap">
<h1>Pannello kW</h1>
<div class="sub">Configurazione display potenza - Home Assistant</div>
<div id="msg"></div>

<div class="card"><h2>Stato</h2>
<div class="live">
  <div><div class="big" id="w">--</div><div class="tag">W</div></div>
  <div><div id="kw" class="tag">-- kW</div><div id="net" class="tag">--</div>
  <div id="up" class="tag">--</div></div>
</div></div>

<form id="cfg">
<div class="card"><h2>Home Assistant</h2>
<div class="row">
  <div class="f" style="flex:2 1 260px"><label>Host / IP</label><input type="text" name="haHost"></div>
  <div class="f" style="flex:0 1 110px"><label>Porta</label><input type="number" name="haPort" min="1" max="65535"></div>
</div>
<div class="f"><label>Entity ID</label><input type="text" name="haEntity"></div>
<div class="f"><label>Long-Lived Access Token (vuoto = non modificare)</label>
<input type="password" name="haToken" placeholder="token non mostrato" autocomplete="new-password"></div>
</div>

<div class="card"><h2>Colori e soglie</h2>
<div class="row">
  <div class="f"><label>Colore basso</label><input type="color" name="colLow"></div>
  <div class="f"><label>Colore medio</label><input type="color" name="colMid"></div>
  <div class="f"><label>Colore alto</label><input type="color" name="colHigh"></div>
</div>
<div class="row">
  <div class="f"><label>Soglia colore medio (W)</label><input type="number" name="thrMidW" min="0" step="50"></div>
  <div class="f"><label>Soglia colore alto (W)</label><input type="number" name="thrHighW" min="0" step="50"></div>
</div>
<div class="f" style="flex:0 1 200px"><label>Orientamento display</label>
  <select name="rotation"><option value="0">Normale</option><option value="2">Ruotato 180&deg;</option></select></div>
<div class="f"><label>Luminosita': <span id="brv"></span></label>
<input type="range" name="brightness" min="5" max="100" step="1" oninput="brv.textContent=this.value"></div>
</div>

<div class="card"><h2>Allarme</h2>
<div class="row">
  <div class="f"><label>Soglia allarme (W)</label><input type="number" name="alarmW" min="0" step="50"></div>
  <div class="f"><label>Ripetizione (s)</label><input type="number" name="alarmRepeatS" min="3" max="3600"></div>
  <div class="f"><label>Lettura HA (s)</label><input type="number" name="fetchS" min="1" max="600"></div>
  <div class="f chk"><input type="checkbox" name="buzzerOn" id="bz"><label for="bz" style="margin:0">Buzzer attivo</label></div>
</div>
</div>

<div class="btns">
  <button type="submit">Salva</button>
  <button type="button" class="sec" onclick="post('/api/test','Test buzzer inviato')">Test buzzer</button>
  <button type="button" class="sec" onclick="if(confirm('Riavviare?'))post('/api/restart','Riavvio in corso...')">Riavvia</button>
  <a class="sec" href="/update" style="padding:10px 16px;border-radius:8px;background:#2a3038;text-decoration:none">Aggiornamento OTA</a>
</div>
</form>
</div>
<script>
const $=s=>document.querySelector(s), f=document.getElementById('cfg');
function msg(t,ok){const m=$('#msg');m.textContent=t;m.className=ok?'ok':'er';m.style.display='block';
setTimeout(()=>m.style.display='none',3500);}
function load(){fetch('/api/config').then(r=>r.json()).then(c=>{
  for(const k in c){const el=f.elements[k];if(!el)continue;
    if(el.type==='checkbox')el.checked=!!c[k];else el.value=c[k];}
  f.elements['haToken'].value='';
  $('#brv').textContent=f.elements['brightness'].value;});}
function status(){fetch('/api/status').then(r=>r.json()).then(s=>{
  $('#w').textContent=(s.valid?s.watts.toFixed(0):'--')+(s.over?'  !':'');
  $('#kw').textContent=(s.valid?(s.watts/1000).toFixed(2):'--')+' kW'+(s.over?'  ALLARME':'');
  $('#net').textContent='IP '+s.ip+'  RSSI '+s.rssi+' dBm';
  $('#up').textContent='Uptime '+s.uptime+'s  Heap '+s.heap+' B';}).catch(()=>{});}
f.addEventListener('submit',e=>{e.preventDefault();
  const o={};for(const el of f.elements){if(!el.name)continue;
    o[el.name]=el.type==='checkbox'?el.checked:el.value;}
  if(!o.haToken)delete o.haToken;
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})
  .then(r=>r.ok?msg('Configurazione salvata',1):msg('Errore nel salvataggio',0));});
function post(u,t){fetch(u,{method:'POST'}).then(()=>msg(t,1));}
load();status();setInterval(status,2000);
</script></body></html>
)HTMLPAGE";

// =========================
// HANDLER HTTP
// =========================
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX);
}

void handleGetConfig() {
  String o;
  o.reserve(420);
  o  = "{\"haHost\":\"";      o += cfg.haHost;
  o += "\",\"haPort\":";      o += String(cfg.haPort);
  o += ",\"haEntity\":\"";    o += cfg.haEntity;
  o += "\",\"colLow\":\"";    o += hex6(cfg.colLow);
  o += "\",\"colMid\":\"";    o += hex6(cfg.colMid);
  o += "\",\"colHigh\":\"";   o += hex6(cfg.colHigh);
  o += "\",\"thrMidW\":";     o += String(cfg.thrMidW, 0);
  o += ",\"thrHighW\":";      o += String(cfg.thrHighW, 0);
  o += ",\"alarmW\":";        o += String(cfg.alarmW, 0);
  o += ",\"buzzerOn\":";      o += (cfg.buzzerOn ? "true" : "false");
  o += ",\"alarmRepeatS\":";  o += String(cfg.alarmRepeatS);
  o += ",\"brightness\":";    o += String(cfg.brightness);
  o += ",\"fetchS\":";        o += String(cfg.fetchS);
  o += ",\"rotation\":";      o += String(cfg.rotation);
  o += "}";
  server.send(200, "application/json", o);
}

void handlePostConfig() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }

#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonDocument doc;
#else
  DynamicJsonDocument doc(1024);
#endif
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "json error");
    return;
  }

  if (doc["haHost"].is<const char*>())   strlcpy(cfg.haHost,   doc["haHost"],   sizeof(cfg.haHost));
  if (doc["haEntity"].is<const char*>()) strlcpy(cfg.haEntity, doc["haEntity"], sizeof(cfg.haEntity));
  if (doc["haToken"].is<const char*>()) {
    String t = doc["haToken"].as<String>();
    t.trim();
    if (t.length() > 0) strlcpy(cfg.haToken, t.c_str(), sizeof(cfg.haToken));
  }

  if (!doc["haPort"].isNull())       cfg.haPort       = doc["haPort"].as<uint16_t>();
  if (!doc["colLow"].isNull())       cfg.colLow       = parseHex6(doc["colLow"].as<String>());
  if (!doc["colMid"].isNull())       cfg.colMid       = parseHex6(doc["colMid"].as<String>());
  if (!doc["colHigh"].isNull())      cfg.colHigh      = parseHex6(doc["colHigh"].as<String>());
  if (!doc["thrMidW"].isNull())      cfg.thrMidW      = doc["thrMidW"].as<float>();
  if (!doc["thrHighW"].isNull())     cfg.thrHighW     = doc["thrHighW"].as<float>();
  if (!doc["alarmW"].isNull())       cfg.alarmW       = doc["alarmW"].as<float>();
  if (!doc["buzzerOn"].isNull())     cfg.buzzerOn     = doc["buzzerOn"].as<bool>();
  if (!doc["alarmRepeatS"].isNull()) cfg.alarmRepeatS = doc["alarmRepeatS"].as<uint16_t>();
  if (!doc["brightness"].isNull())   cfg.brightness   = doc["brightness"].as<uint8_t>();
  if (!doc["fetchS"].isNull())       cfg.fetchS       = doc["fetchS"].as<uint16_t>();
  if (!doc["rotation"].isNull())     cfg.rotation     = doc["rotation"].as<uint8_t>();

  applyConfig();
  saveConfig();

  if (hasValidValue) showPower(currentWatts);
  lastFetchMs = 0;   // rilettura immediata

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  String o;
  o.reserve(200);
  o  = "{\"watts\":";     o += String(currentWatts, 1);
  o += ",\"valid\":";     o += (hasValidValue ? "true" : "false");
  o += ",\"over\":";      o += (overLimit ? "true" : "false");
  o += ",\"ip\":\"";      o += WiFi.localIP().toString();
  o += "\",\"rssi\":";    o += String(WiFi.RSSI());
  o += ",\"uptime\":";    o += String(millis() / 1000);
  o += ",\"heap\":";      o += String(ESP.getFreeHeap());
  o += "}";
  server.send(200, "application/json", o);
}

void handleTest() {
  melodyStart();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRestart() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  ESP.restart();
}

void setupServer() {
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/config",  HTTP_GET,  handleGetConfig);
  server.on("/api/config",  HTTP_POST, handlePostConfig);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/test",    HTTP_POST, handleTest);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });

  httpUpdater.setup(&server, "/update", UPDATE_USER, UPDATE_PASS);
  server.begin();
  Serial.println(F("[WEB] server avviato su porta 80"));
}

void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    melodyStop();
    drawCenteredText("OTA", colorBlue(), true);
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    static uint8_t last = 255;
    uint8_t pct = (t > 0) ? (uint8_t)(p * 100UL / t) : 0;
    if (pct != last) {
      last = pct;
      drawCenteredText(String(pct) + "%", colorWhite(), true);
    }
  });
  ArduinoOTA.onEnd([]() { drawCenteredText("OK", rgb565(cfg.colLow), true); });
  ArduinoOTA.onError([](ota_error_t e) { drawCenteredText("ERR", colorErr(), true); });

  ArduinoOTA.begin();
  Serial.println(F("[OTA] pronto"));
}

// =========================
// SETUP
// =========================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n\n=== Pannello_Kw v2.0 ==="));

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  setDefaults();

  if (!LittleFS.begin()) {
    Serial.println(F("[FS] mount fallito, formatto..."));
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();

  matrix.begin();
  matrix.setRotation(cfg.rotation);
  matrix.setBrightness(cfg.brightness);
  matrix.fillScreen(0);
  matrix.show();

  drawCenteredText("BOOT", colorWhite(), true);

  startWiFi();
  waitWiFi(20000);

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
    }
    setupServer();
    setupOTA();
    scrollText(WiFi.localIP().toString(), colorBlue(), 55);
  } else {
    drawCenteredText("NET!", colorErr(), true);
  }

  applyConfig();

  float watts;
  if (fetchPowerFromHA(watts)) {
    currentWatts  = watts;
    hasValidValue = true;
    showPower(currentWatts);
  } else if (WiFi.status() == WL_CONNECTED) {
    drawCenteredText("HA!", colorErr(), true);
  }
  lastFetchMs = millis();
}

// =========================
// LOOP
// =========================
void loop() {
  unsigned long now = millis();

  melodyTask();

  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    MDNS.update();
    ArduinoOTA.handle();
  } else {
    if (now - lastWifiRetryMs >= 10000UL) {
      lastWifiRetryMs = now;
      Serial.println(F("[WIFI] riconnessione..."));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      if (!hasValidValue) drawCenteredText("NET!", colorErr());
    }
  }

  // lettura periodica da Home Assistant
  if (now - lastFetchMs >= (unsigned long)cfg.fetchS * 1000UL) {
    lastFetchMs = now;

    float watts;
    if (fetchPowerFromHA(watts)) {
      currentWatts  = watts;
      hasValidValue = true;
      showPower(currentWatts);
      handleAlarm(currentWatts);
    } else if (!hasValidValue) {
      // mai letto nulla: mostro errore. Altrimenti lascio l'ultimo valore a schermo.
      drawCenteredText("HA!", colorErr());
    }
  }

  // ridisegno solo se qualcosa e' cambiato (colori/luminosita' da web)
  if (hasValidValue && forceRedraw) showPower(currentWatts);

  yield();
}
