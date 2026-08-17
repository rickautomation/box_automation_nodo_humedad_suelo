#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
const char *FIRMWARE_VERSION_CODE = "1.0.8-suelo";
String latestFirmwareVersion = FIRMWARE_VERSION_CODE;

// ======================================================
// 1. CONFIGURACIÓN DE RED, FIREBASE Y PORTAL CAUTIVO
// ======================================================

// ⚠️ REEMPLAZAR CON TUS CLAVES Y HOST DE FIREBASE
const char *API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8";
const char *RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";

// 🔑 CREDENCIALES POR DEFECTO 🔑
const char *DEFAULT_SSID = "tili";
const char *DEFAULT_PASS = "Ubuntu1234$";

// NVS y Portal Cautivo
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

const char *PREFS_NAMESPACE = "wifi_config";
const char *PREF_SSID = "ssid";
const char *PREF_PASS = "pass";
const char *AP_SSID = "NODO_SUELO_SETUP"; // Nombre del AP para configuración

String loadedSsid = "";
String loadedPassword = "";

const int WIFI_RESET_PIN = 9; // GPIO 9 (Botón BOOT)

// ======================================================
// 2. CONFIGURACIÓN DINÁMICA (Nodriza)
// ======================================================
String backendHost = ""; // Si está vacío, es Lobo Solitario (reporta directo a la Nodriza)
int backendPort = 3000;
long intervaloEnvioMs = 60000;
bool isLoneWolf = true;
bool flagActivo = true;

const String NODRIZA_HOST = "nodrizabackend-production.up.railway.app";
const int TIEMPO_MAX_CONEXION_WIFI = 20000;
const long CONFIG_FETCH_INTERVAL = 60000;

// ======================================================
// 3. DATOS DEL DISPOSITIVO Y SENSORES (MÚLTIPLES SENSORES)
// ======================================================
// 🆕 ESTA VARIABLE ALMACENARÁ EL SERIAL ÚNICO GENERADO POR LA MAC
String boxSerialId;

// 💧 CONFIGURACIÓN PARA MÚLTIPLES SENSORES DE SUELO (4 SENSORES - ADC1)
// Sensor 1: GPIO 0 (A0 / ADC1_CH0)
// Sensor 2: GPIO 1 (A1 / ADC1_CH1)
// Sensor 3: GPIO 3 (A3 / ADC1_CH3)
// Sensor 4: GPIO 4 (A4 / ADC1_CH4)
const int sensorPins[] = {0, 1, 3, 4};
const int numSensores = sizeof(sensorPins) / sizeof(sensorPins[0]);
// Etiquetas asociadas a cada pin
const char *arduinoPins[] = {"A0 (GPIO0)", "A1 (GPIO1)", "A3 (GPIO3)", "A4 (GPIO4)"};

// ****************** VALORES DE CALIBRACIÓN COMÚN ******************
// Sensores Capacitivos v1.2: En aire leen ~3350 (0% H), en agua leen ~1500 (100% H)
const int valorSeco = 3350;
const int valorMojado = 1500;
// ************************************************************

const int NUM_MUESTRAS = 15;

// --- ⏱️ VARIABLES GLOBALES DEL CICLO ---
unsigned long tiempoUltimaMuestra = 0;
unsigned long lastConfigFetch = 0;
int muestrasTomadas = 0;
// Array bidimensional para guardar las lecturas de CADA sensor
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
void saveCredentials(const String &ssid, const String &password);
bool loadCredentials();
void clearCredentials();
void startConfigPortal();
void handleRoot();
void handleSave();

// Remote Config y OTA
void obtener_remote_config();
int compareVersions(String current, String remote);
bool check_for_update();
void perform_update();

// ======================================================
// SETUP: Inicialización y Generación del Serial ID
// ======================================================

void sendTelemetry() {
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) +
                 "/api/health/metrics";
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
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
             reason == ESP_RST_PANIC) {
    rtc_wdt_resets++;
  }
  rtc_boot_count++;

  // Iniciar Hardware Watchdog (30 segundos) - API v3.x (ESP-IDF v5)
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true};
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_task_wdt_add(NULL);

  delay(1000);

  preferences.begin(PREFS_NAMESPACE, false);
  backendHost = preferences.getString("bHost", "192.168.68.82");
  backendPort = preferences.getInt("bPort", 5001);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  
  // Configurar resolución y atenuación ADC1 (rango 0V - 3.3V)
  analogSetAttenuation(ADC_11db);
  for (int i = 0; i < numSensores; i++) {
    pinMode(sensorPins[i], INPUT);
  }
  delay(100);

  resetWifiStack();
  WiFi.mode(WIFI_STA);
  boxSerialId = WiFi.macAddress();
  boxSerialId.replace(":", "");

  logMessage("INFO",
             "\n--- 🪴 Nodo de Múltiples Sensores de Suelo (Final) 🪴 ---");
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
  if (tiempoActual - lastTelemetry >= 3600000 ||
      lastTelemetry == 0) { // 1 hora o al iniciar
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

    // Muestrear y promediar con Filtro de Mediana
    for (int s = 0; s < numSensores; s++) {
      int muestrasArr[NUM_MUESTRAS];
      Serial.printf(F("\n--- Sensor %d (Pin %s) ---\n"), s + 1, arduinoPins[s]);
      Serial.print(F("Muestras crudas: ["));
      for (int i = 0; i < NUM_MUESTRAS; i++) {
        muestrasArr[i] = analogRead(sensorPins[s]);
        Serial.print(muestrasArr[i]);
        if (i < NUM_MUESTRAS - 1)
          Serial.print(", ");
        delay(5); // Pequeña pausa entre muestras
      }
      Serial.println(F("]"));

      // Ordenar arreglo (Bubble Sort)
      for (int i = 0; i < NUM_MUESTRAS - 1; i++) {
        for (int j = i + 1; j < NUM_MUESTRAS; j++) {
          if (muestrasArr[i] > muestrasArr[j]) {
            int temp = muestrasArr[i];
            muestrasArr[i] = muestrasArr[j];
            muestrasArr[j] = temp;
          }
        }
      }

      // Mostrar arreglo ordenado
      Serial.print(F("Muestras ordenadas: ["));
      for (int i = 0; i < NUM_MUESTRAS; i++) {
        Serial.print(muestrasArr[i]);
        if (i < NUM_MUESTRAS - 1)
          Serial.print(", ");
      }
      Serial.println(F("]"));

      // Descartar extremos (aprox 25% superior y 25% inferior)
      int descarte = NUM_MUESTRAS / 4;
      long sum = 0;
      int validSamples = NUM_MUESTRAS - (descarte * 2);

      Serial.print(F("Valores centrales (Promediados): ["));
      for (int i = descarte; i < NUM_MUESTRAS - descarte; i++) {
        sum += muestrasArr[i];
        Serial.print(muestrasArr[i]);
        if (i < NUM_MUESTRAS - descarte - 1)
          Serial.print(", ");
      }
      Serial.println(F("]"));

      mediasCrudas[s] = sum / validSamples;
      int porcentaje = map(mediasCrudas[s], valorMojado, valorSeco, 100, 0);
      if (porcentaje < 0) porcentaje = 0;
      if (porcentaje > 100) porcentaje = 100;
      Serial.printf(F("Promedio final filtrado: %d | Humedad estimada: %d%%\n"), mediasCrudas[s], porcentaje);
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
  logMessage("INFO", "📦 Preparando envío POST...");

  HTTPClient http;
  String url;

  if (!isLoneWolf && backendHost != "") {
    // Modo Enjambre: Enviar a la GreenBox local
    url = "http://" + backendHost + ":" + String(backendPort) + "/sensor-data/soil";
    
    DynamicJsonDocument doc(512);
    doc["boxSerialId"] = boxSerialId;

    int sumaPorcentajes = 0;
    JsonArray sensoresArr = doc.createNestedArray("sensors");
    JsonArray dataArr = doc.createNestedArray("data");

    for (int i = 0; i < numSensores; i++) {
      int porcentaje = map(mediasCrudas[i], valorMojado, valorSeco, 100, 0);
      if (porcentaje < 0) porcentaje = 0;
      if (porcentaje > 100) porcentaje = 100;
      sumaPorcentajes += porcentaje;

      JsonObject sObj = sensoresArr.createNestedObject();
      sObj["pin"] = arduinoPins[i];
      sObj["raw"] = mediasCrudas[i];
      sObj["humidity"] = porcentaje;

      JsonObject dObj = dataArr.createNestedObject();
      dObj["arduinoPin"] = arduinoPins[i];
      dObj["raw"] = mediasCrudas[i];
      dObj["unit"] = "%";
      dObj["humidity"] = porcentaje;
    }

    int p0 = map(mediasCrudas[0], valorMojado, valorSeco, 100, 0);
    p0 = constrain(p0, 0, 100);
    doc["humidity"] = p0;
    doc["soilMoisture"] = p0;

    if (numSensores > 1) {
      int p1 = map(mediasCrudas[1], valorMojado, valorSeco, 100, 0);
      doc["soilMoistureA1"] = constrain(p1, 0, 100);
    }
    if (numSensores > 2) {
      int p2 = map(mediasCrudas[2], valorMojado, valorSeco, 100, 0);
      doc["soilMoistureA2"] = constrain(p2, 0, 100);
    }
    if (numSensores > 3) {
      int p3 = map(mediasCrudas[3], valorMojado, valorSeco, 100, 0);
      doc["soilMoistureA3"] = constrain(p3, 0, 100);
    }

    String jsonBuffer;
    serializeJson(doc, jsonBuffer);

    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(jsonBuffer);
    Serial.printf(F("📡 [GreenBox Local] POST -> Code: %d\n"), httpResponseCode);
    http.end();
  } else {
    // Modo Lobo Solitario: Enviar directo a la Nodriza Cloud
    url = "https://" + NODRIZA_HOST + "/api/sync";

    DynamicJsonDocument doc(1024);
    doc["carrierId"] = boxSerialId;
    doc["type"] = "ESP32";
    
    JsonArray readings = doc.createNestedArray("readings");

    JsonObject reading = readings.createNestedObject();
    reading["nodeId"] = boxSerialId;

    int p0 = map(mediasCrudas[0], valorMojado, valorSeco, 100, 0);
    p0 = constrain(p0, 0, 100);
    reading["soilMoisture"] = p0;

    if (numSensores > 1) {
      int p1 = map(mediasCrudas[1], valorMojado, valorSeco, 100, 0);
      reading["soilMoistureA1"] = constrain(p1, 0, 100);
    }
    if (numSensores > 2) {
      int p2 = map(mediasCrudas[2], valorMojado, valorSeco, 100, 0);
      reading["soilMoistureA2"] = constrain(p2, 0, 100);
    }
    if (numSensores > 3) {
      int p3 = map(mediasCrudas[3], valorMojado, valorSeco, 100, 0);
      reading["soilMoistureA3"] = constrain(p3, 0, 100);
    }

    String jsonBuffer;
    serializeJson(doc, jsonBuffer);

    WiFiClientSecure client;
    client.setInsecure();

    http.begin(client, url);
    http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(jsonBuffer);
    Serial.printf(F("🌐 [Nodriza Cloud Directo] POST -> Code: %d\n"), httpResponseCode);
    http.end();
  }
}

// resetear_ciclo no se usa

// ======================================================
// IMPLEMENTACIONES DE GESTIÓN DE RED Y CONFIGURACIÓN REMOTA
// ======================================================

// *** Funciones de NVS y Portal Cautivo (omitiendo código por longitud, pero no
// hubo cambios funcionales aquí) ***

void saveCredentials(const String &ssid, const String &password) {
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
  resetWifiStack();
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false); // CRÍTICO: Evita que el ESP32-C3 apague la radio
  IPAddress localIP(192, 168, 4, 1);
  WiFi.softAPConfig(localIP, localIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, NULL, 6, 0, 4);
  Serial.printf(F("📡 Portal activo. Red: '%s' → http://192.168.4.1\n"),
                AP_SSID);
  dnsServer.start(53, "*", localIP);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  server.on("/hotspot-detect.html", []() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  unsigned long portalStart = millis();
  while (millis() - portalStart < 600000) { // 10 minutos de timeout
    esp_task_wdt_reset();
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
  }
  Serial.println(F("⏳ Timeout del Portal Cautivo (10 min). Reiniciando nodo para buscar Wi-Fi..."));
  delay(1000);
  ESP.restart();
}

void handleRoot() {
  String html = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
  <title>Nodo Suelo</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      margin: 0; padding: 16px;
      background: #0a0a0a; color: #e8e8e8;
      min-height: 100vh; display: flex; align-items: center; justify-content: center;
    }
    .card {
      width: 100%; max-width: 420px;
      background: #141414; border: 1px solid #3d2f1f;
      border-radius: 16px; padding: 24px 20px;
      box-shadow: 0 8px 32px rgba(200, 140, 83, 0.12);
    }
    .logo { font-size: 40px; text-align: center; margin-bottom: 8px; }
    h1 { color: #e6a800; font-size: 22px; text-align: center; margin: 0 0 8px 0; }
    .sub { text-align: center; color: #9e9e9e; font-size: 14px; margin-bottom: 20px; line-height: 1.4; }
    label { display: block; color: #c88c00; font-size: 13px; font-weight: 600; margin: 12px 0 6px 0; }
    input[type="text"], input[type="password"] {
      width: 100%; padding: 14px 12px; font-size: 16px;
      background: #0a0a0a; color: #fff;
      border: 1px solid #7d5a2e; border-radius: 10px;
    }
    input:focus { outline: none; border-color: #e6a800; box-shadow: 0 0 0 2px rgba(230,168,0,0.2); }
    .btn {
      width: 100%; margin-top: 20px; padding: 15px;
      background: linear-gradient(135deg, #c88c00, #7d5a2e);
      color: #000; font-size: 17px; font-weight: 700;
      border: none; border-radius: 10px; cursor: pointer;
    }
    .hint {
      margin-top: 16px; padding: 10px; border-radius: 8px;
      background: #1a1a1a; border-left: 3px solid #e6a800;
      font-size: 12px; color: #bdbdbd; line-height: 1.5;
    }
    .footer { text-align: center; margin-top: 18px; font-size: 12px; color: #616161; }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">🪴</div>
    <h1>Nodo Suelo</h1>
    <p class="sub">Configura la red Wi-Fi para enviar niveles de humedad.</p>
    <form method="POST" action="/save" enctype="application/x-www-form-urlencoded">
      <label for="ssid">Nombre de la red (SSID)</label>
      <input type="text" id="ssid" name="ssid" required placeholder="MiRedWiFi" autocomplete="off" autocapitalize="none" spellcheck="false">
      <label for="password">Contraseña Wi-Fi</label>
      <input type="password" id="password" name="password" placeholder="Contraseña de la red" autocomplete="new-password" autocapitalize="none">
      <input class="btn" type="submit" value="Probar y Guardar">
    </form>
    <div class="hint">
      Usa red <strong>2.4 GHz</strong>. Mantén <strong>BOOT</strong> al encender para volver a este portal.
    </div>
    <div class="footer">Firmware v)raw" +
                String(FIRMWARE_VERSION_CODE) + R"raw(</div>
  </div>
</body>
</html>
)raw";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  if (newPassword.length() == 0)
    newPassword = server.arg("pass");
  if (newPassword.length() == 0)
    newPassword = server.arg("p");
  newSsid.trim();
  newPassword.trim();

  Serial.printf(F("📥 Portal recibió: SSID='%s', pass_len=%d\n"),
                newSsid.c_str(), newPassword.length());

  if (newSsid.length() == 0) {
    server.send(400, "text/html",
                "<html><body "
                "style='background:#0a0a0a;color:#fff;text-align:center;"
                "padding:40px;'><h1 style='color:#ff5252;'>SSID vacío</h1><a "
                "style='color:#e6a800;' href='/'>Volver</a></body></html>");
    return;
  }

  Serial.println(F("⏳ Probando credenciales antes de guardar..."));
  int estadoFinal = WL_DISCONNECTED;
  bool conecto = probarCredencialesWifi(newSsid, newPassword, estadoFinal);

  if (!conecto) {
    String errorHtml = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Error WiFi</title>
  <style>
    body { font-family: sans-serif; background:#0a0a0a; color:#e8e8e8; text-align:center; padding:32px 20px; }
    h1 { color:#ff5252; font-size:22px; }
    p { color:#bdbdbd; line-height:1.6; }
    .code { color:#e6a800; font-weight:bold; }
    a { display:inline-block; margin-top:20px; padding:14px 24px; background:#c88c00; color:#000; text-decoration:none; border-radius:10px; font-weight:700; }
  </style>
</head>
<body>
  <h1>❌ No se pudo conectar</h1>
  <p>SSID: <strong>)raw" +
                       newSsid + R"raw(</strong></p>
  <p>Estado WiFi: <span class="code">)raw" +
                       String(estadoFinal) + R"raw(</span></p>
  <p>Revisa SSID, contraseña y que sea red <strong>2.4 GHz</strong>.</p>
  <a href='/'>Intentar de nuevo</a>
</body>
</html>
)raw";
    server.send(200, "text/html", errorHtml);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    return;
  }

  saveCredentials(newSsid, newPassword);
  preferences.end();

  String successHtml = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Guardado</title>
  <style>
    body { font-family: sans-serif; background:#0a0a0a; color:#e8e8e8; text-align:center; padding:40px 20px; }
    h1 { color:#e6a800; }
    p { color:#bdbdbd; line-height:1.6; }
    .ssid { color:#c88c00; font-weight:bold; }
  </style>
</head>
<body>
  <h1>✅ Conexión exitosa</h1>
  <p>Red: <span class="ssid">)raw" +
                       newSsid + R"raw(</span></p>
  <p>Credenciales guardadas. Reiniciando...</p>
</body>
</html>
)raw";
  server.send(200, "text/html", successHtml);
  delay(1500);
  server.stop();
  dnsServer.stop();
  resetWifiStack();
  ESP.restart();
}

void resetWifiStack() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000);
}

bool probarCredencialesWifi(const String &ssid, const String &password,
                            int &estadoFinal) {
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long inicio = millis();
  rtc_wifi_disconnects++;
  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < TIEMPO_MAX_CONEXION_WIFI) {
    esp_task_wdt_reset();
    delay(300);
    dnsServer.processNextRequest();
    server.handleClient();
  }

  estadoFinal = WiFi.status();
  if (estadoFinal == WL_CONNECTED) {
    Serial.printf(F("✅ Prueba WiFi OK. IP: %s\n"),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.printf(F("❌ Prueba WiFi falló. Estado: %d\n"), estadoFinal);
  WiFi.disconnect(true);
  return false;
}

bool conectar_wifi() {
  if (loadedSsid.length() == 0)
    return false;
  if (WiFi.status() == WL_CONNECTED)
    return true;

  Serial.print(F("\n📡 Encendiendo Wi-Fi y conectando a: "));
  Serial.print(loadedSsid);
  resetWifiStack();
  WiFi.mode(WIFI_STA);
  // Reducimos la potencia Tx para mejorar la estabilidad si se usa ADC
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - inicio < TIEMPO_MAX_CONEXION_WIFI)) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(F("\n✅ WiFi Conectado. IP: %s\n"),
                  WiFi.localIP().toString().c_str());
    return true;
  } else {
    Serial.printf(F("\n❌ Falló la conexión a WiFi.\n"));
    return false;
  }
}

// *** Funciones de Remote Config y OTA ***

void obtener_remote_config() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  Serial.println(F("📥 Consultando configuración dinámica en la Nodriza..."));
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  String url = "https://" + NODRIZA_HOST + "/api/nodes/" + boxSerialId + "/config";
  http.begin(client, url);
  
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      isLoneWolf = doc["remote_config"]["is_lone_wolf"] | false;
      if (!isLoneWolf && doc["remote_config"]["backend_host"] && !doc["remote_config"]["backend_host"].isNull()) {
        backendHost = doc["remote_config"]["backend_host"].as<String>();
        backendPort = doc["remote_config"]["backend_port"] | 5001;
        
        Preferences prefs;
        prefs.begin("nodriza", false);
        prefs.putString("bHost", backendHost);
        prefs.putInt("bPort", backendPort);
        prefs.end();

        Serial.println(F("✅ Asignado a GreenBox (Enjambre). Guardado en NVS."));
        Serial.printf(F("   → IP GreenBox Local: %s:%d\n"), backendHost.c_str(), backendPort);
      } else {
        backendHost = "";
        isLoneWolf = true;
        Preferences prefs;
        prefs.begin("nodriza", false);
        prefs.putString("bHost", "");
        prefs.end();
        Serial.println(F("🐺 Modo Lobo Solitario (Reporta directamente a Nodriza Cloud)."));
      }
    }
  }
  http.end();
}

bool check_for_update() {
  if (WiFi.status() != WL_CONNECTED)
    return false;
  Serial.println("[OTA] Buscando actualizaciones en la Nodriza...");

  WiFiClientSecure client;
  client.setInsecure();
  String otaUrl =
      "https://nodrizabackend-production.up.railway.app/api/ota/check/" +
      boxSerialId;

  t_httpUpdate_return ret =
      httpUpdate.update(client, otaUrl, FIRMWARE_VERSION_CODE);

  if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("[OTA] No hay actualizaciones (304).");
  } else if (ret == HTTP_UPDATE_FAILED) {
    Serial.printf("[OTA] Error (%d): %s", httpUpdate.getLastError(),
    httpUpdate.getLastErrorString().c_str());
  }
  return false;
}

void logMessage(String level, String msg) {
  Serial.println("[" + level + "] " + msg);
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) +
                 "/sensor-data/logs";
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