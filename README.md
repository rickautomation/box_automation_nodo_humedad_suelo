# 🪴 Nodo Sensor de Humedad de Suelo (ESP32-C3)

**Versión Firmware:** `1.0.8-suelo`  
**Microcontrolador:** ESP32-C3 SuperMini  
**Código Fuente:** [`nodo_suelo_new/nodo_suelo_new.ino`](file:///Users/dariancampos/Documents/Arduino/box_automation_nodo_humedad_suelo/nodo_suelo_new/nodo_suelo_new.ino)

---

## 📌 Descripción General

El **Nodo de Suelo** es un dispositivo IoT basado en el microcontrolador **ESP32-C3 SuperMini** diseñado para monitorear hasta **4 canales independientes de humedad de suelo** mediante sensores capacitivos v1.2.

Incorpora filtrado anti-ruido por hardware con capacitores cerámicos 104, procesamiento por media recortada en software, aprovisionamiento Wi-Fi por Portal Cautivo, actualización remota de firmware (OTA) y modos de transmisión dual (Gateway Local / Cloud).

---

## 📋 Mapeo de Pines Estandarizado (ADC1)

Para evitar conflictos de hardware con el módem Wi-Fi, todos los sensores analógicos están conectados a canales del bloque **ADC1**:

| Sensor / Periférico | Etiqueta Placa | Pin GPIO ESP32-C3 | Canal ADC1 | Componente Anti-Ruido |
| :--- | :--- | :--- | :--- | :--- |
| **Sensor 1** | `A0` | **GPIO 0** | `ADC1_CH0` | Capacitor 104 (100nF) entre `GPIO 0` y `GND` |
| **Sensor 2** | `A1` | **GPIO 1** | `ADC1_CH1` | Capacitor 104 (100nF) entre `GPIO 1` y `GND` |
| **Sensor 3** | `A3` | **GPIO 3** | `ADC1_CH3` | Capacitor 104 (100nF) entre `GPIO 3` y `GND` |
| **Sensor 4** | `A4` | **GPIO 4** | `ADC1_CH4` | Capacitor 104 (100nF) entre `GPIO 4` y `GND` |
| **Reset NVS / Wi-Fi**| `BOOT` | **GPIO 9** | Input Pull-Up | Conectado a `GND` para borrar Wi-Fi |
| **Alimentación VCC** | `3.3V` | **3.3V** | Regulador Lógica | Bus de 3.3V para VCC de sensores |
| **Masa Común** | `GND` | **GND** | Bus de Tierra | Bus de Tierra unificado |

> [!NOTE]
> **GPIO 2** se evita deliberadamente por ser un *Strapping Pin* con resistencias de pull-up internas que pueden distorsionar las lecturas analógicas a 4095. Se utiliza **GPIO 3** en su lugar.

---

## ⚡ Especificación de Voltajes y Filtro Anti-Ruido Hardware

### 1. Alimentación y Niveles Eléctricos
- **Entrada de Alimentación:** `5V DC` vía puerto USB-C o pin `5V` (corriente mínima $\ge 500\text{ mA}$).
- **Voltaje de Sensores:** `3.3V DC` entregados por el regulador integrado de la placa.
- **Rango de Señal Analógica:** `0.0V` a `3.3V` (conversor ADC1 de 12 bits, resolución `0` a `4095`).

### 2. Filtro Hardware RC con Capacitores Cerámicos 104 (100nF)
Cada línea de señal analógica ($A_{\text{OUT}}$) incluye un **capacitor cerámico 104 (100nF)** conectado directamente entre la línea de señal y `GND`:
- **Función:** Forma un filtro pasa-bajos hardware que deriva a tierra el ruido electromagnético y los picos de alta frecuencia provocados por bombas de agua, relés y fuentes conmutadas.

---

## ⚙️ Algoritmo de Procesamiento y Calibración

### Pipeline de Filtrado Software (Trimmed Mean Filter)
1. **Muestreo:** Se capturan 15 muestras analógicas consecutivas por sensor con pausas de 5ms.
2. **Ordenamiento:** Arreglo ordenado mediante algoritmo *Bubble Sort*.
3. **Descarte de Extremos:** Se descarta el 25% de los valores superiores y el 25% de los inferiores.
4. **Promedio:** Se calcula la media de los 9 valores centrales (`mediasCrudas`).
5. **Mapeo de Humedad (%):**
   - `valorSeco` = `3350` ($\rightarrow 0\%$ Humedad)
   - `valorMojado` = `1500` ($\rightarrow 100\%$ Humedad)

$$\text{Humedad (\%)} = \text{constrain}\left(\text{map}(\text{MediaCruda}, 1500, 3350, 100, 0), 0, 100\right)$$

---

## 🌐 Modos de Comunicación Dual

### 1. Modo Enjambre (GreenBox Gateway Local)
Envía lecturas detalladas por canal a la pasarela local vía HTTP POST (`/sensor-data/soil`):
```json
{
  "boxSerialId": "48F6EE230524",
  "sensors": [
    { "pin": "A0 (GPIO0)", "raw": 3354, "humidity": 0 },
    { "pin": "A1 (GPIO1)", "raw": 3331, "humidity": 2 },
    { "pin": "A3 (GPIO3)", "raw": 3359, "humidity": 0 },
    { "pin": "A4 (GPIO4)", "raw": 3309, "humidity": 3 }
  ],
  "humidity": 0,
  "soilMoisture": 0,
  "soilMoistureA1": 2,
  "soilMoistureA2": 0,
  "soilMoistureA3": 3
}
```

### 2. Modo Lobo Solitario (Nodriza Cloud Directo)
Envía lecturas cifradas por HTTPS POST a la nube (`/api/sync`):
```json
{
  "carrierId": "48F6EE230524",
  "type": "ESP32",
  "readings": [
    {
      "nodeId": "48F6EE230524",
      "soilMoisture": 0,
      "soilMoistureA1": 2,
      "soilMoistureA2": 0,
      "soilMoistureA3": 3
    }
  ]
}
```

---

## 🛡️ Resiliencia y Monitoreo de Salud

- **Hardware Watchdog (WDT):** Timeout de 30s reconfigurado para ESP-IDF v5 / Arduino Core 3.x.
- **Telemetría RTC:** Registro persistente de estadísticas entre reinicios (`boot_count`, `wdt_resets`, `wifi_disconnects`, `http_errors`) enviadas periódicamente a `/api/health/metrics`.
- **Portal Cautivo AP (`NODO_SUELO_SETUP`):** Acceso en `192.168.4.1` para aprovisionamiento Wi-Fi y Host guardado en memoria **NVS Preferences**.
- **Over-The-Air (OTA):** Verificación automática periódica contra la Nodriza para actualización de firmware por `HTTPUpdate`.
