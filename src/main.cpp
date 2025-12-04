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
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LiquidCrystal_I2C.h>

// === CONFIGURACIÓN ===
const String TELEGRAM_TOKEN = "8561349984:AAEeukrg0mnGVkTtfDC_Dk143XyuyWsvJSA";
const String TELEGRAM_CHAT_ID = "-1003421846114";
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzNBfhyNIXLme3hGhUazuk5Vi4YSHwf7DX8OLePAfcgTVfZIlNl4GZIvlmRvXq2QcuIiQ/exec";

// === CONFIGURACIÓN DE PINES ===
const int SENSOR_PINS[3] = {32, 14, 27};        // Sensores de agua
const int FOAM_SENSOR_PIN = 5;                  // TCR5000 en GPIO5 (DIGITAL) - CORREGIDO
const int PUMP_PIN = 17;                        // Bomba/MOSFET en GPIO25 (LOW = ACTIVADO para relé LOW-Trigger)
const int WATER_LEVEL_LEDS[3] = {16, 26, 33};   // LEDs nivel agua  
const int PUMP_LED_PIN = 15;                    // LED bomba
const int SHUTDOWN_BUTTON_PIN = 4;              // Botón apagado (GPIO4 para evitar conflicto)
const int TEST_BUTTON_PIN = 18;                 // Botón test

// Configuración OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Configuración LCD I2C
LiquidCrystal_I2C lcd(0x27, 16, 2);

// === PARÁMETROS DEL SISTEMA ===
int FOAM_THRESHOLD = 1;  // Sensor digital: 0 = no espuma, 1 = espuma
const unsigned long SHEETS_INTERVAL_MS = 60000UL;
unsigned long telegramReportIntervalMin = 30;

// NTP Server
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;

unsigned long lastChangeTime = 0;
const unsigned long debounceMs = 60;

// === VARIABLES GLOBALES ===
WebServer server(80);
WebSocketsServer webSocket(81);
WiFiClientSecure secureClient;
UniversalTelegramBot bot(TELEGRAM_TOKEN, secureClient);

// Estados del sistema
bool sensorsState[3] = {false, false, false};
bool sensorsConnected[3] = {false, false, false};
bool pumpConnected = false;
int waterLevelPercent = 0;
int foamValue = 0;  // Valor digital del sensor (0 o 1)
int foamPercent = 0;  // Porcentaje para mostrar
bool pumpState = false;
bool systemShutdown = false;
bool manualPumpControl = false;

// Timing
unsigned long lastSheetSend = 0;
unsigned long lastTelegramAutoSend = 0;
unsigned long lastTelegramCheck = 0;
unsigned long lastConnectionCheck = 0;
bool dataChanged = false;

// === FUNCIONES LCD I2C ===
void initLCD() {
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...    ");
  delay(2000);  // Mostrar "Iniciando" por 2 segundos
}

void updateLCDNormal() {
  lcd.clear();
  
  // Línea 1: Nivel de agua
  lcd.setCursor(0, 0);
  lcd.print("Agua: ");
  lcd.print(waterLevelPercent);
  lcd.print("%   ");
  
  // Línea 2: Estado de vinaza
  lcd.setCursor(0, 1);
  lcd.print("Vinaza: ");
  lcd.print(foamPercent);
  lcd.print("%   ");
  
  // Si está en modo manual, mostrar indicador
  if (manualPumpControl) {
    lcd.setCursor(13, 1);
    lcd.print("M");
  }
  
  // Si está apagado, mostrar en la segunda línea
  if (systemShutdown) {
    lcd.setCursor(0, 1);
    lcd.print("SIST. APAGADO  ");
  }
}

// === FUNCIONES OLED ===
void initOLED() {
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Error OLED");
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("Binasa");
  display.println("Sistema");
  display.display();
  delay(1000);
}

void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  if (systemShutdown) {
    display.setTextSize(2);
    display.setCursor(10, 20);
    display.println("SISTEMA");
    display.setCursor(30, 40);
    display.println("APAGADO");
  } else {
    display.println("Agua: " + String(waterLevelPercent) + "%");
    display.println("Vinaza: " + String(foamPercent) + "%");
    display.println("Bomba: " + String(pumpState ? "ON" : "OFF"));
    display.println("Modo: " + String(manualPumpControl ? "MAN" : "AUTO"));
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[9];
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
      display.println(timeStr);
    }
  }
  display.display();
}

// === FUNCIONES UTILITARIAS ===
void addLog(const String &s) {
  Serial.println("LOG: " + s);
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

// === DETECCIÓN DE CONEXIONES ===
void checkSensorsConnection() {
  for (int i = 0; i < 3; i++) {
    bool connected = (digitalRead(SENSOR_PINS[i]) == LOW);
    if (connected != sensorsConnected[i]) {
      sensorsConnected[i] = connected;
      dataChanged = true;
    }
  }
}

void checkPumpConnection() {
  static bool lastPumpState = pumpState;
  static unsigned long pumpCheckTime = 0;
  static bool testingPump = false;
  
  if (pumpState != lastPumpState) {
    pumpCheckTime = millis() + 500;
    testingPump = true;
    lastPumpState = pumpState;
  }
  
  if (testingPump && millis() >= pumpCheckTime) {
    // Para relé LOW-Trigger: HIGH = apagado, LOW = encendido
    bool actualState = (digitalRead(PUMP_PIN) == LOW);  // LOW significa bomba ON
    pumpConnected = (actualState == pumpState);
    testingPump = false;
    dataChanged = true;
  }
}

void checkAllConnections() {
  checkSensorsConnection();
  checkPumpConnection();
}

// === SENSORES ===
int computeWaterPercent(bool low, bool mid, bool high) {
  if (high) return 100;
  if (mid) return 50;
  if (low) return 25;
  return 0;
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
  StaticJsonDocument<300> doc;
  doc["level"] = waterLevelPercent;
  doc["foam"] = foamPercent;
  doc["pump"] = pumpState;
  doc["shutdown"] = systemShutdown;
  doc["manualMode"] = manualPumpControl;
  doc["pumpConnected"] = pumpConnected;
  
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < 3; i++) arr.add(sensorsState[i]);
  
  JsonArray conn = doc.createNestedArray("sensorsConnected");
  for (int i = 0; i < 3; i++) conn.add(sensorsConnected[i]);
  
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

// === SENSORES ===
void setPumpState(bool on, const String &reason = "") {
  if (on == pumpState || systemShutdown) return;
  
  pumpState = on;
  
  if (reason == "Control Web" || reason == "Comando Telegram" || reason == "WebSocket") {
    manualPumpControl = true;
  } else if (reason.indexOf("automática") >= 0 || reason.indexOf("TCR5000") >= 0) {
    manualPumpControl = false;
  }
  
  // Para relé LOW-Trigger: LOW = activado, HIGH = desactivado
  digitalWrite(PUMP_PIN, pumpState ? LOW : HIGH);
  digitalWrite(PUMP_LED_PIN, pumpState);
  
  addLog("Bomba " + String(pumpState ? "ON" : "OFF") + " - " + reason);
  dataChanged = true;
}

void sampleSensors() {
  static unsigned long lastDebounceTime = 0;
  static bool lastSensorReading = HIGH;
  static bool stableSensorState = HIGH;
  
  // 1. Sensores de agua (SOLO para mostrar nivel)
  bool s0 = (digitalRead(SENSOR_PINS[0]) == LOW);
  bool s1 = (digitalRead(SENSOR_PINS[1]) == LOW);
  bool s2 = (digitalRead(SENSOR_PINS[2]) == LOW);
  
  sensorsState[0] = s0;
  sensorsState[1] = s1;
  sensorsState[2] = s2;

  int newLevel = computeWaterPercent(s0, s1, s2);
  if (newLevel != waterLevelPercent) {
    waterLevelPercent = newLevel;
    dataChanged = true;
  }

  // 2. Sensor TCR5000 - ¡ESTA ES LA PARTE IMPORTANTE!
  int raw = digitalRead(FOAM_SENSOR_PIN);
  
  // DEBUG: Ver valor crudo del sensor
  static int lastRawDebug = -1;
  if (raw != lastRawDebug) {
    Serial.printf("[TCR5000 RAW] Valor: %d (LOW=detecta, HIGH=no detecta)\n", raw);
    lastRawDebug = raw;
  }
  
  // Debounce
  if (raw != lastSensorReading) {
    lastDebounceTime = millis();
    lastSensorReading = raw;
  }
  
  if ((millis() - lastDebounceTime) > debounceMs) {
    if (raw != stableSensorState) {
      stableSensorState = raw;
      
      // Actualizar variables para visualización
      foamValue = stableSensorState;
      // LOW = sensor detecta superficie → 100%
      // HIGH = sensor NO detecta → 0%
      foamPercent = (foamValue == LOW) ? 100 : 0;
      
      // ¡¡¡CONTROL AUTOMÁTICO DE LA BOMBA!!!
      if (!systemShutdown && !manualPumpControl) {
        // LÓGICA CORREGIDA:
        // Cuando el sensor DETECTA (LOW) → Bomba ON
        // Cuando el sensor NO DETECTA (HIGH) → Bomba OFF
        
        if (stableSensorState == LOW) {
          // SENSOR DETECTA SUPERFICIE
          if (!pumpState) {
            Serial.println("[TCR5000] ¡DETECTADO! Activando bomba...");
            setPumpState(true, "Detección TCR5000");
          }
        } else {
          // SENSOR NO DETECTA
          if (pumpState) {
            Serial.println("[TCR5000] NO detectado. Apagando bomba...");
            setPumpState(false, "Sin detección TCR5000");
          }
        }
      }
      
      dataChanged = true;
      addLog("Sensor TCR5000: " + String(stableSensorState == LOW ? "DETECTA" : "NO DETECTA"));
    }
  }
}

// === GOOGLE SHEETS ===
void sendToGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED || systemShutdown) return;

  HTTPClient http;

  String url = GOOGLE_SCRIPT_URL;
  url += "?date=" + getDate();
  url += "&time=" + getTime();
  url += "&water=" + String(waterLevelPercent);
  url += "&foam=" + String(foamPercent);
  url += "&pump=" + String(pumpState ? "1" : "0");

  Serial.println("[Sheets] Enviando a: " + url);

  http.setTimeout(10000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 

  // Usar secureClient para HTTPS
  if (!http.begin(secureClient, url)) {
    Serial.println("[Sheets] http.begin fallo");
    return;
  }

  // GET
  int httpCode = http.GET();

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[Sheets] HTTP Code: %d\n", httpCode);
    Serial.print("[Sheets] Respuesta (inicio): ");
    Serial.println(response.substring(0, min(200, (int)response.length())));
    if (httpCode == 200) {
      addLog("Sheets: Datos enviados OK");
    } else {
      addLog("Sheets Error: " + String(httpCode));
    }
  } else {
    Serial.printf("[Sheets] Error HTTP: %d - %s\n", httpCode, http.errorToString(httpCode).c_str());
    addLog("Sheets: Error de conexión");
  }

  http.end();
}

// === BOTONES FÍSICOS ===
void handleShutdownButton() {
  static bool lastState = HIGH;
  static unsigned long lastDebounce = 0;
  
  int reading = digitalRead(SHUTDOWN_BUTTON_PIN);
  
  if (reading != lastState) {
    lastDebounce = millis();
  }
  
  if ((millis() - lastDebounce) > 500 && reading == LOW) {
    systemShutdown = !systemShutdown;
    
    if (systemShutdown) {
      setPumpState(false, "Apagado total");
      addLog("SISTEMA APAGADO");
    } else {
      addLog("SISTEMA REANUDADO");
    }
    dataChanged = true;
    
    delay(300);
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
  
  if ((millis() - lastDebounce) > 500 && reading == LOW) {
    addLog("Test iniciado");
    
    // Test LEDs
    for (int i = 0; i < 3; i++) {
      digitalWrite(WATER_LEVEL_LEDS[i], HIGH);
      delay(100);
      digitalWrite(WATER_LEVEL_LEDS[i], LOW);
    }
    
    digitalWrite(PUMP_LED_PIN, HIGH);
    delay(200);
    digitalWrite(PUMP_LED_PIN, LOW);
    
    addLog("Test completo");
    
    delay(300);
  }
  
  lastState = reading;
}

// === WEB SERVER ===
void handleStatusAPI() {
  StaticJsonDocument<300> doc;
  doc["level"] = waterLevelPercent;
  doc["foam"] = foamPercent;
  doc["pump"] = pumpState;
  doc["shutdown"] = systemShutdown;
  doc["manualMode"] = manualPumpControl;
  doc["pumpConnected"] = pumpConnected;
  doc["timestamp"] = getDateTime();
  
  JsonArray arr = doc.createNestedArray("sensors");
  for (int i = 0; i < 3; i++) arr.add(sensorsState[i]);
  
  JsonArray conn = doc.createNestedArray("sensorsConnected");
  for (int i = 0; i < 3; i++) conn.add(sensorsConnected[i]);
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
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
    server.send(404, "text/plain", "Archivo no encontrado");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleFileServe);
  server.on("/index.html", HTTP_GET, handleFileServe);
  server.on("/script.js", HTTP_GET, handleFileServe);
  server.on("/style.css", HTTP_GET, handleFileServe);
  server.on("/status", HTTP_GET, handleStatusAPI);
  
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
      response = "{\"status\":\"ok\",\"message\":\"Bomba ON - Manual\",\"manualMode\":true}";
    } else if (action == "off") {
      setPumpState(false, "Control Web");
      response = "{\"status\":\"ok\",\"message\":\"Bomba OFF - Manual\",\"manualMode\":true}";
    } else if (action == "auto") {
      manualPumpControl = false;
      response = "{\"status\":\"ok\",\"message\":\"Modo Auto\",\"manualMode\":false}";
    } else {
      response = "{\"error\":\"Acción inválida\"}";
    }
    
    server.send(200, "application/json", response);
  });
  
  server.begin();
}

// === TELEGRAM BOT ===
void sendCompleteStatusToTelegram(String chat_id = TELEGRAM_CHAT_ID) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  String message = "Estado del Sistema\n\n";
  message += "Agua: " + String(waterLevelPercent) + "%\n";
  message += "Vinaza: " + String(foamPercent) + "%\n";
  message += "Bomba: " + String(pumpState ? "ON" : "OFF") + "\n";
  message += "Modo: " + String(manualPumpControl ? "MANUAL" : "AUTO") + "\n";
  message += getDateTime();
  
  bot.sendMessage(chat_id, message, "");
}

void processTelegramCommand(const String &text, const String &chat_id) {
  String cmd = text;
  cmd.toLowerCase();

  if (cmd == "/start" || cmd == "/menu") {
    String reply = "BINASAMAN - Comandos:\n\n";
    reply += "/datasensores - Datos actuales\n";
    reply += "/setinterval [min] - Intervalo reportes\n";
    reply += "/status - Estado general\n";
    reply += "/infodevices - Info dispositivo\n";
    reply += "/pump_on - Bomba ON manual\n";
    reply += "/pump_off - Bomba OFF manual\n";
    reply += "/auto_mode - Modo automático\n";
    reply += "/test - Test sistema\n";
    
    bot.sendMessage(chat_id, reply, "");
    return;
  }

  if (cmd == "/datasensores") {
    sendCompleteStatusToTelegram(chat_id);
    return;
  }

  if (cmd == "/status") {
    String message = "Estado General:\n\n";
    message += "Reportes: " + String(telegramReportIntervalMin) + " min\n";
    message += "WiFi: " + String(WiFi.RSSI()) + " dBm\n";
    message += "IP: " + WiFi.localIP().toString() + "\n";
    message += "Sistema: " + String(systemShutdown ? "OFF" : "ON") + "\n";
    message += getDateTime();
    
    bot.sendMessage(chat_id, message, "");
    return;
  }

  if (cmd == "/infodevices") {
    uint64_t chipid = ESP.getEfuseMac();
    String chipIdStr = String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
    
    String message = "Info Dispositivo\n\n";
    message += "Chip ID: " + chipIdStr + "\n";
    message += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    message += "Uptime: " + String(millis() / 1000) + "s\n";
    message += "IP: " + WiFi.localIP().toString();
    
    bot.sendMessage(chat_id, message, "");
    return;
  }

  if (cmd == "/pump_on" && !systemShutdown) {
    setPumpState(true, "Comando Telegram");
    bot.sendMessage(chat_id, "Bomba ON manual", "");
    return;
  } 

  if (cmd == "/pump_off") {
    setPumpState(false, "Comando Telegram");
    bot.sendMessage(chat_id, "Bomba OFF manual", "");
    return;
  }

  if (cmd == "/auto_mode") {
    manualPumpControl = false;
    bot.sendMessage(chat_id, "Modo automático", "");
    return;
  }

  if (cmd == "/test") {
    bot.sendMessage(chat_id, "Test iniciado...", "");
    handleTestButton();
    sendCompleteStatusToTelegram(chat_id);
    return;
  }

  if (cmd.startsWith("/setinterval")) {
    int sp = cmd.indexOf(' ');
    if (sp > 0) {
      int newInterval = cmd.substring(sp+1).toInt();
      if (newInterval > 0 && newInterval <= 1440) {
        telegramReportIntervalMin = newInterval;
        bot.sendMessage(chat_id, "Intervalo: " + String(newInterval) + " min", "");
      } else {
        bot.sendMessage(chat_id, "Intervalo 1-1440 min", "");
      }
    } else {
      bot.sendMessage(chat_id, "Uso: /setinterval [min]", "");
    }
    return;
  }

  bot.sendMessage(chat_id, "Comando no reconocido\nUsa /menu", "");
}

// === SETUP ===
void setup() {
  Serial.begin(115200);
  Serial.println("Iniciando BINASAMAN...");
  
  // Configurar pines
  for (int i = 0; i < 3; i++) {
    pinMode(SENSOR_PINS[i], INPUT_PULLUP);
    pinMode(WATER_LEVEL_LEDS[i], OUTPUT);
    digitalWrite(WATER_LEVEL_LEDS[i], LOW);
  }
  
  // Configurar bomba con relé LOW-Trigger (LOW = ON, HIGH = OFF)
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, HIGH);  // Inicialmente APAGADA
  
  pinMode(PUMP_LED_PIN, OUTPUT);
  digitalWrite(PUMP_LED_PIN, LOW);
  pinMode(SHUTDOWN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);
  pinMode(FOAM_SENSOR_PIN, INPUT);  // TCR5000 sin pull-up
  
  // VERIFICACIÓN CRÍTICA DEL TCR5000
  Serial.println("\n=== VERIFICACIÓN TCR5000 ===");
  delay(100);
  int sensorInitial = digitalRead(FOAM_SENSOR_PIN);
  Serial.printf("Estado inicial TCR5000 (GPIO5): %d\n", sensorInitial);
  Serial.println("LOW = sensor detecta (superficie cerca)");
  Serial.println("HIGH = sensor NO detecta (sin superficie)");
  Serial.println("=== FIN VERIFICACIÓN ===\n");
  if (sensorInitial != HIGH && sensorInitial != LOW) {
    Serial.println("ERROR: Lectura inválida del TCR5000. Verifica conexiones.");
    while (true) {
      delay(1000);
    }
}
  // Inicializar displays
  initOLED();
  initLCD();

  // Mostrar mensaje inicial en LCD por más tiempo
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...    ");
  delay(1000);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("Error SPIFFS");
    lcd.setCursor(0, 1);
    lcd.print("Error SPIFFS    ");
    delay(1000);
  }

  // WiFi
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Conectando WiFi");
  
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  
  if (!wm.autoConnect("BINASAMAN-Setup")) {
    Serial.println("WiFi falló, reiniciando...");
    lcd.setCursor(0, 1);
    lcd.print("Reiniciando...  ");
    delay(1000);
    ESP.restart();
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi: OK        ");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP().toString());
  delay(1000);
  
  Serial.println("WiFi: " + WiFi.localIP().toString());

  // Tiempo
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sinc. tiempo... ");
  delay(500);

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

  // Mensaje inicio Telegram
  String startMsg = "Sistema Iniciado\n\n";
  startMsg += "IP: " + WiFi.localIP().toString() + "\n";
  startMsg += "WiFi: " + WiFi.SSID() + "\n";
  startMsg += "Vinaza: Sensor OK";
  
  bot.sendMessage(TELEGRAM_CHAT_ID, startMsg, "");
  addLog("Sistema iniciado OK");
  
  // Mostrar estado inicial en LCD
  updateLCDNormal();
}

// === LOOP ===
void loop() {
  server.handleClient();
  webSocket.loop();

  // Botones físicos
  handleShutdownButton();
  handleTestButton();

  unsigned long now = millis();

  // DEBUG del estado cada 3 segundos
  static unsigned long lastDebug = 0;
  if (now - lastDebug >= 3000) {
    lastDebug = now;
    Serial.printf("[DEBUG] TCR5000: %d, Bomba: %s, Manual: %s, Shutdown: %s\n",
                  digitalRead(FOAM_SENSOR_PIN),
                  pumpState ? "ON" : "OFF",
                  manualPumpControl ? "SI" : "NO",
                  systemShutdown ? "SI" : "NO");
                  
  }

  // Sensores cada 1 segundo (INCLUYE TCR5000 CON DEBOUNCE)
  static unsigned long lastSensor = 0;
  if (!systemShutdown && (now - lastSensor >= 300)) {
    lastSensor = now;
    sampleSensors();
    checkForChanges();
  }

  // WebSocket cuando hay cambios
  if (dataChanged) {
    broadcastWS();
    dataChanged = false;
  }

  // LEDs cada 100ms
  static unsigned long lastLED = 0;
  if (now - lastLED >= 100) {
    lastLED = now;
    updateLEDs();
  }

  // OLED cada 2 segundos
  static unsigned long lastOLED = 0;
  if (now - lastOLED >= 2000) {
    lastOLED = now;
    updateOLED();
  }

  // LCD cada 2 segundos
  static unsigned long lastLCD = 0;
  if (now - lastLCD >= 2000) {
    lastLCD = now;
    updateLCDNormal();
  }

  // Google Sheets cada 1 minuto
  if (!systemShutdown && (now - lastSheetSend >= SHEETS_INTERVAL_MS)) {
    lastSheetSend = now;
    sendToGoogleSheets();
  }

  // Telegram mensajes cada 3 segundos
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
      String report = "REPORTE AUTOMÁTICO\n\n";
      report += "Agua: " + String(waterLevelPercent) + "%\n";
      report += "Vinaza: " + String(foamPercent) + "%\n";
      report += "Bomba: " + String(pumpState ? "ON" : "OFF") + "\n";
      report += "Modo: " + String(manualPumpControl ? "MANUAL" : "AUTO") + "\n";
      report += getDateTime();
      bot.sendMessage(TELEGRAM_CHAT_ID, report, "");
    }
  }

  delay(10);
}