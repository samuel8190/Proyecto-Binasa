/* main.cpp
   BINASAMAN - UniversalTelegramBot integration version
   Mantiene: WebServer, WebSockets, SPIFFS, Google Sheets, sensores, bomba, logs.
   Reemplaza la parte Telegram por UniversalTelegramBot (bot.getUpdates)
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPIFFS.h>

#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// === USER CONFIGURATION ===

// Telegram (token + chat id provistos)
const String TELEGRAM_TOKEN = "8561349984:AAEeukrg0mnGVkTtfDC_Dk143XyuyWsvJSA";
const String TELEGRAM_CHAT_ID = "-1003421846114";
const char* TG_HOST = "api.telegram.org";

// Google Apps Script (tu URL)
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbwXT9fR9JXHdoM63XyWFnNlpLW6bmnp8M9MnJmmoyD-6R82mSADm-V6z4kcspjHQ8Bypg/exec";

// Pins (ajustar según tu placa)
const int SENSOR_PINS[3] = {13, 12, 14}; // flotantes (low, mid, high)
const int FOAM_SENSOR_PIN = 27;          // ADC pin (TCRT5000 AO)
const int PUMP_PIN = 26;                 // MOSFET gate (activo HIGH)

// Umbrales / tiempos
int FOAM_THRESHOLD = 70; // % (0..100) - puede setearse vía /foam/th/<val>
const int WATER_HIGH_LEVEL = 90; // umbral para alerta (no usado en lógica principal)
const unsigned long SHEETS_INTERVAL_MS = 60000UL; // 1 minuto fijo para Sheets
unsigned long telegramReportIntervalMin = 30;

// sampling
const unsigned long SENSOR_SAMPLE_MS = 500;
const unsigned long WEBSOCKET_SEND_MS = 2000;
const int ADC_READ_SAMPLES = 6;

// -------------------------
// === GLOBALS / STATE ===
// -------------------------
WebServer server(80);
WebSocketsServer webSocket(81);

// Telegram objects
WiFiClientSecure secureClient;
UniversalTelegramBot bot(TELEGRAM_TOKEN, secureClient);

bool sensorsState[3] = {false,false,false}; // low, mid, high
int waterLevelPercent = 0; // 0 /25/50/75/100
int foamPercent = 0;
bool pumpState = false;
bool telegramOk = false;

// timing
unsigned long lastSensorMillis = 0;
unsigned long lastWsSendMillis = 0;
unsigned long lastSheetSend = 0;
unsigned long lastTelegramAutoSend = 0;
unsigned long lastTelegramCheck = 0;

// logs (ring buffer)
#define LOG_CAP 120
String logsArr[LOG_CAP];
int logsIdx=0, logsCount=0;
void addLog(const String &s) {
  logsArr[logsIdx] = s;
  logsIdx = (logsIdx + 1) % LOG_CAP;
  if (logsCount < LOG_CAP) logsCount++;
  Serial.println(s);
}
String getLogsJson() {
  String j = "[";
  int start = (logsIdx - logsCount + LOG_CAP) % LOG_CAP;
  for (int i=0;i<logsCount;i++) {
    String v = logsArr[(start + i) % LOG_CAP];
    // escape simple
    v.replace("\\","\\\\");
    v.replace("\"","\\\"");
    j += "\"" + v + "\"";
    if (i < logsCount-1) j += ",";
  }
  j += "]";
  return j;
}

// -------------------------
// === UTIL: urlEncode ===
// -------------------------
String urlEncode(const String &str) {
  String encoded = "";
  char c;
  char buf[4];
  for (size_t i=0;i<str.length();i++){
    c = str[i];
    if ( (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') ||
         c == '-' || c == '_' || c == '.' || c == '~' ) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      sprintf(buf, "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

// -------------------------
// === NETWORK HELPERS ===
// -------------------------
void sendTelegramMessageRaw(const String &text) {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("Telegram send skipped - no WiFi");
    return;
  }
  // Use UniversalTelegramBot to send message
  int res = bot.sendMessage(TELEGRAM_CHAT_ID, text, "Markdown");
  if (res > 0) {
    telegramOk = true;
    addLog("Telegram sent");
  } else {
    telegramOk = false;
    addLog("Telegram failed sendMessage");
  }
}

void sendTelegramMessage(const String &text) {
  // wrapper with small size check
  String t = text;
  if (t.length() > 4000) t = t.substring(0,4000);
  sendTelegramMessageRaw(t);
}

// -------------------------
// === Google Sheets logging ===
// -------------------------
void postToGoogleSheets(const String &eventType, int levelPercent, const String &extra = String()) {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("Sheets skip - no WiFi");
    return;
  }
  HTTPClient http;
  http.begin(GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["type"] = eventType;
  doc["level"] = levelPercent;
  doc["pump"] = pumpState ? "ON":"OFF";
  doc["foam"] = foamPercent;
  doc["extra"] = extra;

  String payload;
  serializeJson(doc, payload);
  int code = http.POST(payload);
  if (code == 200) addLog("Posted to Sheets: " + eventType);
  else addLog("Sheets POST failed: " + String(code));
  http.end();
}

// -------------------------
// === SENSORS / ACTUATORS ===
// -------------------------
int readFoamADC() {
  long sum = 0;
  for (int i=0;i<ADC_READ_SAMPLES;i++) {
    sum += analogRead(FOAM_SENSOR_PIN);
    delay(2);
  }
  int adc = (int)(sum / ADC_READ_SAMPLES);
  // map ADC 0..4095 to 0..100 %
  int pct = map(adc, 0, 4095, 0, 100);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

int computeWaterPercent(bool low, bool mid, bool high) {
  // Desired mapping: 0, 25, 50, 75, 100 (based on combos)
  if (!low && !mid && !high) return 0;
  // exact combos priority
  if (low && !mid && !high) return 25;
  if (low && mid && !high) return 50;
  if (!low && mid && high) return 75;
  if (low && mid && high) return 100;
  // other combos fallback:
  if (!low && !mid && high) return 75;
  if (!low && mid && !high) return 50;
  if (low && !mid && high) return 75;
  // default: count-based fallback
  int cnt = (low?1:0) + (mid?1:0) + (high?1:0);
  if (cnt == 1) return 25;
  if (cnt == 2) return 50;
  if (cnt == 3) return 100;
  return 0;
}

void setPumpState(bool on, const String &reason = "") {
  if (on == pumpState) return;
  pumpState = on;
  digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
  addLog(String("Pump ") + (pumpState ? "ON":"OFF") + " - " + reason);
  postToGoogleSheets(String("pump_activation"), waterLevelPercent, reason);
  String tmsg = pumpState ? "⚙️ *BOMBA ENCENDIDA*":"🛑 *BOMBA APAGADA*";
  tmsg += "\n💧 Nivel: " + String(waterLevelPercent) + "%\n🔦 Foam: " + String(foamPercent) + "%\n📝 " + reason;
  sendTelegramMessage(tmsg);
}

// -------------------------
// === WEB & WS ENDPOINTS ===
// -------------------------
void handleFileServe() {
  String path = server.uri();
  if (path == "/") path = "/index.html";
  if (!SPIFFS.exists(path)) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  File f = SPIFFS.open(path, "r");
  String ct = "text/plain";
  if (path.endsWith(".html")) ct = "text/html";
  else if (path.endsWith(".js")) ct = "application/javascript";
  else if (path.endsWith(".css")) ct = "text/css";
  server.streamFile(f, ct);
  f.close();
}

void handleStatusAPI() {
  StaticJsonDocument<256> doc;
  JsonArray arr = doc.createNestedArray("floats");
  arr.add(sensorsState[0]?1:0);
  arr.add(sensorsState[1]?1:0);
  arr.add(sensorsState[2]?1:0);
  doc["adc"] = foamPercent;
  doc["pump"] = pumpState?1:0;
  doc["foamTh"] = FOAM_THRESHOLD;
  doc["level"] = waterLevelPercent;
  // include logs
  JsonArray logs = doc.createNestedArray("logs");
  int start = (logsIdx - logsCount + LOG_CAP) % LOG_CAP;
  for (int i=0;i<logsCount;i++) logs.add(logsArr[(start+i)%LOG_CAP]);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePumpAPI() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "body required");
    return;
  }
  String body = server.arg("plain");
  // expected JSON: {"action":"on"|"off"|"toggle"}
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "json parse error");
    return;
  }
  String action = doc["action"] | "";
  if (action == "on") setPumpState(true, "Manual via Web");
  else if (action == "off") setPumpState(false, "Manual via Web");
  else if (action == "toggle") setPumpState(!pumpState, "Manual toggle via Web");
  else {
    server.send(400, "text/plain", "invalid action");
    return;
  }
  server.send(200, "text/plain", "OK");
}

void handleFoamThAPI() {
  // GET /foam/th/<value>
  String uri = server.uri(); // e.g. /foam/th/70
  int lastSlash = uri.lastIndexOf('/');
  if (lastSlash > 0) {
    String v = uri.substring(lastSlash+1);
    int val = v.toInt();
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    FOAM_THRESHOLD = val;
    addLog("FOAM_THRESHOLD set to " + String(FOAM_THRESHOLD));
    server.send(200, "text/plain", "OK");
    return;
  }
  server.send(400, "text/plain", "Bad request");
}

void handleWifiReset() {
  // simple: restart - user should use WiFiManager AP to set new creds
  server.send(200, "text/plain", "Rebooting to AP...");
  delay(200);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleFileServe);
  server.on("/index.html", HTTP_GET, handleFileServe);
  server.on("/script.js", HTTP_GET, handleFileServe);
  server.on("/styles.css", HTTP_GET, handleFileServe);
  server.on("/status", HTTP_GET, handleStatusAPI);
  server.on("/pump", HTTP_POST, handlePumpAPI);
  server.on("/foam/th", HTTP_GET, handleFoamThAPI);
  server.on("/wifi/reset", HTTP_GET, handleWifiReset);
  server.begin();
  addLog("Web server started");
}

// -------------------------
// === Telegram: polling & commands (REPLACED) ===
// -------------------------

// New handler using UniversalTelegramBot
void processTelegramText(const String &text, const String &fromId) {
  String cmd = text;
  cmd.toLowerCase();
  addLog("TG cmd from " + fromId + ": " + cmd);

  // /menu
  if (cmd == "/menu" || cmd == "/start" || cmd == "/help") {
    String m = "📋 *BINASAMAN - Menú*\n\n";
    m += "🔎 /status - Estado del sistema\n";
    m += "⚙️ /pump_on - Encender bomba\n";
    m += "🛑 /pump_off - Apagar bomba\n";
    m += "🔁 /pump_toggle - Alternar bomba\n";
    m += "⏱ /set_interval <min> - Intervalo reportes Telegram (min)\n";
    m += "📈 /chart_today - Ver gráfica del día\n";
    m += "📅 /chart_date YYYY-MM-DD - Ver gráfica por fecha\n";
    m += "\nSi ingresás mal un comando, te diré que uses /menu.";
    sendTelegramMessage(m);
    return;
  }

  if (cmd == "/status") {
    String s = "📡 *BINASAMAN - Estado*\n\n";
    s += "💧 Nivel: " + String(waterLevelPercent) + "%\n";
    s += "🔦 Sensor óptico: " + String(foamPercent) + "% (thr " + String(FOAM_THRESHOLD) + "%)\n";
    s += "⚙️ Bomba: " + String(pumpState ? "ENCENDIDA":"APAGADA") + "\n";
    s += "🔢 Sensores: " + String(sensorsState[0]?1:0) + String(sensorsState[1]?1:0) + String(sensorsState[2]?1:0);
    sendTelegramMessage(s);
    return;
  }

  if (cmd == "/pump_on") {
    setPumpState(true, "Cmd Telegram /pump_on");
    return;
  }
  if (cmd == "/pump_off") {
    setPumpState(false, "Cmd Telegram /pump_off");
    return;
  }
  if (cmd == "/pump_toggle") {
    setPumpState(!pumpState, "Cmd Telegram /pump_toggle");
    return;
  }

  if (cmd.startsWith("/set_interval")) {
    // parse number
    int sp = cmd.indexOf(' ');
    if (sp < 0) {
      sendTelegramMessage("Uso: /set_interval <minutos>");
      return;
    }
    String num = cmd.substring(sp+1);
    int m = num.toInt();
    if (m < 0) { sendTelegramMessage("Valor invalido"); return; }
    telegramReportIntervalMin = (unsigned long)m;
    sendTelegramMessage("⏱ Intervalo Telegram ajustado a " + String(m) + " min");
    addLog("Telegram interval set to " + String(m) + " min");
    return;
  }

  // unknown
  sendTelegramMessage("❌ *Comando no reconocido*\nUsá /menu para ver todos los comandos disponibles.");
}

// handle new messages from bot (UniversalTelegramBot)
void handleNewMessages(int numNew) {
  for (int i = 0; i < numNew; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String from_name = bot.messages[i].from_name;
    String text = bot.messages[i].text;

    addLog("TG new msg from " + chat_id + ": " + text);

    // only accept messages from the configured chat (group/chat id)
    if (chat_id == TELEGRAM_CHAT_ID && text.length() > 0) {
      processTelegramText(text, chat_id);
    } else {
      addLog("Message from other chat: " + chat_id);
    }
  }
}

// -------------------------
// === CORE: loop sampling ===
// -------------------------
void sampleSensors() {
  // read floats (INPUT_PULLUP assumed)
  bool s0 = (digitalRead(SENSOR_PINS[0]) == LOW);
  bool s1 = (digitalRead(SENSOR_PINS[1]) == LOW);
  bool s2 = (digitalRead(SENSOR_PINS[2]) == LOW);
  sensorsState[0] = s0; sensorsState[1] = s1; sensorsState[2] = s2;

  // compute water level %
  int newLevel = computeWaterPercent(s0,s1,s2);
  if (newLevel != waterLevelPercent) {
    waterLevelPercent = newLevel;
    addLog("Nivel cambiado: " + String(waterLevelPercent) + "%");
    // send an entry to sheets on level change
    postToGoogleSheets("water_level", waterLevelPercent, "change_event");
  }

  // foam ADC
  int newFoam = readFoamADC();
  if (abs(newFoam - foamPercent) >= 3) {
    foamPercent = newFoam;
  }

  // auto anti-foam logic
  if (foamPercent >= FOAM_THRESHOLD && !pumpState) {
    setPumpState(true, "Auto foam detect");
  } else if (foamPercent < (FOAM_THRESHOLD - 10) && pumpState) {
    setPumpState(false, "Auto foam cleared");
  }
}

void broadcastWS() {
  StaticJsonDocument<256> doc;
  doc["level"] = waterLevelPercent;
  doc["foam"] = foamPercent;
  doc["pump"] = pumpState ? 1 : 0;
  JsonArray arr = doc.createNestedArray("sensors");
  arr.add(sensorsState[0]?1:0);
  arr.add(sensorsState[1]?1:0);
  arr.add(sensorsState[2]?1:0);
  String out; serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

// -------------------------
// === SETUP / LOOP ===
// -------------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(SENSOR_PINS[0], INPUT_PULLUP);
  pinMode(SENSOR_PINS[1], INPUT_PULLUP);
  pinMode(SENSOR_PINS[2], INPUT_PULLUP);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  // WiFi Manager
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("BINASAMAN-Setup")) {
    // failed, restart
    Serial.println("WiFi failed");
    ESP.restart();
  }
  Serial.println("Connected to WiFi: " + WiFi.localIP().toString());

  // setup WS and HTTP
  webSocket.begin();
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length){
    if (type == WStype_TEXT) {
        StaticJsonDocument<128> j;
        DeserializationError e = deserializeJson(j, payload, length);
        if (!e && j.containsKey("type") && String(j["type"].as<const char*>()) == "control") {
            if (j.containsKey("pump")) {
                bool p = (j["pump"].as<int>() != 0);
                setPumpState(p, "WebSocket control");
            }
        }
    }
    else if (type == WStype_CONNECTED) {
        // Si querés poner un log:
        addLog("WS client conectado");
    }
});


  setupWebServer();
  addLog("System started");

  // Secure client for Telegram
  secureClient.setInsecure(); // skip cert validation (common on ESP32)
  // send startup message via UniversalTelegramBot
  sendTelegramMessage("🚀 *BINASAMAN iniciado*\nIP: " + WiFi.localIP().toString());

  lastSensorMillis = millis();
  lastWsSendMillis = millis();
  lastSheetSend = millis();
  lastTelegramAutoSend = millis();
  lastTelegramCheck = millis();
}

void loop() {
  server.handleClient();
  webSocket.loop();

  unsigned long now = millis();

  // sampling sensors
  static unsigned long lastSensor = 0;
  if (now - lastSensor >= SENSOR_SAMPLE_MS) {
    lastSensor = now;
    sampleSensors();
  }

  // websocket broadcast
  static unsigned long lastWs = 0;
  if (now - lastWs >= WEBSOCKET_SEND_MS) {
    lastWs = now;
    broadcastWS();
  }

  // Google Sheets fixed periodic send every 1 minute
  static unsigned long lastSheet = 0;
  if (now - lastSheet >= SHEETS_INTERVAL_MS) {
    lastSheet = now;
    postToGoogleSheets("periodic", waterLevelPercent, "periodic_send");
  }

  // Telegram: check for new messages periodically (every 2s)
  if (now - lastTelegramCheck >= 2000) {
    lastTelegramCheck = now;
    int numNew = bot.getUpdates(bot.last_message_received + 1);
    if (numNew > 0) {
      addLog("Telegram updates: " + String(numNew));
      handleNewMessages(numNew);
    }
  }

  // auto telegram report if interval configured (>0)
  if (telegramReportIntervalMin > 0) {
    unsigned long intervalMs = telegramReportIntervalMin * 60000UL;
    if (now - lastTelegramAutoSend >= intervalMs) {
      lastTelegramAutoSend = now;
      // create and send summary
      String s = "📡 *BINASAMAN - Reporte automático*\n";
      s += "💧 Nivel: " + String(waterLevelPercent) + "%\n";
      s += "🔦 Foam: " + String(foamPercent) + "%\n";
      s += "⚙️ Bomba: " + String(pumpState ? "ENCENDIDA":"APAGADA");
      sendTelegramMessage(s);
    }
  }
}
