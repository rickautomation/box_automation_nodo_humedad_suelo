#include <WiFi.h>              
#include <HTTPClient.h>        
#include <ArduinoJson.h>       
#include <Update.h>            
#include <WiFiClientSecure.h>  
#include <Preferences.h>        
#include <WebServer.h>          
#include <DNSServer.h>          
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>


// ======================================================
// TELEMETRIA RTC
// ======================================================
#include <esp_system.h>
RTC_DATA_ATTR uint32_t rtc_boot_count = 0;
RTC_DATA_ATTR uint32_t rtc_wdt_resets = 0;
RTC_DATA_ATTR uint32_t rtc_wifi_disconnects = 0;
RTC_DATA_ATTR uint32_t rtc_http_errors = 0;

// ======================================================
// 0. VERSIÓN LOCAL DEL FIRMWARE
// ======================================================
const char* FIRMWARE_VERSION_CODE = "1.0.4";
String latestFirmwareVersion = FIRMWARE_VERSION_CODE;

// ======================================================
// 1. CONFIGURACIÓN DE RED, FIREBASE Y PORTAL CAUTIVO
// ======================================================

// ⚠️ REEMPLAZAR CON TUS CLAVES Y HOST DE FIREBASE
const char* API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8"; 
const char* RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";                   

// 🔑 CREDENCIALES POR DEFECTO 🔑
const char* DEFAULT_SSID = "tili";         
const char* DEFAULT_PASS = "Ubuntu1234$"; 

// NVS y Portal Cautivo
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

const char* PREFS_NAMESPACE = "wifi_config";
const char* PREF_SSID = "ssid";
const char* PREF_PASS = "pass";
const char* AP_SSID = "NODO_SUELO_SETUP"; // Nombre del AP para configuración

String loadedSsid = "";
String loadedPassword = "";

const int WIFI_RESET_PIN = 9; // GPIO 9 (Botón BOOT)

// ======================================================
// 2. CONFIGURACIÓN DINÁMICA (LEÍDA DE FIREBASE)
// ======================================================
// Valores por defecto (Fallback) 
String backendHost = "192.168.68.68";    
int backendPort = 3000;                  
String endpoint = "/sensor-data/arduino/batch"; // Valor por defecto
long intervaloEnvioMs = 4000;            
bool flagActivo = true;                  
String remoteFirmwareVersion = "0.0.0"; 
String firmwareUrl = "";                 

const String RTDB_CONFIG_URL_BASE = "https://" + String(RTDB_HOST) + "/.json";
const char* NODE_TYPE_KEY = "NODO_SUELO"; 

const int TIEMPO_MAX_CONEXION_WIFI = 15000; 
const long CONFIG_FETCH_INTERVAL = 60000; // 1 minuto

// ======================================================
// 3. DATOS DEL DISPOSITIVO Y SENSORES (MÚLTIPLES SENSORES) 
// ======================================================
// 🆕 ESTA VARIABLE ALMACENARÁ EL SERIAL ÚNICO GENERADO POR LA MAC
String boxSerialId; 

// 💧 CONFIGURACIÓN PARA MÚLTIPLES SENSORES DE SUELO
// Pines ADC en la ESP32-C3 (A0 suele mapear al GPIO 4 en las SuperMini)
const int sensorPins[] = {4}; 
const int numSensores = sizeof(sensorPins) / sizeof(sensorPins[0]); 
// Etiquetas que el backend espera
const char* arduinoPins[] = {"A0"}; 

// ****************** VALORES DE CALIBRACIÓN COMÚN ******************
const int valorSeco = 4095;  
const int valorMojado = 250; 
// ************************************************************

const int NUM_MUESTRAS = 10;

// --- ⏱️ VARIABLES GLOBALES DEL CICLO ---
unsigned long tiempoUltimaMuestra = 0;
unsigned long lastConfigFetch = 0; 
int muestrasTomadas = 0;
// Array bidimensional para guardar las lecturas de CADA sensor (5 sensores x 10 muestras)
int lecturas[numSensores][NUM_MUESTRAS]; 
int mediasCrudas[numSensores] = {0}; 


// ======================================================
// 4. DECLARACIONES DE FUNCIONES
// ======================================================
void tomar_y_acumular_muestras();
bool conectar_wifi();
void enviar_post();
void logMessage(String level, String msg);


// NVS y Portal Cautivo
void saveCredentials(const String& ssid, const String& password);
bool loadCredentials();
void clearCredentials(); 
void startConfigPortal();
void handleRoot();
void handleSave();

// Remote Config y OTA
bool obtener_remote_config(); 
int compareVersions(String current, String remote);
bool check_for_update();
void perform_update();


// ======================================================
// SETUP: Inicialización y Generación del Serial ID
// ======================================================

void sendTelemetry() {
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) + "/api/health/metrics";
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    String jsonStr = "{\"boxSerialId\":\"" + boxSerialId + "\",";
    jsonStr += "\"boot_count\":" + String(rtc_boot_count) + ",";
    jsonStr += "\"wdt_resets\":" + String(rtc_wdt_resets) + ",";
    jsonStr += "\"wifi_disconnects\":" + String(rtc_wifi_disconnects) + ",";
    jsonStr += "\"http_errors\":" + String(rtc_http_errors) + "}";
    http.POST(jsonStr);
    http.end();
  }
}

void setup() {
  Serial.begin(115200);

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON) {
    rtc_boot_count = 0;
    rtc_wdt_resets = 0;
    rtc_wifi_disconnects = 0;
    rtc_http_errors = 0;
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_PANIC) {
    rtc_wdt_resets++;
  }
  rtc_boot_count++;
 
  
  // Iniciar Hardware Watchdog (30 segundos) - API v3.x (ESP-IDF v5)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_task_wdt_add(NULL);

  delay(1000); 
  
  preferences.begin(PREFS_NAMESPACE, false);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  delay(100); 

  resetWifiStack();
  WiFi.mode(WIFI_STA); 
  boxSerialId = WiFi.macAddress();
  boxSerialId.replace(":", ""); 
  
  logMessage("INFO", "\n--- 🪴 Nodo de Múltiples Sensores de Suelo (Final) 🪴 ---");
  logMessage("INFO", "🆔 ID: " + boxSerialId);
  
  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    clearCredentials(); 
    startConfigPortal(); 
  }
  
  bool credentialsLoaded = loadCredentials();
  if (!credentialsLoaded) {
      saveCredentials(DEFAULT_SSID, DEFAULT_PASS); 
      loadCredentials();
      credentialsLoaded = true; 
  }
  
  if (credentialsLoaded && conectar_wifi()) {
      ArduinoOTA.begin();
      obtener_remote_config(); 
      check_for_update();
      lastConfigFetch = millis();
  } else {
      startConfigPortal(); 
  }
}

// ======================================================
// BUCLE PRINCIPAL (LÓGICA DE ESTADOS Y TIEMPO DINÁMICO)
// ======================================================
void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();

  unsigned long tiempoActual = millis();

  static unsigned long lastTelemetry = 0;
  if (tiempoActual - lastTelemetry >= 3600000 || lastTelemetry == 0) { // 1 hora o al iniciar
    lastTelemetry = tiempoActual == 0 ? 1 : tiempoActual;
    sendTelemetry();
  }

  
  if (tiempoActual - lastConfigFetch >= CONFIG_FETCH_INTERVAL) {
      if (conectar_wifi()) { 
          obtener_remote_config();
          check_for_update();
          lastConfigFetch = tiempoActual;
      }
  }
  
  if (tiempoActual - tiempoUltimaMuestra >= intervaloEnvioMs) { 
    tiempoUltimaMuestra = tiempoActual;
    
    // Muestrear y promediar bloqueante rapido
    for(int s=0; s<numSensores; s++) {
      long sum = 0;
      for (int i=0; i<NUM_MUESTRAS; i++) {
        sum += analogRead(sensorPins[s]);
        delay(2);
      }
      mediasCrudas[s] = sum / NUM_MUESTRAS;
    }
    
    if (flagActivo && conectar_wifi()) {
      enviar_post(); 
    }
  }
}


// ======================================================
// FUNCIONES DE MUESTREO Y ENVÍO
// ======================================================

// (tomar_y_acumular_muestras ya no se usa, lo integramos en loop)

void enviar_post() {
  logMessage("INFO", "📦 Filtrando, calculando media y preparando envío POST...");
  
  for (int i = 0; i < numSensores; i++) {
    
    // Cálculo de porcentaje
    int porcentaje = map(mediasCrudas[i], valorMojado, valorSeco, 100, 0);
    if (porcentaje < 0) porcentaje = 0;
    if (porcentaje > 100) porcentaje = 100;
    
    Serial.printf(F("Sensor %d (%s): %d (Humedad: %d%%)\n"), i + 1, arduinoPins[i], mediasCrudas[i], porcentaje);
  }

  // 2. Crear el JSON en formato batch
  DynamicJsonDocument doc(1024); 
  // 🆕 Usa el ID ÚNICO generado por MAC
  doc["boxSerialId"] = boxSerialId; 
  JsonArray dataArray = doc.createNestedArray("data");
  
  // Llenar el arreglo 'data' con los valores de los 5 sensores
  for (int i = 0; i < numSensores; i++) {
    JsonObject item = dataArray.createNestedObject();
    
    item["arduinoPin"] = arduinoPins[i]; 
    item["raw"] = mediasCrudas[i];
    item["unit"] = "%";
    item["key"] = String("humedad_suelo_") + arduinoPins[i]; 
  }
  
  // 3. Serializar y Enviar
  String jsonBuffer;
  serializeJson(doc, jsonBuffer);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    // URL usa backendHost, Port y endpoint DINÁMICOS
    String url = "http://" + backendHost + ":" + String(backendPort) + endpoint; 
    
    Serial.printf(F("URL de envío: %s\n"), url.c_str());
    
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    
    int httpResponseCode = http.POST(jsonBuffer);
    
    if (httpResponseCode > 0) {
      Serial.printf(F("✅ POST exitoso. Código: %d\n"), httpResponseCode);
    } else {
      Serial.printf(F("❌ Error en el POST. Código: %d. Mensaje: %s\n"), httpResponseCode, http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

// resetear_ciclo no se usa

// ======================================================
// IMPLEMENTACIONES DE GESTIÓN DE RED Y CONFIGURACIÓN REMOTA
// ======================================================

// *** Funciones de NVS y Portal Cautivo (omitiendo código por longitud, pero no hubo cambios funcionales aquí) ***

void saveCredentials(const String& ssid, const String& password) {
  preferences.putString(PREF_SSID, ssid);
  preferences.putString(PREF_PASS, password);
  loadedSsid = ssid;
  loadedPassword = password;
  Serial.printf(F("💾 Credenciales guardadas: SSID = %s\n"), ssid.c_str());
}

bool loadCredentials() {
  loadedSsid = preferences.getString(PREF_SSID, "");
  loadedPassword = preferences.getString(PREF_PASS, "");
  return loadedSsid.length() > 0;
}

void clearCredentials() {
    preferences.remove(PREF_SSID);
    preferences.remove(PREF_PASS);
    loadedSsid = "";
    loadedPassword = "";
    Serial.println(F("🗑️ CREDENCIALES BORRADAS DE NVS."));
}

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  
  WiFi.softAPConfig(localIP, gateway, subnet);
  WiFi.softAP(AP_SSID);
  
  Serial.printf(F("AP creado. Conéctate a '%s' para configurar.\n"), AP_SSID);

  DNSServer dnsServer;
  dnsServer.start(53, "*", localIP);
  
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  unsigned long portalStart = millis();
  while (millis() - portalStart < 180000) { // 3 minutos de timeout
    esp_task_wdt_reset();
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
  }
  Serial.println(F("⏳ Timeout del Portal Cautivo (3 min). Reiniciando nodo para buscar Wi-Fi..."));
  delay(1000);
  ESP.restart();
}


void handleRoot() {
  String html = R"raw(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Configuracion NODO SUELO</title><style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background-color: #f4f7f6; }
    .container { max-width: 400px; margin: auto; padding: 25px; background: #ffffff; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
    h1 { color: #2E7D32; margin-bottom: 20px; font-size: 24px; }
    input[type="text"], input[type="password"] { width: 100%; padding: 12px; margin: 10px 0 20px 0; border: 1px solid #ccc; border-radius: 6px; box-sizing: border-box; font-size: 16px; }
    input[type="submit"] { background-color: #2E7D32; color: white; padding: 14px 20px; border: none; border-radius: 6px; cursor: pointer; width: 100%; font-size: 18px; transition: background-color 0.3s; }
    input[type="submit"]:hover { background-color: #1B5E20; }
    .footer { margin-top: 20px; color: #757575; font-size: 14px; }
    .logo { color: #2E7D32; font-size: 30px; margin-bottom: 10px; }
  </style></head>
  <body><div class="container"><div class="logo">🪴</div><h1>Configura tu Nodo Suelo</h1>
  <p>Conéctate a tu red Wi-Fi para que el nodo pueda enviar datos.</p>
  <p style="font-size: 12px; color: #B00020; font-weight: bold;">MANTÉN PRESIONADO BOOT AL INICIAR para entrar aquí.</p>
  <form method="POST" action="/save">
    <label for="ssid">SSID (Nombre de la Red):</label><input type="text" id="ssid" name="ssid" required placeholder="MiRedWiFi">
    <label for="password">Contraseña:</label><input type="password" id="password" name="password" placeholder="Dejar vacío si no tiene clave">
    <input type="submit" value="Guardar y Conectar">
  </form><div class="footer">Version Firmware: )raw" + String(FIRMWARE_VERSION_CODE) + R"raw(</div></div></body></html>)raw";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  
  if (newSsid.length() > 0) {
    saveCredentials(newSsid, newPassword);
    
    String successHtml = R"raw(<!DOCTYPE html><html><head><meta http-equiv="refresh" content="5;url=/" /></head><body>
      <div style="text-align: center; margin-top: 50px;"><h1>✅ Credenciales Guardadas</h1>
        <p>Intentando conectar a la red: <strong>)raw" + newSsid + R"raw(</strong></p>
        <p>El nodo se reiniciará en 5 segundos para aplicar la nueva configuración.</p></div></body></html>)raw";
    server.send(200, "text/html", successHtml);
    
    server.stop();
    // dnsServer.stop(); // Se elimina la llamada a dnsServer.stop() aquí para evitar posibles crashes.
    Serial.println(F("🔄 Reiniciando ESP32..."));
    ESP.restart();
  } else {
    server.send(200, "text/html", "<h1>❌ ERROR: SSID vacío.</h1>");
  }
}


void resetWifiStack() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000);
}
bool conectar_wifi() {
  if (loadedSsid.length() == 0) return false;
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print(F("\n📡 Encendiendo Wi-Fi y conectando a: ")); Serial.print(loadedSsid);
  resetWifiStack();
  WiFi.mode(WIFI_STA);
  // Reducimos la potencia Tx para mejorar la estabilidad si se usa ADC
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  
  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio < TIEMPO_MAX_CONEXION_WIFI)) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(F("\n✅ WiFi Conectado. IP: %s\n"), WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.printf(F("\n❌ Falló la conexión a WiFi.\n"));
    return false;
  }
}


// *** Funciones de Remote Config y OTA ***

bool obtener_remote_config() {
  Serial.println(F("\n--- Obteniendo Configuración Dinámica ---"));
  
  if (WiFi.status() != WL_CONNECTED) return false;
  
  String fullUrl = RTDB_CONFIG_URL_BASE + "?auth=" + String(API_KEY); 
  
  HTTPClient http;
  http.begin(fullUrl); 
  http.setTimeout(3000);
  int httpCode = http.GET();
  
  if (httpCode == 200) { 
    String payload = http.getString();
    DynamicJsonDocument doc(1536); 
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.printf(F("❌ Fallo al parsear JSON: %s\n"), error.c_str());
      http.end();
      return false;
    }

    JsonObject remoteConfig = doc[F("remote_config")];
    if (!remoteConfig.isNull()) {
        if (remoteConfig.containsKey(F("backend_host"))) backendHost = remoteConfig[F("backend_host")].as<String>();
        if (remoteConfig.containsKey(F("backend_port"))) backendPort = remoteConfig[F("backend_port")].as<int>();
        
        // 🆕 CAMBIO CLAVE: BUSCAR 'endpoint_humedad_suelo'
        if (remoteConfig.containsKey(F("endpoint_humedad_suelo"))) {
            endpoint = remoteConfig[F("endpoint_humedad_suelo")].as<String>();
        } else if (remoteConfig.containsKey(F("endpoint_humedad"))) {
            // Fallback por si la clave vieja todavía existe
            endpoint = remoteConfig[F("endpoint_humedad")].as<String>(); 
        }

        if (remoteConfig.containsKey(F("intervalo_envio_ms"))) intervaloEnvioMs = remoteConfig[F("intervalo_envio_ms")].as<long>();
        if (remoteConfig.containsKey(F("flag_activo"))) flagActivo = remoteConfig[F("flag_activo")].as<bool>();
    }
    
    JsonObject nodeConfig = doc[F("firmware_updates")][NODE_TYPE_KEY];
    if (!nodeConfig.isNull()) {
        if (nodeConfig.containsKey(F("latest_firmware_version"))) remoteFirmwareVersion = nodeConfig[F("latest_firmware_version")].as<String>();
        if (nodeConfig.containsKey(F("firmware_url"))) firmwareUrl = nodeConfig[F("firmware_url")].as<String>();
    }
    
    Serial.printf(F("Backend: %s:%d%s | Intervalo: %ld ms | Ver. Remota: %s\n"), backendHost.c_str(), backendPort, endpoint.c_str(), intervaloEnvioMs, remoteFirmwareVersion.c_str());
    http.end();
    return true;
  } else {
    Serial.printf(F("❌ Fallo al obtener la configuración (HTTP Code: %d).\n"), httpCode);
    http.end();
    return false;
  }
}

int compareVersions(String current, String remote) {
  int cur_v[3] = {0, 0, 0};
  int rem_v[3] = {0, 0, 0};

  sscanf(current.c_str(), "%d.%d.%d", &cur_v[0], &cur_v[1], &cur_v[2]);
  sscanf(remote.c_str(), "%d.%d.%d", &rem_v[0], &rem_v[1], &rem_v[2]);

  for (int i = 0; i < 3; i++) {
    if (cur_v[i] < rem_v[i]) return -1;
    if (cur_v[i] > rem_v[i]) return 1;
  }
  return 0; 
}

bool check_for_update() {
  if (remoteFirmwareVersion.isEmpty() || compareVersions(latestFirmwareVersion, remoteFirmwareVersion) >= 0) {
    Serial.printf(F("✅ OTA: La versión actual (%s) está al día.\n"), latestFirmwareVersion.c_str());
    return false;
  }

  Serial.printf(F("🔴 📢 ACTUALIZACIÓN REQUERIDA: %s -> %s\n"), latestFirmwareVersion.c_str(), remoteFirmwareVersion.c_str());
  if (!firmwareUrl.isEmpty()) {
    perform_update();
    return true;
  } else {
    Serial.println(F("❌ ERROR OTA: URL de firmware vacía. No se puede actualizar."));
    return false;
  }
}

void perform_update() {
  Serial.printf(F("🚀 Iniciando actualización OTA desde: %s\n"), firmwareUrl.c_str());
  
  if (!firmwareUrl.startsWith("https://")) {
      Serial.println(F("❌ ERROR: La URL del firmware debe ser HTTPS."));
      return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Permite conexiones sin verificación de certificado (para GitHub raw o testing)
  
  HTTPClient http;
  
  if (http.begin(client, firmwareUrl)) {
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      int contentLength = http.getSize();
      Serial.printf(F("Tamaño del nuevo firmware: %d bytes.\n"), contentLength);
      
      if (Update.begin(contentLength)) {
        WiFiClient* stream = http.getStreamPtr(); 
        size_t written = Update.writeStream(*stream);
        
        if (written == contentLength) {
          Serial.printf(F("Descarga y escritura completada: %d bytes.\n"), written);
        } else {
          Serial.printf(F("❌ Error de escritura. Escrito %zu de %d bytes.\n"), written, contentLength);
        }
        
        if (Update.end()) {
          Serial.println(F("✅ Actualización finalizada exitosamente. Reiniciando..."));
          ESP.restart(); 
        } else {
          Serial.printf(F("❌ Error al finalizar. Error: %d. Mensaje: %s\n"), Update.getError(), Update.errorString());
        }
      } else {
        Serial.println(F("❌ ERROR: No hay suficiente espacio para la actualización."));
      }
    } else {
      Serial.printf(F("❌ ERROR HTTP (%d): No se pudo descargar el archivo.\n"), httpCode);
    }
    http.end();
  } else {
    Serial.println(F("❌ ERROR: No se pudo conectar a la URL de firmware."));
  }
}

void logMessage(String level, String msg) {
  Serial.println("[" + level + "] " + msg);
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) + "/sensor-data/logs";
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(512);
    doc["boxSerialId"] = boxSerialId;
    doc["level"] = level;
    doc["message"] = msg;
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    http.POST(jsonStr);
    http.end();
  }
}