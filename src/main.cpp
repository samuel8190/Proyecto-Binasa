#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "time.h"

// === CONFIGURACIÓN ===
const String TELEGRAM_TOKEN = "8561349984:AAEeukrg0mnGVkTtfDC_Dk143XyuyWsvJSA";
const String TELEGRAM_CHAT_ID = "-1003421846114";
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbwXT9fR9JXHdoM63XyWFnNlpLW6bmnp8M9MnJmmoyD-6R82mSADm-V6z4kcspjHQ8Bypg/exec";

// Pines
const int SENSOR_PINS[3] = {13, 12, 14};
const int FOAM_SENSOR_PIN = 27;
const int PUMP_PIN = 26;
const int WATER_LEVEL_LEDS[3] = {12, 14, 27};
const int PUMP_LED_PIN = 26;
const int SHUTDOWN_BUTTON_PIN = 4;
const int TEST_BUTTON_PIN = 5;

// Configuración
int FOAM_THRESHOLD = 70;
const unsigned long SHEETS_INTERVAL_MS = 300000UL;
unsigned long telegramReportIntervalMin = 30;

// NTP Server
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;

// === VARIABLES GLOBALES ===
WebServer server(80);
WebSocketsServer webSocket(81);
WiFiClientSecure secureClient;
UniversalTelegramBot bot(TELEGRAM_TOKEN, secureClient);

// Estados del sistema
bool sensorsState[3] = {false, false, false};
int waterLevelPercent = 0;
int foamPercent = 0;
bool pumpState = false;
bool systemShutdown = false;

// Timing
unsigned long lastSheetSend = 0;
unsigned long lastTelegramAutoSend = 0;
unsigned long lastTelegramCheck = 0;
bool dataChanged = false;

// === FUNCIONES MEJORADAS ===
void addLog(const String &s) {
  Serial.println("📝 " + s);
}

String getDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00:00";
  
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

String getDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01";
  
  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
  return String(buffer);
}

String getTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00:00";
  
  char buffer[9];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}

int readFoamADC() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(FOAM_SENSOR_PIN);
    delay(1);
  }
  return map(sum / 10, 0, 4095, 0, 100);
}

int computeWaterPercent(bool low, bool mid, bool high) {
  // Tabla de verdad optimizada
  const int levels[] = {0, 25, 50, 75, 100, 75, 50, 75};
  int index = (high ? 4 : 0) + (mid ? 2 : 0) + (low ? 1 : 0);
  return levels[index];
}

void setPumpState(bool on, const String &reason = "") {
  if (on == pumpState || systemShutdown) return;
  
  pumpState = on;
  digitalWrite(PUMP_PIN, pumpState);
  addLog("Bomba " + String(pumpState ? "ON" : "OFF") + " - " + reason);
  dataChanged = true;

  if (WiFi.status() == WL_CONNECTED) {
    String msg = pumpState ? "⚙️ *BOMBA ACTIVADA*" : "🛑 *BOMBA APAGADA*";
    msg += "\n💧 Nivel: " + String(waterLevelPercent) + "%";
    msg += "\n⏰ " + getTime();
    bot.sendMessage(TELEGRAM_CHAT_ID, msg, "Markdown");
  }
}

void updateLEDs() {
  if (systemShutdown) {
    for (int i = 0; i < 3; i++) digitalWrite(WATER_LEVEL_LEDS[i], LOW);
    digitalWrite(PUMP_LED_PIN, LOW);
  } else {
    digitalWrite(WATER_LEVEL_LEDS[0], waterLevelPercent >= 25);
    digitalWrite(WATER_LEVEL_LEDS[1], waterLevelPercent >= 50);
    digitalWrite(WATER_LEVEL_LEDS[2], waterLevelPercent >= 75);
    digitalWrite(PUMP_LED_PIN, pumpState);
  }
}

void checkForChanges() {
  static int lastLevel = -1;
  static int lastFoam = -1;
  static bool lastPump = false;
  
  if (waterLevelPercent != lastLevel || foamPercent != lastFoam || pumpState != lastPump) {
    dataChanged = true;
    lastLevel = waterLevelPercent;
    lastFoam = foamPercent;
    lastPump = pumpState;
  }
}

void broadcastWS() {
  StaticJsonDocument<200> doc;
  doc["level"] = waterLevelPercent;
  doc["foam"] = foamPercent;
  doc["pump"] = pumpState;
  doc["shutdown"] = systemShutdown;
  
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < 3; i++) arr.add(sensorsState[i]);
  
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

void sampleSensors() {
  // Leer sensores con debug
  bool s0 = (digitalRead(SENSOR_PINS[0]) == LOW);
  bool s1 = (digitalRead(SENSOR_PINS[1]) == LOW);
  bool s2 = (digitalRead(SENSOR_PINS[2]) == LOW);
  
  Serial.printf("🔍 Sensores: S0=%d, S1=%d, S2=%d -> ", s0, s1, s2);
  
  sensorsState[0] = s0;
  sensorsState[1] = s1;
  sensorsState[2] = s2;

  // Calcular nivel
  int newLevel = computeWaterPercent(s0, s1, s2);
  Serial.printf("Nivel: %d%% - ", newLevel);
  
  if (newLevel != waterLevelPercent) {
    waterLevelPercent = newLevel;
    dataChanged = true;
  }

  // Sensor espuma
  int rawFoam = analogRead(FOAM_SENSOR_PIN);
  int newFoam = readFoamADC();
  Serial.printf("Espuma RAW: %d -> %d%%\n", rawFoam, newFoam);
  
  if (abs(newFoam - foamPercent) >= 3) {
    foamPercent = newFoam;
    dataChanged = true;
  }

  // Control automático de espuma
  if (!systemShutdown) {
    if (foamPercent >= FOAM_THRESHOLD && !pumpState) {
      setPumpState(true, "Detección automática de espuma");
    } else if (foamPercent < (FOAM_THRESHOLD - 15) && pumpState) {
      setPumpState(false, "Espuma controlada");
    }
  }
}

void sendToGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED || systemShutdown) return;
  
  HTTPClient http;
  String payload = "date=" + getDate() + 
                   "&time=" + getTime() +
                   "&water_level=" + String(waterLevelPercent) +
                   "&foam_level=" + String(foamPercent) +
                   "&pump_state=" + String(pumpState);

  http.begin(secureClient, GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = http.POST(payload);
  if (code > 0) {
    Serial.println("✅ Datos enviados a Google Sheets");
  } else {
    Serial.println("❌ Error enviando a Google Sheets");
  }
  http.end();
}

// === SISTEMAS FÍSICOS ===
void handleShutdownButton() {
  static bool lastState = HIGH;
  static unsigned long lastDebounce = 0;
  
  int reading = digitalRead(SHUTDOWN_BUTTON_PIN);
  
  if (reading != lastState) {
    lastDebounce = millis();
  }
  
  if ((millis() - lastDebounce) > 50) {
    if (reading == LOW) {
      systemShutdown = !systemShutdown;
      
      if (systemShutdown) {
        setPumpState(false, "Apagado total del sistema");
        addLog("⚠️ SISTEMA APAGADO");
      } else {
        addLog("✅ SISTEMA REANUDADO");
      }
      dataChanged = true;
    }
  }
  lastState = reading;
}

void handleTestButton() {
  static bool lastState = HIGH;
  static unsigned long lastDebounce = 0;
  
  int reading = digitalRead(TEST_BUTTON_PIN);
  
  if (reading != lastState) {
    lastDebounce = millis();
  }
  
  if ((millis() - lastDebounce) > 50 && reading == LOW) {
    addLog("🧪 Iniciando test del sistema...");
    
    // Test LEDs
    for (int i = 0; i < 3; i++) {
      digitalWrite(WATER_LEVEL_LEDS[i], HIGH);
      delay(150);
    }
    digitalWrite(PUMP_LED_PIN, HIGH);
    delay(300);
    
    // Leer sensores durante test
    sampleSensors();
    
    // Apagar LEDs
    for (int i = 0; i < 3; i++) digitalWrite(WATER_LEVEL_LEDS[i], LOW);
    digitalWrite(PUMP_LED_PIN, LOW);
    
    updateLEDs();
    addLog("🧪 Test completado");
  }
  lastState = reading;
}

// === WEB SERVER MEJORADO ===
void handleStatusAPI() {
  StaticJsonDocument<300> doc;
  doc["level"] = waterLevelPercent;
  doc["foam"] = foamPercent;
  doc["pump"] = pumpState;
  doc["shutdown"] = systemShutdown;
  doc["foamTh"] = FOAM_THRESHOLD;
  doc["timestamp"] = getDateTime();
  
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < 3; i++) arr.add(sensorsState[i]);
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleHistory() {
  if (!server.hasArg("date")) {
    server.send(400, "application/json", "{\"error\":\"Falta fecha\"}");
    return;
  }
  
  String date = server.arg("date");
  addLog("📅 Solicitando historial para: " + date);
  
  // Datos de ejemplo - luego conectar con Google Sheets
  String jsonResponse = "[";
  jsonResponse += "{\"hora\":\"08:00\", \"agua\":25, \"espuma\":10},";
  jsonResponse += "{\"hora\":\"10:00\", \"agua\":50, \"espuma\":25},"; 
  jsonResponse += "{\"hora\":\"12:00\", \"agua\":75, \"espuma\":60},";
  jsonResponse += "{\"hora\":\"14:00\", \"agua\":100, \"espuma\":30},";
  jsonResponse += "{\"hora\":\"16:00\", \"agua\":75, \"espuma\":15},";
  jsonResponse += "{\"hora\":\"18:00\", \"agua\":50, \"espuma\":10},";
  jsonResponse += "{\"hora\":\"" + getTime() + "\", \"agua\":" + String(waterLevelPercent) + ", \"espuma\":" + String(foamPercent) + "}";
  jsonResponse += "]";
  
  server.send(200, "application/json", jsonResponse);
}

void handleFileServe() {
  String path = server.uri();
  if (path == "/") path = "/index.html";
  
  if (SPIFFS.exists(path)) {
    File f = SPIFFS.open(path, "r");
    String ct = "text/plain";
    if (path.endsWith(".css")) ct = "text/css";
    else if (path.endsWith(".js")) ct = "application/javascript";
    else if (path.endsWith(".html")) ct = "text/html";
    
    server.streamFile(f, ct);
    f.close();
  } else {
    server.send(404, "text/plain", "Archivo no encontrado: " + path);
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleFileServe);
  server.on("/index.html", HTTP_GET, handleFileServe);
  server.on("/script.js", HTTP_GET, handleFileServe);
  server.on("/style.css", HTTP_GET, handleFileServe);
  server.on("/status", HTTP_GET, handleStatusAPI);
  server.on("/history", HTTP_GET, handleHistory);
  
  server.on("/control", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
      server.send(400, "application/json", "{\"error\":\"JSON inválido\"}");
      return;
    }
    
    String action = doc["action"];
    String response = "";
    
    if (action == "on" && !systemShutdown) {
      setPumpState(true, "Control Web");
      response = "{\"status\":\"ok\",\"message\":\"Bomba encendida\"}";
    } else if (action == "off") {
      setPumpState(false, "Control Web");
      response = "{\"status\":\"ok\",\"message\":\"Bomba apagada\"}";
    } else {
      response = "{\"error\":\"Acción inválida o sistema apagado\"}";
    }
    
    server.send(200, "application/json", response);
  });
  
  server.begin();
}

// === TELEGRAM BOT MEJORADO ===
void sendCompleteStatusToTelegram(String chat_id = TELEGRAM_CHAT_ID) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  String message = "📡 *Estado Completo del Sistema*\n\n";
  message += "📊 *SENSORES:*\n";
  message += "💧 Nivel Agua: " + String(waterLevelPercent) + "%\n";
  message += "🌿 Nivel Biomasa: " + String(foamPercent) + "%\n\n";
  
  message += "⚡ *ACTUADORES:*\n";
  message += "• Bomba: " + String(pumpState ? "✅ ENCENDIDA" : "❌ APAGADA") + "\n";
  message += "• Sistema: " + String(systemShutdown ? "🔴 APAGADO" : "🟢 ACTIVO") + "\n\n";
  
  message += "⏰ " + getDateTime();
  
  bot.sendMessage(chat_id, message, "Markdown");
}

void processTelegramCommand(const String &text, const String &chat_id) {
  String cmd = text;
  cmd.toLowerCase();

  if (cmd == "/start" || cmd == "/menu") {
    String reply = "📋 *BINASAMAN - Menú de Comandos:*\n\n";
    reply += "📊 /datasensores - Datos actuales + Actuadores\n";
    reply += "⏱️ /setinterval [min] - Cambiar intervalo\n";
    reply += "📈 /status - Estado general\n";
    reply += "🖥️ /infodevices - Info del dispositivo\n";
    reply += "⚙️ /pump_on - Encender bomba manualmente\n";
    reply += "🛑 /pump_off - Apagar bomba manualmente\n";
    reply += "🔧 /test - Test del sistema\n";
    
    bot.sendMessage(chat_id, reply, "Markdown");
    return;
  }

  if (cmd == "/datasensores") {
    sendCompleteStatusToTelegram(chat_id);
    return;
  }

  if (cmd == "/status") {
    String message = "📈 *Estado General:*\n\n";
    message += "⏱️ Intervalo Reportes: " + String(telegramReportIntervalMin) + " min\n";
    message += "📶 WiFi: " + String(WiFi.RSSI()) + " dBm\n";
    message += "📡 IP: " + WiFi.localIP().toString() + "\n";
    message += "🔌 Sistema: " + String(systemShutdown ? "APAGADO 🔴" : "ACTIVO 🟢") + "\n";
    message += "💧 Nivel Agua: " + String(waterLevelPercent) + "%\n";
    message += "🌿 Biomasa: " + String(foamPercent) + "%\n";
    message += "⏰ " + getDateTime();
    
    bot.sendMessage(chat_id, message, "Markdown");
    return;
  }

  if (cmd == "/infodevices") {
    uint64_t chipid = ESP.getEfuseMac();
    String chipIdStr = String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
    
    String message = "🖥️ *Información del Dispositivo*\n\n";
    message += "💻 *Chip ID:* `" + chipIdStr + "`\n";
    message += "📶 *RSSI:* " + String(WiFi.RSSI()) + " dBm\n";
    message += "⏱️ *Uptime:* " + String(millis() / 1000) + "s\n";
    message += "🔁 *Próximo reporte:* " + String(telegramReportIntervalMin) + " min\n";
    message += "📡 *WiFi:* " + WiFi.SSID() + "\n";
    message += "🌍 *IP:* " + WiFi.localIP().toString() + "\n";
    message += "🔢 *MAC:* " + WiFi.macAddress() + "\n";
    
    bot.sendMessage(chat_id, message, "Markdown");
    return;
  }

  if (cmd == "/pump_on" && !systemShutdown) {
    setPumpState(true, "Comando Telegram");
    bot.sendMessage(chat_id, "✅ *Bomba encendida manualmente*", "Markdown");
    return;
  } 

  if (cmd == "/pump_off") {
    setPumpState(false, "Comando Telegram");
    bot.sendMessage(chat_id, "🛑 *Bomba apagada manualmente*", "Markdown");
    return;
  }

  if (cmd == "/test") {
    bot.sendMessage(chat_id, "🧪 *Ejecutando test del sistema...*", "Markdown");
    handleTestButton();
    delay(2000);
    sendCompleteStatusToTelegram(chat_id);
    return;
  }

  if (cmd.startsWith("/setinterval")) {
    int sp = cmd.indexOf(' ');
    if (sp > 0) {
      int newInterval = cmd.substring(sp+1).toInt();
      if (newInterval > 0 && newInterval <= 1440) {
        telegramReportIntervalMin = newInterval;
        bot.sendMessage(chat_id, "⏱️ *Intervalo cambiado a " + String(newInterval) + " minutos*", "Markdown");
      } else {
        bot.sendMessage(chat_id, "⚠️ *Intervalo inválido (1-1440 min)*", "Markdown");
      }
    } else {
      bot.sendMessage(chat_id, "⚠️ *Uso: /setinterval [minutos]*", "Markdown");
    }
    return;
  }

  bot.sendMessage(chat_id, "❓ *Comando no reconocido*\nUsa /menu para ver las opciones", "Markdown");
}

// === SETUP OPTIMIZADO ===
void setup() {
  Serial.begin(115200);
  Serial.println("\n🚀 Iniciando BINASAMAN...");
  
  // Configurar pines
  for (int i = 0; i < 3; i++) {
    pinMode(SENSOR_PINS[i], INPUT_PULLUP);
    pinMode(WATER_LEVEL_LEDS[i], OUTPUT);
    digitalWrite(WATER_LEVEL_LEDS[i], LOW);
  }
  
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  pinMode(SHUTDOWN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ Error al montar SPIFFS");
  }

  // WiFi
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  
  if (!wm.autoConnect("BINASAMAN-Setup")) {
    Serial.println("❌ Fallo de conexión, reiniciando...");
    ESP.restart();
  }

  Serial.println("✅ WiFi conectado: " + WiFi.localIP().toString());

  // Configurar tiempo
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Servidores
  webSocket.begin();
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_TEXT) {
      DynamicJsonDocument doc(128);
      if (!deserializeJson(doc, payload) && doc["type"] == "control") {
        if (doc.containsKey("pump")) {
          setPumpState(doc["pump"], "WebSocket");
        }
      }
    }
  });
  
  setupWebServer();
  secureClient.setInsecure();

  // Mensaje de inicio
  String startMsg = "⚙️ *Sistema Iniciado: Monitoreo de DHT22*\n\n";
  startMsg += "🔧 Motivo: ⚙ Power On\n";
  startMsg += "🌍 IP: " + WiFi.localIP().toString() + "\n";
  startMsg += "📶 WiFi: " + WiFi.SSID() + "\n";
  startMsg += "🔢 MAC: " + WiFi.macAddress();
  
  bot.sendMessage(TELEGRAM_CHAT_ID, startMsg, "Markdown");
  addLog("Sistema iniciado correctamente");
}

// === LOOP OPTIMIZADO ===
void loop() {
  server.handleClient();
  webSocket.loop();

  // Botones físicos
  handleShutdownButton();
  handleTestButton();

  unsigned long now = millis();

  // Sensores (cada 1 segundo)
  static unsigned long lastSensor = 0;
  if (!systemShutdown && (now - lastSensor >= 1000)) {
    lastSensor = now;
    sampleSensors();
    checkForChanges();
  }

  // WebSocket (solo cuando hay cambios)
  if (dataChanged) {
    broadcastWS();
    dataChanged = false;
  }

  // LEDs (cada 100ms)
  static unsigned long lastLED = 0;
  if (now - lastLED >= 100) {
    lastLED = now;
    updateLEDs();
  }

  // Google Sheets (cada 5 minutos)
  if (!systemShutdown && (now - lastSheetSend >= SHEETS_INTERVAL_MS)) {
    lastSheetSend = now;
    sendToGoogleSheets();
  }

  // Telegram mensajes (cada 3 segundos)
  if (now - lastTelegramCheck >= 3000) {
    lastTelegramCheck = now;
    int numNew = bot.getUpdates(bot.last_message_received + 1);
    for (int i = 0; i < numNew; i++) {
      if (String(bot.messages[i].chat_id) == TELEGRAM_CHAT_ID) {
        processTelegramCommand(bot.messages[i].text, bot.messages[i].chat_id);
      }
    }
  }

  // Reporte automático Telegram
  if (!systemShutdown && telegramReportIntervalMin > 0) {
    if (now - lastTelegramAutoSend >= telegramReportIntervalMin * 60000UL) {
      lastTelegramAutoSend = now;
      String report = "📡 *REPORTE AUTOMÁTICO*\n\n";
      report += "💧 Nivel Agua: " + String(waterLevelPercent) + "%\n";
      report += "🌿 Nivel Biomasa: " + String(foamPercent) + "%\n";
      report += "⚙️ Bomba: " + String(pumpState ? "ENCENDIDA" : "APAGADA") + "\n";
      report += "⏰ " + getDateTime();
      bot.sendMessage(TELEGRAM_CHAT_ID, report, "Markdown");
    }
  }

  delay(10);
}