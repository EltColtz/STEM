/*
 * =========================================================================================
 *   SMART HYPER-MONITORED BIOSPHERE CABIN - FIRMWARE ARCHITECTURE
 *   Dedicated Cultivation Engine for Broccoli Microgreens (Brassica oleracea var. italica)
 * =========================================================================================
 *   Target Platform : ESP32 Dev Module (WROOM-32 / NodeMCU ESP32)
 *   Standard        : Non-Blocking Real-Time Architecture (Zero delay()), Fault-Tolerant,
 *                     Advanced Agronomic Telemetry, Cyber-Agri Glassmorphism Web Dashboard
 * =========================================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <math.h>

// =========================================================================================
// 1. HARDWARE TOPOLOGY PINOUT & CALIBRATION CONSTANTS
// =========================================================================================

// Wi-Fi Credentials
const char* WIFI_SSID     = "Tryout_SA_0509";
const char* WIFI_PASSWORD = "SAujian1234";

// Sensors
#define PIN_DHT22               14   // GPIO 14: DHT22 (Air Temperature & Relative Humidity RH)
#define PIN_SOIL_ADC            34   // GPIO 34 (ADC1): Capacitive Soil Moisture Sensor

#define DHTTYPE                 DHT22

// 6-Channel Relay Module (Active-LOW Trigger)
#define RELAY_TRIGGER_ON        LOW
#define RELAY_TRIGGER_OFF       HIGH

#define PIN_RELAY_PELTIER       19   // CH1: Peltier Thermoelectric Module 12V (Cooling)
#define PIN_RELAY_FAN_HEATSINK  18   // CH2: External Heatsink Fan 12V (Interlocked with CH1)
#define PIN_RELAY_BLOWER        5    // CH3: DC Blower 12V (Turbulence & Moisture Evacuation)
#define PIN_RELAY_GROWLIGHT     17   // CH4: Full-Spectrum LED Grow Light
#define PIN_RELAY_SPRAY_T1      16   // CH5: Nano Ultrasonic Spray Tank 1 (Raw Water)
#define PIN_RELAY_SPRAY_T2      4    // CH6: Nano Ultrasonic Spray Tank 2 (Micro Nutrients)

// Mechanical Valve Servomotors
#define PIN_SERVO_VALVE_T1      25   // GPIO 25: Tank 1 Flow Valve (0° Closed, 90° Open)
#define PIN_SERVO_VALVE_T2      26   // GPIO 26: Tank 2 Flow Valve (0° Closed, 90° Open)

// Visual Indicator
#define PIN_STATUS_LED          27   // GPIO 27: System Heartbeat / Health Indicator LED

// Capacitive Soil Moisture Sensor ADC Calibration Constants
// ESP32 ADC: 12-bit (0 - 4095). Sensor output is inversely proportional to moisture.
#define SOIL_DRY_RAW            3200 // Raw ADC in ambient dry air (0% Moisture)
#define SOIL_WET_RAW            1400 // Raw ADC fully submerged in water (100% Moisture)

// Ultrasonic Mist Flow Rate Calibration (approx. 48 mL/hour = 0.8 mL/second)
#define SPRAY_FLOW_RATE_ML_S    0.8f
#define TANK_MAX_CAPACITY_ML    1000.0f

// =========================================================================================
// 2. AGRONOMIC TELEMETRY & SYSTEM STATE STRUCTURES
// =========================================================================================

enum GrowthPhase {
  PHASE_GERMINATION_BLACKOUT = 1, // Days 1-3: Blackout, high RH (60-70%), pure Tank 1 (Raw)
  PHASE_AUTOTROPHIC_LIGHT    = 2  // Days 4-10: 16h/8h Photoperiod, VPD 0.6-0.8 kPa, Tank 2 (Nutrients)
};

enum ClimateStatus {
  STATUS_OPTIMAL,
  STATUS_WASPADA,
  STATUS_KRITIS,
  STATUS_SAFEMODE
};

struct AgronomicMetrics {
  float temperature;        // °C
  float humidity;           // % RH
  float soilMoisture;       // %
  float vpSat;              // Saturated Vapor Pressure (kPa)
  float vpAct;              // Actual Vapor Pressure (kPa)
  float vpd;                // Vapor Pressure Deficit (kPa)
  float plantComfortScore;  // Plant Comfort Score (0 - 100%)
  ClimateStatus status;
  bool sensorFault;
};

struct SystemState {
  // Operational Modes
  bool autoMode;            // true = AUTOMATIC CLIMATE CONTROL, false = MANUAL OVERRIDE
  GrowthPhase phase;        // Current Growth Phase
  uint8_t dayCounter;       // Days in Cultivation (1 - 10)
  uint8_t activeTank;       // 1 = Tank 1, 2 = Tank 2

  // Actuator Relay States (true = ON, false = OFF)
  bool peltier;
  bool heatsinkFan;
  bool blower;
  bool growLight;
  bool sprayT1;
  bool sprayT2;

  // Servomotor Valve Angles (0° - 90°)
  int valve1Angle;
  int valve2Angle;

  // Tank Volumetrics (mL)
  float tank1VolumeMl;
  float tank2VolumeMl;

  // Anti-Fungal Purge Machine
  bool purgeActive;
  uint32_t purgeStartTime;
  uint32_t lastPurgeCheckTime;

  // Emergency Flush State Machine
  bool emergencyFlushActive;
  uint32_t emergencyFlushStartTime;

  // Automated Spray Pulse Machine
  bool autoSprayActive;
  uint32_t autoSprayStartTime;
  uint32_t lastAutoSprayTime;
};

// Circular Buffers for Telemetry History (Last 20 Points)
#define HISTORY_LENGTH 20
float tempHistory[HISTORY_LENGTH];
float rhHistory[HISTORY_LENGTH];
uint8_t historyHead = 0;
uint8_t historyCount = 0;

// System Log Console Buffer (Last 5 Timestamped Events)
#define LOG_CAPACITY 5
struct LogEntry {
  char timestamp[12];
  char message[64];
};
LogEntry logBuffer[LOG_CAPACITY];
uint8_t logHead = 0;
uint8_t logCount = 0;

// Global Instances
DHT dht(PIN_DHT22, DHTTYPE);
Servo servoValve1;
Servo servoValve2;
WebServer server(80);

AgronomicMetrics metrics;
SystemState state;

// Non-Blocking Timing Trackers
uint32_t lastSensorPollTime    = 0;
uint32_t lastClimateLoopTime   = 0;
uint32_t lastLedBlinkTime      = 0;
uint32_t lastVolumeUpdateTime  = 0;
uint32_t lastDayIncrementTime  = 0;

bool ledState = false;

// =========================================================================================
// 3. LOGGING & TIME UTILITIES
// =========================================================================================

void getFormattedTimestamp(char* buffer, size_t maxLen) {
  uint32_t sec = millis() / 1000;
  uint32_t hrs = (sec / 3600);
  uint32_t mins = (sec % 3600) / 60;
  uint32_t secs = sec % 60;
  snprintf(buffer, maxLen, "%02u:%02u:%02u", hrs, mins, secs);
}

void addSystemLog(const char* msg) {
  char ts[12];
  getFormattedTimestamp(ts, sizeof(ts));
  
  strncpy(logBuffer[logHead].timestamp, ts, sizeof(logBuffer[logHead].timestamp) - 1);
  logBuffer[logHead].timestamp[sizeof(logBuffer[logHead].timestamp) - 1] = '\0';
  
  strncpy(logBuffer[logHead].message, msg, sizeof(logBuffer[logHead].message) - 1);
  logBuffer[logHead].message[sizeof(logBuffer[logHead].message) - 1] = '\0';

  Serial.printf("[%s] %s\n", ts, msg);

  logHead = (logHead + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) {
    logCount++;
  }
}

// =========================================================================================
// 4. ADVANCED AGRONOMIC METRICS & COMPUTATION (TETENS VPD & PCS)
// =========================================================================================

/**
 * @brief Computes Tetens Equation Saturated Vapor Pressure (VPsat)
 * Equation: VPsat = 0.61078 * exp((17.27 * T) / (T + 237.3))  [kPa]
 */
float calculateVpSat(float T) {
  return 0.61078f * expf((17.27f * T) / (T + 237.3f));
}

/**
 * @brief Computes Actual Vapor Pressure (VPact)
 * Equation: VPact = VPsat * (RH / 100.0)  [kPa]
 */
float calculateVpAct(float vpSat, float rh) {
  return vpSat * (rh / 100.0f);
}

/**
 * @brief Computes Vapor Pressure Deficit (VPD)
 * Equation: VPD = VPsat - VPact  [kPa]
 * Optimal for Broccoli Microgreens: 0.40 - 0.80 kPa
 */
float calculateVPD(float T, float rh, float &outVpSat, float &outVpAct) {
  outVpSat = calculateVpSat(T);
  outVpAct = calculateVpAct(outVpSat, rh);
  float vpd = outVpSat - outVpAct;
  return (vpd < 0.0f) ? 0.0f : vpd;
}

/**
 * @brief Formulates Plant Comfort Score (PCS) 0 - 100%
 * Based on weighted deviations from optimal broccoli microgreens criteria:
 * - Temperature Target: 20.0°C (Tolerance: 18.0 - 22.0°C; Pathogenic risk > 24.0°C) [Weight: 40%]
 * - Relative Humidity Target: 57.5% (Tolerance: 50.0 - 65.0%) [Weight: 30%]
 * - Target VPD: 0.60 kPa (Tolerance: 0.40 - 0.80 kPa) [Weight: 30%]
 */
float calculatePlantComfortScore(float T, float rh, float vpd) {
  // 1. Temperature Deviation Score (Weight: 40%)
  float tScore = 1.0f;
  float tDev = fabsf(T - 20.0f);
  if (tDev <= 2.0f) {
    tScore = 1.0f - (tDev / 2.0f) * 0.15f; // Within 18-22°C: 85% - 100%
  } else if (T > 24.0f) {
    // Critical Pythium root rot threshold
    float severeDev = T - 24.0f;
    tScore = 0.4f - (severeDev * 0.15f);
    if (tScore < 0.0f) tScore = 0.0f;
  } else {
    // Between 22-24°C or < 18°C
    tScore = 0.85f - ((tDev - 2.0f) / 2.0f) * 0.45f;
    if (tScore < 0.1f) tScore = 0.1f;
  }

  // 2. Relative Humidity Deviation Score (Weight: 30%)
  float rhScore = 1.0f;
  float rhDev = fabsf(rh - 57.5f);
  if (rhDev <= 7.5f) {
    rhScore = 1.0f - (rhDev / 7.5f) * 0.15f; // Within 50-65%: 85% - 100%
  } else {
    rhScore = 0.85f - ((rhDev - 7.5f) / 20.0f) * 0.70f;
    if (rhScore < 0.0f) rhScore = 0.0f;
  }

  // 3. Vapor Pressure Deficit Deviation Score (Weight: 30%)
  float vpdScore = 1.0f;
  float vpdDev = fabsf(vpd - 0.60f);
  if (vpdDev <= 0.20f) {
    vpdScore = 1.0f - (vpdDev / 0.20f) * 0.15f; // Within 0.4 - 0.8 kPa: 85% - 100%
  } else {
    vpdScore = 0.85f - ((vpdDev - 0.20f) / 0.50f) * 0.75f;
    if (vpdScore < 0.0f) vpdScore = 0.0f;
  }

  float totalScore = (tScore * 0.40f + rhScore * 0.30f + vpdScore * 0.30f) * 100.0f;
  return constrain(totalScore, 0.0f, 100.0f);
}

// =========================================================================================
// 5. HARDWARE DRIVERS, INTERLOCKS & SAFETY MECHANISMS
// =========================================================================================

void rawRelayWrite(uint8_t pin, bool active) {
  digitalWrite(pin, active ? RELAY_TRIGGER_ON : RELAY_TRIGGER_OFF);
}

/**
 * @brief Enforces Thermal Safety Interlock on CH1 (Peltier) and CH2 (Heatsink Fan)
 * Specification: Peltier (CH1) is strictly FORBIDDEN to turn ON without CH2 active simultaneously!
 */
void applyPeltierInterlock(bool enablePeltier) {
  if (enablePeltier) {
    // CH2 Heatsink Fan MUST activate first/simultaneously
    rawRelayWrite(PIN_RELAY_FAN_HEATSINK, true);
    state.heatsinkFan = true;

    rawRelayWrite(PIN_RELAY_PELTIER, true);
    state.peltier = true;
  } else {
    // Turn off Peltier immediately
    rawRelayWrite(PIN_RELAY_PELTIER, false);
    state.peltier = false;
    // Heatsink Fan can be shut off or maintained as needed
  }
}

/**
 * @brief Controls Heatsink Fan with safety interlock check
 */
void applyHeatsinkFan(bool enableFan) {
  if (!enableFan && state.peltier) {
    // If Peltier is currently active, fan CANNOT be turned off! Safety auto-shutdown.
    addSystemLog("SAFETY INTERLOCK: Peltier forced OFF due to Fan deactivation!");
    applyPeltierInterlock(false);
  }
  rawRelayWrite(PIN_RELAY_FAN_HEATSINK, enableFan);
  state.heatsinkFan = enableFan;
}

/**
 * @brief Controls Air Blower (CH3)
 */
void applyBlower(bool enableBlower) {
  rawRelayWrite(PIN_RELAY_BLOWER, enableBlower);
  state.blower = enableBlower;
}

/**
 * @brief Controls LED Grow Light (CH4)
 */
void applyGrowLight(bool enableLight) {
  rawRelayWrite(PIN_RELAY_GROWLIGHT, enableLight);
  state.growLight = enableLight;
}

/**
 * @brief Anti-Mix Dual-Tank Valve & Spray Control Logic
 * Specification: Opens designated servo valve (90°) ONLY when spray line is active;
 * locks valve to 0° when spray stops to prevent cross-contamination.
 */
void applySprayAndValves(bool spray1, bool spray2) {
  // Tank 1 Control
  if (spray1) {
    servoValve1.write(90);
    state.valve1Angle = 90;
    rawRelayWrite(PIN_RELAY_SPRAY_T1, true);
    state.sprayT1 = true;
  } else {
    rawRelayWrite(PIN_RELAY_SPRAY_T1, false);
    state.sprayT1 = false;
    servoValve1.write(0);
    state.valve1Angle = 0;
  }

  // Tank 2 Control
  if (spray2) {
    servoValve2.write(90);
    state.valve2Angle = 90;
    rawRelayWrite(PIN_RELAY_SPRAY_T2, true);
    state.sprayT2 = true;
  } else {
    rawRelayWrite(PIN_RELAY_SPRAY_T2, false);
    state.sprayT2 = false;
    servoValve2.write(0);
    state.valve2Angle = 0;
  }
}

/**
 * @brief Robust Safe-Mode Fallback on Sensor Read Fault (NaN)
 */
void triggerSafeModeFallback() {
  metrics.status = STATUS_SAFEMODE;
  metrics.sensorFault = true;

  // Safe mode protocol: Peltier OFF, Blower ON (prevent stagnant buildup), Sprays OFF, Valves 0°
  applyPeltierInterlock(false);
  applyBlower(true);
  applySprayAndValves(false, false);

  addSystemLog("CRITICAL: Sensor read NaN! System reverted to SAFE-MODE.");
}

// =========================================================================================
// 6. SENSOR ACQUISITION & TELEMETRY ENGINE
// =========================================================================================

void pollSensors() {
  float rawT = dht.readTemperature();
  float rawRh = dht.readHumidity();

  // Handle DHT22 read failure
  if (isnan(rawT) || isnan(rawRh)) {
    triggerSafeModeFallback();
    return;
  }

  metrics.sensorFault = false;
  metrics.temperature = rawT;
  metrics.humidity = rawRh;

  // Capacitive Soil Moisture Sensor (ADC1, GPIO 34)
  int rawSoilAdc = analogRead(PIN_SOIL_ADC);
  // Map inverted ADC values: SOIL_DRY_RAW (3200) -> 0%, SOIL_WET_RAW (1400) -> 100%
  float soilPct = (float)(SOIL_DRY_RAW - rawSoilAdc) * 100.0f / (float)(SOIL_DRY_RAW - SOIL_WET_RAW);
  metrics.soilMoisture = constrain(soilPct, 0.0f, 100.0f);

  // Compute Advanced Agronomic Metrics
  metrics.vpd = calculateVPD(metrics.temperature, metrics.humidity, metrics.vpSat, metrics.vpAct);
  metrics.plantComfortScore = calculatePlantComfortScore(metrics.temperature, metrics.humidity, metrics.vpd);

  // Climate Status Evaluation
  if (metrics.temperature > 24.0f || metrics.humidity > 75.0f || metrics.soilMoisture < 35.0f) {
    metrics.status = STATUS_KRITIS;
  } else if (metrics.temperature > 22.0f || metrics.temperature < 18.0f ||
             metrics.humidity > 65.0f || metrics.humidity < 50.0f ||
             metrics.soilMoisture < 45.0f || metrics.soilMoisture > 70.0f ||
             metrics.vpd < 0.40f || metrics.vpd > 0.80f) {
    metrics.status = STATUS_WASPADA;
  } else {
    metrics.status = STATUS_OPTIMAL;
  }

  // Push to circular history buffer
  tempHistory[historyHead] = metrics.temperature;
  rhHistory[historyHead] = metrics.humidity;
  historyHead = (historyHead + 1) % HISTORY_LENGTH;
  if (historyCount < HISTORY_LENGTH) {
    historyCount++;
  }
}

// =========================================================================================
// 7. CLOSED-LOOP CLIMATE CONTROL & GROWTH SEQUENCER
// =========================================================================================

void executeAutomatedClimateControl() {
  if (!state.autoMode || metrics.sensorFault) {
    return; // Manual mode or fault condition suppresses automated control
  }

  // --- Growth Phase Sequencer: Grow Light Scheduling ---
  if (state.phase == PHASE_GERMINATION_BLACKOUT) {
    // Days 1-3: Blackout Phase (Grow Light MUST remain OFF)
    applyGrowLight(false);
  } else {
    // Days 4-10: Autotrophic Light Phase (16 hours ON / 8 hours OFF photoperiod)
    uint32_t dayMillis = millis() % (24UL * 3600UL * 1000UL);
    bool isLightCycle = (dayMillis < (16UL * 3600UL * 1000UL));
    applyGrowLight(isLightCycle);
  }

  // --- Temperature Regulation (Peltier & Interlocked Heatsink Fan) ---
  // Target: 18.0°C - 22.0°C (Critical > 24.0°C)
  if (metrics.temperature > 22.0f) {
    if (!state.peltier) {
      applyPeltierInterlock(true);
      addSystemLog("Cooling activated: Temp > 22.0C");
    }
  } else if (metrics.temperature <= 19.5f) {
    if (state.peltier) {
      applyPeltierInterlock(false);
      applyHeatsinkFan(false);
      addSystemLog("Cooling target reached: Peltier OFF");
    }
  }

  // --- Air Circulation & Humidity Evacuation (Blower DC 12V) ---
  // If not currently in dedicated purge cycle, evaluate standard humidity threshold
  if (!state.purgeActive) {
    if (metrics.humidity > 65.0f) {
      if (!state.blower) {
        applyBlower(true);
        addSystemLog("Blower ON: Humidity > 65% (Evacuating moisture)");
      }
    } else if (metrics.humidity <= 60.0f && !state.peltier) {
      if (state.blower) {
        applyBlower(false);
        addSystemLog("Blower OFF: Air humidity normalized");
      }
    }
  }

  // --- Dual-Tank Nano Spray Pulsing ---
  // In Germination: High RH (60-70%), pure Tank 1 (Demineralized raw water)
  // In Autotrophic: VPD target 0.6 - 0.8 kPa, Tank 2 (Micro nutrients)
  uint32_t now = millis();
  if (!state.emergencyFlushActive && !state.autoSprayActive) {
    bool triggerSpray = false;
    uint8_t targetTank = state.activeTank;

    if (state.phase == PHASE_GERMINATION_BLACKOUT) {
      targetTank = 1; // Pure Tank 1
      if (metrics.soilMoisture < 50.0f || metrics.humidity < 60.0f) {
        triggerSpray = true;
      }
    } else {
      // Autotrophic phase
      if (metrics.soilMoisture < 48.0f || metrics.vpd > 0.80f) {
        triggerSpray = true;
      }
    }

    // Interval between automatic spray pulses: 5 minutes (300,000 ms)
    if (triggerSpray && (now - state.lastAutoSprayTime >= 300000UL)) {
      state.autoSprayActive = true;
      state.autoSprayStartTime = now;
      state.lastAutoSprayTime = now;

      if (targetTank == 1) {
        applySprayAndValves(true, false);
        addSystemLog("Auto-Spray Pulse: Tank 1 (Raw Water) 5s");
      } else {
        applySprayAndValves(false, true);
        addSystemLog("Auto-Spray Pulse: Tank 2 (Nutrients) 5s");
      }
    }
  }

  // Check completion of automated 5s spray pulse
  if (state.autoSprayActive && (now - state.autoSprayStartTime >= 5000UL)) {
    state.autoSprayActive = false;
    applySprayAndValves(false, false);
  }
}

/**
 * @brief Anti-Fungal Purge Cycle State Machine
 * Specification: Every 30 minutes, if air humidity > 65%, run Blower (CH3)
 * for 45 seconds to break stagnant boundary layer on leaves.
 */
void processAntiFungalPurge() {
  uint32_t now = millis();

  // Check 30-minute interval (1,800,000 ms)
  if (!state.purgeActive && (now - state.lastPurgeCheckTime >= 1800000UL)) {
    state.lastPurgeCheckTime = now;
    if (metrics.humidity > 65.0f) {
      state.purgeActive = true;
      state.purgeStartTime = now;
      applyBlower(true);
      addSystemLog("Anti-Fungal Purge: Blower running 45s (RH > 65%)");
    }
  }

  // Manage 45-second active purge duration
  if (state.purgeActive && (now - state.purgeStartTime >= 45000UL)) {
    state.purgeActive = false;
    if (!state.autoMode || metrics.humidity <= 65.0f) {
      applyBlower(false);
    }
    addSystemLog("Anti-Fungal Purge cycle completed.");
  }
}

/**
 * @brief Emergency Flush State Machine (5 seconds spray from Tank 1)
 */
void processEmergencyFlush() {
  if (state.emergencyFlushActive) {
    if (millis() - state.emergencyFlushStartTime >= 5000UL) {
      state.emergencyFlushActive = false;
      applySprayAndValves(false, false);
      addSystemLog("Emergency Flush cycle finished.");
    }
  }
}

/**
 * @brief Tank Reservoir Volumetric Depletion Tracker
 */
void updateTankVolumes() {
  uint32_t now = millis();
  float dtSeconds = (float)(now - lastVolumeUpdateTime) / 1000.0f;
  lastVolumeUpdateTime = now;

  if (state.sprayT1) {
    state.tank1VolumeMl -= (SPRAY_FLOW_RATE_ML_S * dtSeconds);
    if (state.tank1VolumeMl < 0.0f) state.tank1VolumeMl = 0.0f;
  }
  if (state.sprayT2) {
    state.tank2VolumeMl -= (SPRAY_FLOW_RATE_ML_S * dtSeconds);
    if (state.tank2VolumeMl < 0.0f) state.tank2VolumeMl = 0.0f;
  }
}

/**
 * @brief Status LED Heartbeat / Fault Signaler
 */
void processStatusLed() {
  uint32_t now = millis();
  uint32_t blinkInterval = 1000; // Normal: 1s

  if (metrics.status == STATUS_SAFEMODE) {
    blinkInterval = 100; // Ultra-fast blink for fault
  } else if (metrics.status == STATUS_KRITIS) {
    blinkInterval = 250; // Fast blink for critical
  } else if (metrics.status == STATUS_WASPADA) {
    blinkInterval = 500; // Moderate blink for warning
  }

  if (now - lastLedBlinkTime >= blinkInterval) {
    lastLedBlinkTime = now;
    ledState = !ledState;
    digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
  }
}

// =========================================================================================
// 8. HIGH-PERFORMANCE EMBEDDED WEB DASHBOARD (HTML5, CSS3, JAVASCRIPT)
// =========================================================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Biosphere Cabin | Broccoli Microgreens</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;600;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg-dark: #0a0e17;
      --card-bg: rgba(14, 21, 37, 0.72);
      --card-border: rgba(0, 255, 157, 0.16);
      --neon-mint: #00ff9d;
      --neon-blue: #00b4d8;
      --neon-amber: #ffb703;
      --neon-red: #ff0055;
      --text-main: #f8fafc;
      --text-muted: #94a3b8;
      --glass-blur: blur(16px);
      --glow-mint: 0 0 20px rgba(0, 255, 157, 0.25);
      --glow-blue: 0 0 20px rgba(0, 180, 216, 0.25);
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background-color: var(--bg-dark);
      background-image:
        radial-gradient(circle at 15% 15%, rgba(0, 255, 157, 0.07) 0%, transparent 40%),
        radial-gradient(circle at 85% 85%, rgba(0, 180, 216, 0.08) 0%, transparent 45%);
      color: var(--text-main);
      font-family: 'Inter', -apple-system, sans-serif;
      min-height: 100vh;
      padding: 24px;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 24px;
      padding: 18px 24px;
      background: var(--card-bg);
      backdrop-filter: var(--glass-blur);
      border: 1px solid var(--card-border);
      border-radius: 18px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.5);
    }
    .title-group h1 {
      font-size: 1.5rem;
      font-weight: 700;
      letter-spacing: -0.5px;
      background: linear-gradient(135deg, #ffffff 40%, var(--neon-mint));
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      margin-bottom: 4px;
    }
    .title-group p {
      font-size: 0.85rem;
      color: var(--text-muted);
      font-style: italic;
    }
    .header-badges {
      display: flex;
      gap: 12px;
      align-items: center;
    }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 14px;
      border-radius: 9999px;
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.78rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      border: 1px solid currentColor;
    }
    .badge-optimal { color: var(--neon-mint); background: rgba(0,255,157,0.1); border-color: rgba(0,255,157,0.3); }
    .badge-waspada { color: var(--neon-amber); background: rgba(255,183,3,0.1); border-color: rgba(255,183,3,0.3); }
    .badge-kritis  { color: var(--neon-red); background: rgba(255,0,85,0.1); border-color: rgba(255,0,85,0.3); animation: pulse-red 1.5s infinite; }
    .badge-safe    { color: #ff00ea; background: rgba(255,0,234,0.1); border-color: rgba(255,0,234,0.3); }
    .pulse-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: currentColor;
      box-shadow: 0 0 8px currentColor;
    }
    @keyframes pulse-red {
      0%, 100% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.75; transform: scale(0.97); }
    }
    .telemetry-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
      gap: 20px;
      margin-bottom: 24px;
    }
    .card {
      background: var(--card-bg);
      backdrop-filter: var(--glass-blur);
      border: 1px solid var(--card-border);
      border-radius: 18px;
      padding: 20px;
      position: relative;
      overflow: hidden;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.4);
      transition: transform 0.2s, border-color 0.2s;
    }
    .card:hover {
      transform: translateY(-2px);
      border-color: rgba(0, 255, 157, 0.35);
    }
    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      margin-bottom: 12px;
    }
    .card-label {
      font-size: 0.8rem;
      font-weight: 600;
      color: var(--text-muted);
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    .card-target {
      font-size: 0.72rem;
      font-family: 'JetBrains Mono', monospace;
      color: var(--text-muted);
    }
    .card-value {
      font-family: 'JetBrains Mono', monospace;
      font-size: 2.3rem;
      font-weight: 700;
      letter-spacing: -1px;
      color: var(--text-main);
      margin-bottom: 10px;
      display: flex;
      align-items: baseline;
      gap: 6px;
    }
    .card-unit {
      font-size: 1rem;
      font-weight: 500;
      color: var(--text-muted);
    }
    .instruments-grid {
      display: grid;
      grid-template-columns: 1fr 1fr 1fr;
      gap: 20px;
      margin-bottom: 24px;
    }
    @media (max-width: 900px) {
      .instruments-grid { grid-template-columns: 1fr; }
    }
    .inst-title {
      font-size: 0.85rem;
      font-weight: 600;
      text-transform: uppercase;
      color: var(--text-muted);
      margin-bottom: 16px;
      display: flex;
      justify-content: space-between;
    }
    .pcs-meter {
      height: 12px;
      background: rgba(255, 255, 255, 0.08);
      border-radius: 999px;
      overflow: hidden;
      margin: 12px 0;
      position: relative;
    }
    .pcs-fill {
      height: 100%;
      background: linear-gradient(90deg, var(--neon-amber), var(--neon-mint));
      border-radius: 999px;
      transition: width 0.4s ease;
      box-shadow: 0 0 10px rgba(0,255,157,0.5);
    }
    .tank-row {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .tank-item {
      background: rgba(0,0,0,0.25);
      border: 1px solid rgba(255,255,255,0.06);
      border-radius: 12px;
      padding: 12px;
    }
    .tank-header {
      display: flex;
      justify-content: space-between;
      font-size: 0.8rem;
      margin-bottom: 6px;
    }
    .tank-bar {
      height: 8px;
      background: rgba(255,255,255,0.08);
      border-radius: 999px;
      overflow: hidden;
    }
    .tank-fill-1 { height: 100%; background: var(--neon-blue); transition: width 0.4s; }
    .tank-fill-2 { height: 100%; background: var(--neon-mint); transition: width 0.4s; }
    .matrix-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
      gap: 14px;
      margin-top: 10px;
    }
    .matrix-node {
      background: rgba(0,0,0,0.3);
      border: 1px solid rgba(255,255,255,0.07);
      border-radius: 12px;
      padding: 12px;
      text-align: center;
      transition: all 0.2s;
    }
    .matrix-node.active {
      border-color: var(--neon-mint);
      background: rgba(0, 255, 157, 0.08);
      box-shadow: var(--glow-mint);
    }
    .matrix-node.active-fan {
      border-color: var(--neon-blue);
      background: rgba(0, 180, 216, 0.08);
      box-shadow: var(--glow-blue);
    }
    .matrix-name {
      font-size: 0.72rem;
      font-weight: 600;
      color: var(--text-muted);
      margin-bottom: 6px;
    }
    .matrix-led {
      width: 12px;
      height: 12px;
      border-radius: 50%;
      background: rgba(255,255,255,0.15);
      margin: 0 auto 6px;
      transition: all 0.3s;
    }
    .matrix-node.active .matrix-led {
      background: var(--neon-mint);
      box-shadow: 0 0 10px var(--neon-mint);
    }
    .matrix-node.active-fan .matrix-led {
      background: var(--neon-blue);
      box-shadow: 0 0 10px var(--neon-blue);
    }
    .matrix-val {
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.78rem;
      font-weight: 700;
    }
    .chart-section {
      margin-bottom: 24px;
    }
    .chart-card {
      background: var(--card-bg);
      backdrop-filter: var(--glass-blur);
      border: 1px solid var(--card-border);
      border-radius: 18px;
      padding: 20px;
    }
    .chart-svg-container {
      width: 100%;
      height: 190px;
    }
    .controls-grid {
      display: grid;
      grid-template-columns: 2fr 1fr;
      gap: 20px;
      margin-bottom: 24px;
    }
    @media (max-width: 900px) {
      .controls-grid { grid-template-columns: 1fr; }
    }
    .btn {
      background: rgba(255,255,255,0.06);
      color: var(--text-main);
      border: 1px solid rgba(255,255,255,0.15);
      border-radius: 10px;
      padding: 10px 16px;
      font-size: 0.82rem;
      font-weight: 600;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      transition: all 0.2s;
    }
    .btn:hover {
      background: rgba(255,255,255,0.12);
      border-color: var(--neon-mint);
    }
    .btn-mint {
      background: rgba(0,255,157,0.15);
      border-color: var(--neon-mint);
      color: var(--neon-mint);
    }
    .btn-mint:hover {
      background: var(--neon-mint);
      color: #000;
      box-shadow: var(--glow-mint);
    }
    .btn-red {
      background: rgba(255,0,85,0.18);
      border-color: var(--neon-red);
      color: var(--neon-red);
    }
    .btn-red:hover {
      background: var(--neon-red);
      color: #fff;
      box-shadow: 0 0 15px rgba(255,0,85,0.6);
    }
    .toggle-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 10px 0;
      border-bottom: 1px solid rgba(255,255,255,0.06);
    }
    .toggle-label {
      font-size: 0.85rem;
      font-weight: 500;
    }
    .terminal-card {
      background: #050811;
      border: 1px solid rgba(0, 255, 157, 0.2);
      border-radius: 18px;
      padding: 18px;
      font-family: 'JetBrains Mono', monospace;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.6);
    }
    .terminal-header {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 12px;
      padding-bottom: 8px;
      border-bottom: 1px solid rgba(255,255,255,0.08);
      font-size: 0.75rem;
      color: var(--text-muted);
      text-transform: uppercase;
    }
    .term-dot { width: 10px; height: 10px; border-radius: 50%; }
    .term-red { background: #ff5f56; }
    .term-yellow { background: #ffbd2e; }
    .term-green { background: #27c93f; }
    .terminal-body {
      display: flex;
      flex-direction: column;
      gap: 6px;
      font-size: 0.82rem;
      min-height: 100px;
    }
    .term-log {
      display: flex;
      gap: 10px;
    }
    .term-time { color: var(--neon-blue); }
    .term-msg { color: #d1d5db; }
  </style>
</head>
<body>

  <!-- Header -->
  <div class="header">
    <div class="title-group">
      <h1>SMART HYPER-MONITORED BIOSPHERE CABIN</h1>
      <p>Broccoli Microgreens (Brassica oleracea var. italica) Cultivation Engine</p>
    </div>
    <div class="header-badges">
      <div id="badgeStatus" class="badge badge-optimal">
        <span class="pulse-dot"></span>
        <span id="badgeStatusText">OPTIMAL</span>
      </div>
      <div class="badge" style="color: var(--neon-blue); border-color: rgba(0,180,216,0.3);">
        IP: <span id="valIp" style="margin-left:4px;">ESP32</span>
      </div>
      <div class="badge" style="color: var(--text-muted); border-color: rgba(255,255,255,0.15);">
        UPTIME: <span id="valUptime" style="margin-left:4px;">00:00:00</span>
      </div>
    </div>
  </div>

  <!-- Main Telemetry Cards (4 Metrics) -->
  <div class="telemetry-grid">
    <div class="card">
      <div class="card-header">
        <span class="card-label">Air Temperature</span>
        <span class="card-target">Tgt: 18.0 - 22.0°C</span>
      </div>
      <div class="card-value">
        <span id="valTemp">--.-</span>
        <span class="card-unit">°C</span>
      </div>
      <div id="badgeTemp" class="badge badge-optimal">NORMAL</div>
    </div>

    <div class="card">
      <div class="card-header">
        <span class="card-label">Relative Humidity</span>
        <span class="card-target">Tgt: 50.0 - 65.0%</span>
      </div>
      <div class="card-value">
        <span id="valRh">--.-</span>
        <span class="card-unit">% RH</span>
      </div>
      <div id="badgeRh" class="badge badge-optimal">NORMAL</div>
    </div>

    <div class="card">
      <div class="card-header">
        <span class="card-label">Soil Moisture</span>
        <span class="card-target">Tgt: 45.0 - 70.0%</span>
      </div>
      <div class="card-value">
        <span id="valSoil">--.-</span>
        <span class="card-unit">%</span>
      </div>
      <div id="badgeSoil" class="badge badge-optimal">NORMAL</div>
    </div>

    <div class="card">
      <div class="card-header">
        <span class="card-label">Vapor Pressure Deficit</span>
        <span class="card-target">Tgt: 0.40 - 0.80 kPa</span>
      </div>
      <div class="card-value">
        <span id="valVpd">-.--</span>
        <span class="card-unit">kPa</span>
      </div>
      <div id="badgeVpd" class="badge badge-optimal">BALANCED</div>
    </div>
  </div>

  <!-- Secondary Metrics & Instruments Grid -->
  <div class="instruments-grid">
    <!-- Plant Comfort Score (PCS) -->
    <div class="card">
      <div class="inst-title">
        <span>Plant Comfort Score (PCS)</span>
        <span id="valPcsText" style="color:var(--neon-mint);">--%</span>
      </div>
      <div class="card-value" style="font-size:2.8rem; margin-bottom:4px;">
        <span id="valPcs">--</span><span class="card-unit">%</span>
      </div>
      <div class="pcs-meter">
        <div id="pcsBar" class="pcs-fill" style="width:0%;"></div>
      </div>
      <div style="font-size:0.75rem; color:var(--text-muted); display:flex; justify-content:space-between;">
        <span>Pathogen Risk: High</span>
        <span>Ideal Homeostasis</span>
      </div>
    </div>

    <!-- Dual-Tank Reservoir Volumetrics -->
    <div class="card">
      <div class="inst-title">
        <span>Dual-Tank Reservoirs</span>
        <span style="font-family:'JetBrains Mono'; font-size:0.75rem;">MIST CONSUMPTION</span>
      </div>
      <div class="tank-row">
        <div class="tank-item">
          <div class="tank-header">
            <span>Tank 1 (Demineralized Air Baku)</span>
            <span id="valTank1Text" style="font-family:'JetBrains Mono'; color:var(--neon-blue);">1000 mL</span>
          </div>
          <div class="tank-bar">
            <div id="tank1Bar" class="tank-fill-1" style="width:100%;"></div>
          </div>
        </div>
        <div class="tank-item">
          <div class="tank-header">
            <span>Tank 2 (Cheated Booster / Nutrisi)</span>
            <span id="valTank2Text" style="font-family:'JetBrains Mono'; color:var(--neon-mint);">1000 mL</span>
          </div>
          <div class="tank-bar">
            <div id="tank2Bar" class="tank-fill-2" style="width:100%;"></div>
          </div>
        </div>
      </div>
      <div style="margin-top:12px; display:flex; gap:8px;">
        <button class="btn" style="flex:1; padding:6px 10px; font-size:0.75rem;" onclick="sendControl('action=refill&tank=1')">Refill T1</button>
        <button class="btn" style="flex:1; padding:6px 10px; font-size:0.75rem;" onclick="sendControl('action=refill&tank=2')">Refill T2</button>
      </div>
    </div>

    <!-- Growth Phase Sequencer Tracker -->
    <div class="card">
      <div class="inst-title">
        <span>Growth Phase Sequencer</span>
        <span id="valDayBadge" class="badge badge-optimal" style="padding:2px 8px;">DAY 1 / 10</span>
      </div>
      <div style="margin-bottom:12px;">
        <div style="font-size:0.75rem; color:var(--text-muted); margin-bottom:4px;">CURRENT REGIME</div>
        <div id="valPhaseName" style="font-size:1.05rem; font-weight:700; color:var(--neon-mint);">Germination Blackout</div>
        <div id="valPhaseDesc" style="font-size:0.78rem; color:var(--text-muted); margin-top:4px;">
          Grow Light OFF • Target RH 60-70% • Pure Tank 1 Spray
        </div>
      </div>
      <div class="pcs-meter" style="height:8px;">
        <div id="phaseProgress" class="pcs-fill" style="width:10%; background:var(--neon-blue);"></div>
      </div>
      <div style="display:flex; justify-content:space-between; font-size:0.75rem; color:var(--text-muted);">
        <span>Day 1 (Germination)</span>
        <span>Day 10 (Harvest)</span>
      </div>
    </div>
  </div>

  <!-- 6-Channel Relay & Valve Servomotor Matrix -->
  <div class="card" style="margin-bottom:24px;">
    <div class="inst-title" style="margin-bottom:8px;">
      <span>Hardware Matrix: 6-Channel Relay & Servo Flow Actuators</span>
      <span style="font-size:0.75rem; color:var(--text-muted);">ACTIVE-LOW & INTERLOCKED</span>
    </div>
    <div class="matrix-grid">
      <div id="nodePeltier" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH1: PELTIER 12V</div>
        <div id="stPeltier" class="matrix-val">OFF</div>
      </div>
      <div id="nodeFan" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH2: HEATSINK FAN</div>
        <div id="stFan" class="matrix-val">OFF</div>
      </div>
      <div id="nodeBlower" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH3: DC BLOWER</div>
        <div id="stBlower" class="matrix-val">OFF</div>
      </div>
      <div id="nodeLight" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH4: GROW LIGHT</div>
        <div id="stLight" class="matrix-val">OFF</div>
      </div>
      <div id="nodeSpray1" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH5: SPRAY T1</div>
        <div id="stSpray1" class="matrix-val">OFF</div>
      </div>
      <div id="nodeSpray2" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH6: SPRAY T2</div>
        <div id="stSpray2" class="matrix-val">OFF</div>
      </div>
      <div id="nodeValve1" class="matrix-node" style="border-color:rgba(0,180,216,0.3);">
        <div class="matrix-name">VALVE SERVO 1</div>
        <div id="stValve1" class="matrix-val" style="color:var(--neon-blue);">0° (CLOSED)</div>
      </div>
      <div id="nodeValve2" class="matrix-node" style="border-color:rgba(0,255,157,0.3);">
        <div class="matrix-name">VALVE SERVO 2</div>
        <div id="stValve2" class="matrix-val" style="color:var(--neon-mint);">0° (CLOSED)</div>
      </div>
    </div>
  </div>

  <!-- Real-Time Trend Graph (SVG Canvas) -->
  <div class="chart-section">
    <div class="chart-card">
      <div class="inst-title">
        <span>Real-Time Agronomic Trend (Last 20 Points)</span>
        <div style="display:flex; gap:16px; font-size:0.75rem; text-transform:none;">
          <span style="display:flex; align-items:center; gap:6px;">
            <span style="width:12px; height:3px; background:var(--neon-mint); display:inline-block;"></span> Temp (°C)
          </span>
          <span style="display:flex; align-items:center; gap:6px;">
            <span style="width:12px; height:3px; background:var(--neon-blue); display:inline-block;"></span> RH (%)
          </span>
        </div>
      </div>
      <div class="chart-svg-container">
        <svg id="trendSvg" width="100%" height="100%" viewBox="0 0 800 190" preserveAspectRatio="none">
          <!-- Background Grid Lines -->
          <line x1="40" y1="20" x2="780" y2="20" stroke="rgba(255,255,255,0.06)" stroke-dasharray="4"/>
          <line x1="40" y1="65" x2="780" y2="65" stroke="rgba(255,255,255,0.06)" stroke-dasharray="4"/>
          <line x1="40" y1="110" x2="780" y2="110" stroke="rgba(255,255,255,0.06)" stroke-dasharray="4"/>
          <line x1="40" y1="155" x2="780" y2="155" stroke="rgba(255,255,255,0.06)" stroke-dasharray="4"/>
          
          <text x="10" y="25" fill="#64748b" font-size="10" font-family="JetBrains Mono">50°/100%</text>
          <text x="10" y="90" fill="#64748b" font-size="10" font-family="JetBrains Mono">25°/50%</text>
          <text x="10" y="160" fill="#64748b" font-size="10" font-family="JetBrains Mono">0°/0%</text>

          <!-- Dynamic SVG Polylines -->
          <polyline id="rhPolyline" fill="none" stroke="#00b4d8" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" points=""/>
          <polyline id="tempPolyline" fill="none" stroke="#00ff9d" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" points=""/>
        </svg>
      </div>
    </div>
  </div>

  <!-- Interactive Control Panel & System Log Console -->
  <div class="controls-grid">
    <!-- Interactive Control Panel -->
    <div class="card">
      <div class="inst-title">
        <span>Bilateral Command & Operational Controls</span>
        <span id="lblMode" class="badge badge-optimal">AUTOMATIC</span>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Operational Mode</div>
          <div style="font-size:0.75rem; color:var(--text-muted);">Toggle between Autonomous Closed-Loop & Manual Override</div>
        </div>
        <button id="btnModeToggle" class="btn btn-mint" onclick="toggleMode()">SWITCH TO MANUAL</button>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Growth Phase Selection</div>
          <div style="font-size:0.75rem; color:var(--text-muted);">Switch between Germination (Blackout) and Autotrophic (Light)</div>
        </div>
        <div style="display:flex; gap:8px;">
          <button class="btn" onclick="sendControl('action=phase&val=1')">Germination</button>
          <button class="btn" onclick="sendControl('action=phase&val=2')">Autotrophic</button>
        </div>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Active Operational Tank</div>
          <div style="font-size:0.75rem; color:var(--text-muted);">Select Primary Hydration Line</div>
        </div>
        <div style="display:flex; gap:8px;">
          <button id="btnTank1" class="btn" onclick="sendControl('action=tank&val=1')">Tank 1 (Raw)</button>
          <button id="btnTank2" class="btn" onclick="sendControl('action=tank&val=2')">Tank 2 (Nutrient)</button>
        </div>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Cultivation Day Stepper</div>
          <div style="font-size:0.75rem; color:var(--text-muted);">Adjust Growth Sequence Day Counter</div>
        </div>
        <div style="display:flex; gap:8px; align-items:center;">
          <button class="btn" style="padding:6px 12px;" onclick="adjustDay(-1)">-1</button>
          <span id="lblDayCount" style="font-family:'JetBrains Mono'; font-weight:700;">Day 1</span>
          <button class="btn" style="padding:6px 12px;" onclick="adjustDay(1)">+1</button>
        </div>
      </div>

      <div class="toggle-row" style="border-bottom:none; margin-top:6px;">
        <div>
          <div class="toggle-label" style="color:var(--neon-red);">Emergency Purge & Flush</div>
          <div style="font-size:0.75rem; color:var(--text-muted);">Force 5-second high-rate mist flush with pure Tank 1</div>
        </div>
        <button class="btn btn-red" onclick="sendControl('action=flush')">⚡ EMERGENCY FLUSH</button>
      </div>

      <!-- Manual Overrides (Visible in Manual Mode) -->
      <div id="manualControls" style="display:none; margin-top:16px; padding-top:16px; border-top:1px solid rgba(255,255,255,0.1);">
        <div class="card-label" style="margin-bottom:10px; color:var(--neon-amber);">Direct Actuator Override (Manual Mode Active)</div>
        <div style="display:flex; flex-wrap:wrap; gap:8px;">
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=1')">Toggle Peltier</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=2')">Toggle Fan</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=3')">Toggle Blower</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=4')">Toggle GrowLight</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=5')">Toggle Spray 1</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=6')">Toggle Spray 2</button>
        </div>
      </div>
    </div>

    <!-- Mini Terminal System Event Console -->
    <div class="terminal-card">
      <div class="terminal-header">
        <span class="term-dot term-red"></span>
        <span class="term-dot term-yellow"></span>
        <span class="term-dot term-green"></span>
        <span style="margin-left:8px;">SYSTEM TELEMETRY LOG CONSOLE</span>
      </div>
      <div id="terminalLogs" class="terminal-body">
        <div class="term-log"><span class="term-time">[00:00:00]</span><span class="term-msg">Initializing Biosphere Cabin Controller...</span></div>
      </div>
    </div>
  </div>

  <script>
    let isAutoMode = true;
    let currentDay = 1;

    async function fetchTelemetry() {
      try {
        const res = await fetch('/api/telemetry');
        if (!res.ok) return;
        const data = await res.json();
        renderDashboard(data);
      } catch (err) {
        console.warn('Telemetry polling err:', err);
      }
    }

    function renderDashboard(d) {
      // 1. IP & Uptime
      document.getElementById('valIp').innerText = d.ip || 'ESP32';
      document.getElementById('valUptime').innerText = d.uptime || '00:00:00';

      // 2. Global Climate Status Badge
      const stBadge = document.getElementById('badgeStatus');
      const stText = document.getElementById('badgeStatusText');
      stBadge.className = 'badge ' + (d.status === 'OPTIMAL' ? 'badge-optimal' : (d.status === 'WASPADA' ? 'badge-waspada' : (d.status === 'SAFEMODE' ? 'badge-safe' : 'badge-kritis')));
      stText.innerText = d.status;

      // 3. Primary Telemetry Cards
      document.getElementById('valTemp').innerText = d.temp.toFixed(1);
      document.getElementById('valRh').innerText = d.rh.toFixed(1);
      document.getElementById('valSoil').innerText = d.soil.toFixed(1);
      document.getElementById('valVpd').innerText = d.vpd.toFixed(2);

      // Card Status Badges
      updateCardBadge('badgeTemp', d.temp >= 18.0 && d.temp <= 22.0, d.temp > 24.0, 'OPTIMAL', 'WASPADA', 'KRITIS');
      updateCardBadge('badgeRh', d.rh >= 50.0 && d.rh <= 65.0, d.rh > 75.0, 'OPTIMAL', 'WASPADA', 'KRITIS');
      updateCardBadge('badgeSoil', d.soil >= 45.0 && d.soil <= 70.0, d.soil < 35.0, 'OPTIMAL', 'WASPADA', 'KRITIS');
      updateCardBadge('badgeVpd', d.vpd >= 0.40 && d.vpd <= 0.80, d.vpd < 0.20 || d.vpd > 1.20, 'BALANCED', 'DRIFT', 'CRITICAL');

      // 4. Plant Comfort Score (PCS)
      document.getElementById('valPcs').innerText = Math.round(d.pcs);
      document.getElementById('valPcsText').innerText = Math.round(d.pcs) + '%';
      document.getElementById('pcsBar').style.width = Math.min(100, Math.max(0, d.pcs)) + '%';

      // 5. Dual-Tank Volumes
      document.getElementById('valTank1Text').innerText = Math.round(d.tank1Vol) + ' mL';
      document.getElementById('valTank2Text').innerText = Math.round(d.tank2Vol) + ' mL';
      document.getElementById('tank1Bar').style.width = ((d.tank1Vol / 1000) * 100) + '%';
      document.getElementById('tank2Bar').style.width = ((d.tank2Vol / 1000) * 100) + '%';

      // 6. Growth Phase Sequencer
      currentDay = d.day;
      document.getElementById('valDayBadge').innerText = 'DAY ' + d.day + ' / 10';
      document.getElementById('lblDayCount').innerText = 'Day ' + d.day;
      document.getElementById('phaseProgress').style.width = ((d.day / 10) * 100) + '%';

      if (d.phase === 1) {
        document.getElementById('valPhaseName').innerText = 'Germination Blackout';
        document.getElementById('valPhaseDesc').innerText = 'Grow Light OFF • Target RH 60-70% • Pure Tank 1 Spray';
      } else {
        document.getElementById('valPhaseName').innerText = 'Autotrophic Light';
        document.getElementById('valPhaseDesc').innerText = '16h Photoperiod • VPD 0.6-0.8 kPa • Tank 2 Booster Spray';
      }

      // 7. Hardware Relays & Valves Matrix
      setNodeActive('nodePeltier', 'stPeltier', d.relays.peltier, false);
      setNodeActive('nodeFan', 'stFan', d.relays.fan, true);
      setNodeActive('nodeBlower', 'stBlower', d.relays.blower, false);
      setNodeActive('nodeLight', 'stLight', d.relays.light, false);
      setNodeActive('nodeSpray1', 'stSpray1', d.relays.spray1, false);
      setNodeActive('nodeSpray2', 'stSpray2', d.relays.spray2, false);

      document.getElementById('stValve1').innerText = d.valves.v1 + '° ' + (d.valves.v1 > 0 ? '(OPEN)' : '(CLOSED)');
      document.getElementById('stValve2').innerText = d.valves.v2 + '° ' + (d.valves.v2 > 0 ? '(OPEN)' : '(CLOSED)');

      // 8. Operational Mode UI
      isAutoMode = d.autoMode;
      document.getElementById('lblMode').innerText = isAutoMode ? 'AUTOMATIC' : 'MANUAL OVERRIDE';
      document.getElementById('lblMode').className = 'badge ' + (isAutoMode ? 'badge-optimal' : 'badge-waspada');
      document.getElementById('btnModeToggle').innerText = isAutoMode ? 'SWITCH TO MANUAL' : 'SWITCH TO AUTOMATIC';
      document.getElementById('manualControls').style.display = isAutoMode ? 'none' : 'block';

      // Active tank button highlighting
      document.getElementById('btnTank1').className = 'btn ' + (d.activeTank === 1 ? 'btn-mint' : '');
      document.getElementById('btnTank2').className = 'btn ' + (d.activeTank === 2 ? 'btn-mint' : '');

      // 9. SVG History Graph
      if (d.history && d.history.temp && d.history.rh) {
        renderSvgHistory(d.history.temp, d.history.rh);
      }

      // 10. System Log Console
      if (d.logs && d.logs.length > 0) {
        let logHtml = '';
        d.logs.forEach(l => {
          logHtml += `<div class="term-log"><span class="term-time">[${l.t}]</span><span class="term-msg">${l.m}</span></div>`;
        });
        document.getElementById('terminalLogs').innerHTML = logHtml;
      }
    }

    function updateCardBadge(id, isOptimal, isCritical, txtOpt, txtWarn, txtCrit) {
      const el = document.getElementById(id);
      if (isOptimal) {
        el.className = 'badge badge-optimal';
        el.innerText = txtOpt;
      } else if (isCritical) {
        el.className = 'badge badge-kritis';
        el.innerText = txtCrit;
      } else {
        el.className = 'badge badge-waspada';
        el.innerText = txtWarn;
      }
    }

    function setNodeActive(nodeId, textId, active, isFan) {
      const el = document.getElementById(nodeId);
      const txt = document.getElementById(textId);
      if (active) {
        el.className = 'matrix-node ' + (isFan ? 'active-fan' : 'active');
        txt.innerText = 'ON';
      } else {
        el.className = 'matrix-node';
        txt.innerText = 'OFF';
      }
    }

    function renderSvgHistory(temps, rhs) {
      const width = 740;
      const height = 140;
      const startX = 40;
      const startY = 20;

      let tempPts = [];
      let rhPts = [];

      const n = Math.max(temps.length, rhs.length);
      if (n < 2) return;

      for (let i = 0; i < n; i++) {
        const x = startX + (i / (n - 1)) * width;
        // Temp scale: 0 to 50°C
        const tVal = Math.min(50, Math.max(0, temps[i] || 0));
        const yTemp = (startY + height) - (tVal / 50) * height;
        tempPts.push(`${x.toFixed(1)},${yTemp.toFixed(1)}`);

        // RH scale: 0 to 100%
        const rhVal = Math.min(100, Math.max(0, rhs[i] || 0));
        const yRh = (startY + height) - (rhVal / 100) * height;
        rhPts.push(`${x.toFixed(1)},${yRh.toFixed(1)}`);
      }

      document.getElementById('tempPolyline').setAttribute('points', tempPts.join(' '));
      document.getElementById('rhPolyline').setAttribute('points', rhPts.join(' '));
    }

    async function sendControl(queryString) {
      try {
        await fetch('/api/control?' + queryString, { method: 'POST' });
        fetchTelemetry();
      } catch (err) {
        console.error('Command err:', err);
      }
    }

    function toggleMode() {
      sendControl('action=mode&val=' + (isAutoMode ? 'manual' : 'auto'));
    }

    function adjustDay(delta) {
      let d = currentDay + delta;
      if (d < 1) d = 1;
      if (d > 10) d = 10;
      sendControl('action=day&val=' + d);
    }

    // High frequency asynchronous telemetry refresh (every 1000ms, zero-reload)
    setInterval(fetchTelemetry, 1000);
    fetchTelemetry();
  </script>
</body>
</html>
)rawliteral";

// =========================================================================================
// 9. REST API & HTTP SERVER HANDLERS
// =========================================================================================

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleTelemetryApi() {
  char uptimeStr[12];
  getFormattedTimestamp(uptimeStr, sizeof(uptimeStr));

  // Determine status label
  const char* statusStr = "OPTIMAL";
  if (metrics.status == STATUS_SAFEMODE) statusStr = "SAFEMODE";
  else if (metrics.status == STATUS_KRITIS) statusStr = "KRITIS";
  else if (metrics.status == STATUS_WASPADA) statusStr = "WASPADA";

  // Build JSON response dynamically without dynamic heap fragmentation
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime\":\"" + String(uptimeStr) + "\",";
  json += "\"status\":\"" + String(statusStr) + "\",";
  json += "\"autoMode\":" + String(state.autoMode ? "true" : "false") + ",";
  json += "\"phase\":" + String((int)state.phase) + ",";
  json += "\"day\":" + String(state.dayCounter) + ",";
  json += "\"activeTank\":" + String(state.activeTank) + ",";
  json += "\"temp\":" + String(metrics.temperature, 2) + ",";
  json += "\"rh\":" + String(metrics.humidity, 2) + ",";
  json += "\"soil\":" + String(metrics.soilMoisture, 2) + ",";
  json += "\"vpd\":" + String(metrics.vpd, 3) + ",";
  json += "\"pcs\":" + String(metrics.plantComfortScore, 1) + ",";
  json += "\"tank1Vol\":" + String(state.tank1VolumeMl, 1) + ",";
  json += "\"tank2Vol\":" + String(state.tank2VolumeMl, 1) + ",";

  // Relays State Object
  json += "\"relays\":{";
  json += "\"peltier\":" + String(state.peltier ? "true" : "false") + ",";
  json += "\"fan\":" + String(state.heatsinkFan ? "true" : "false") + ",";
  json += "\"blower\":" + String(state.blower ? "true" : "false") + ",";
  json += "\"light\":" + String(state.growLight ? "true" : "false") + ",";
  json += "\"spray1\":" + String(state.sprayT1 ? "true" : "false") + ",";
  json += "\"spray2\":" + String(state.sprayT2 ? "true" : "false");
  json += "},";

  // Valve Servomotors Object
  json += "\"valves\":{";
  json += "\"v1\":" + String(state.valve1Angle) + ",";
  json += "\"v2\":" + String(state.valve2Angle);
  json += "},";

  // Telemetry History (Last 20 points)
  json += "\"history\":{";
  json += "\"temp\":[";
  for (uint8_t i = 0; i < historyCount; i++) {
    uint8_t idx = (historyHead + HISTORY_LENGTH - historyCount + i) % HISTORY_LENGTH;
    json += String(tempHistory[idx], 1);
    if (i < historyCount - 1) json += ",";
  }
  json += "],\"rh\":[";
  for (uint8_t i = 0; i < historyCount; i++) {
    uint8_t idx = (historyHead + HISTORY_LENGTH - historyCount + i) % HISTORY_LENGTH;
    json += String(rhHistory[idx], 1);
    if (i < historyCount - 1) json += ",";
  }
  json += "]},";

  // Log Console (Last 5 events)
  json += "\"logs\":[";
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (logHead + LOG_CAPACITY - logCount + i) % LOG_CAPACITY;
    json += "{\"t\":\"" + String(logBuffer[idx].timestamp) + "\",\"m\":\"" + String(logBuffer[idx].message) + "\"}";
    if (i < logCount - 1) json += ",";
  }
  json += "]";

  json += "}";

  server.send(200, "application/json", json);
}

void handleControlApi() {
  if (!server.hasArg("action")) {
    server.send(400, "application/json", "{\"error\":\"Missing action parameter\"}");
    return;
  }

  String action = server.arg("action");

  if (action == "mode") {
    String val = server.arg("val");
    state.autoMode = (val == "auto");
    addSystemLog(state.autoMode ? "Mode set to AUTOMATIC CLIMATE" : "Mode set to MANUAL OVERRIDE");
  } 
  else if (action == "phase") {
    int p = server.arg("val").toInt();
    if (p == 1) {
      state.phase = PHASE_GERMINATION_BLACKOUT;
      addSystemLog("Switched to Phase 1: Germination Blackout");
    } else if (p == 2) {
      state.phase = PHASE_AUTOTROPHIC_LIGHT;
      addSystemLog("Switched to Phase 2: Autotrophic Light");
    }
  } 
  else if (action == "day") {
    int d = server.arg("val").toInt();
    state.dayCounter = constrain(d, 1, 10);
    // Automatic phase synchronization with day counter
    if (state.dayCounter <= 3) {
      state.phase = PHASE_GERMINATION_BLACKOUT;
    } else {
      state.phase = PHASE_AUTOTROPHIC_LIGHT;
    }
    char msg[32];
    snprintf(msg, sizeof(msg), "Day counter set to: Day %u", state.dayCounter);
    addSystemLog(msg);
  } 
  else if (action == "tank") {
    int t = server.arg("val").toInt();
    if (t == 1 || t == 2) {
      state.activeTank = t;
      char msg[32];
      snprintf(msg, sizeof(msg), "Active Tank set to: Tank %u", state.activeTank);
      addSystemLog(msg);
    }
  } 
  else if (action == "flush") {
    // 5-second emergency flush with Tank 1
    state.emergencyFlushActive = true;
    state.emergencyFlushStartTime = millis();
    applySprayAndValves(true, false);
    addSystemLog("EMERGENCY FLUSH: Tank 1 spray activated 5s");
  } 
  else if (action == "refill") {
    int t = server.arg("tank").toInt();
    if (t == 1) {
      state.tank1VolumeMl = TANK_MAX_CAPACITY_ML;
      addSystemLog("Tank 1 refilled to 1000 mL");
    } else if (t == 2) {
      state.tank2VolumeMl = TANK_MAX_CAPACITY_ML;
      addSystemLog("Tank 2 refilled to 1000 mL");
    }
  } 
  else if (action == "toggleRelay") {
    if (state.autoMode) {
      server.send(403, "application/json", "{\"error\":\"Cannot override relays while in AUTOMATIC mode\"}");
      return;
    }
    int ch = server.arg("ch").toInt();
    switch (ch) {
      case 1: // Peltier (Interlock enforced)
        applyPeltierInterlock(!state.peltier);
        addSystemLog(state.peltier ? "Manual: Peltier ON" : "Manual: Peltier OFF");
        break;
      case 2: // Fan
        applyHeatsinkFan(!state.heatsinkFan);
        addSystemLog(state.heatsinkFan ? "Manual: Fan ON" : "Manual: Fan OFF");
        break;
      case 3: // Blower
        applyBlower(!state.blower);
        addSystemLog(state.blower ? "Manual: Blower ON" : "Manual: Blower OFF");
        break;
      case 4: // Grow Light
        applyGrowLight(!state.growLight);
        addSystemLog(state.growLight ? "Manual: GrowLight ON" : "Manual: GrowLight OFF");
        break;
      case 5: // Spray T1
        applySprayAndValves(!state.sprayT1, false);
        addSystemLog(state.sprayT1 ? "Manual: Spray T1 ON" : "Manual: Spray T1 OFF");
        break;
      case 6: // Spray T2
        applySprayAndValves(false, !state.sprayT2);
        addSystemLog(state.sprayT2 ? "Manual: Spray T2 ON" : "Manual: Spray T2 OFF");
        break;
      default:
        break;
    }
  }

  server.send(200, "application/json", "{\"success\":true}");
}

// =========================================================================================
// 10. SETUP & INITIALIZATION ROUTINE
// =========================================================================================

void setup() {
  Serial.begin(115200);
  delay(500); // Allow power rails to stabilize

  Serial.println("\n========================================================");
  Serial.println("  SMART HYPER-MONITORED BIOSPHERE CABIN - ESP32 STARTUP ");
  Serial.println("  Cultivation Engine for Broccoli Microgreens           ");
  Serial.println("========================================================");

  // 1. Safe Relay Initialization (Active-LOW Protection)
  // Set output states to HIGH (OFF) BEFORE enabling OUTPUT mode to prevent relay chatter
  digitalWrite(PIN_RELAY_PELTIER, RELAY_TRIGGER_OFF);
  digitalWrite(PIN_RELAY_FAN_HEATSINK, RELAY_TRIGGER_OFF);
  digitalWrite(PIN_RELAY_BLOWER, RELAY_TRIGGER_OFF);
  digitalWrite(PIN_RELAY_GROWLIGHT, RELAY_TRIGGER_OFF);
  digitalWrite(PIN_RELAY_SPRAY_T1, RELAY_TRIGGER_OFF);
  digitalWrite(PIN_RELAY_SPRAY_T2, RELAY_TRIGGER_OFF);

  pinMode(PIN_RELAY_PELTIER, OUTPUT);
  pinMode(PIN_RELAY_FAN_HEATSINK, OUTPUT);
  pinMode(PIN_RELAY_BLOWER, OUTPUT);
  pinMode(PIN_RELAY_GROWLIGHT, OUTPUT);
  pinMode(PIN_RELAY_SPRAY_T1, OUTPUT);
  pinMode(PIN_RELAY_SPRAY_T2, OUTPUT);

  // 2. Status LED & Sensor Pins
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);
  pinMode(PIN_SOIL_ADC, INPUT);

  // 3. Mechanical Servomotor Valve Initialization
  servoValve1.attach(PIN_SERVO_VALVE_T1);
  servoValve2.attach(PIN_SERVO_VALVE_T2);
  servoValve1.write(0); // 0° Closed
  servoValve2.write(0); // 0° Closed

  // 4. Initialize State Structures
  state.autoMode = true;
  state.phase = PHASE_GERMINATION_BLACKOUT;
  state.dayCounter = 1;
  state.activeTank = 1;
  state.peltier = false;
  state.heatsinkFan = false;
  state.blower = false;
  state.growLight = false;
  state.sprayT1 = false;
  state.sprayT2 = false;
  state.valve1Angle = 0;
  state.valve2Angle = 0;
  state.tank1VolumeMl = TANK_MAX_CAPACITY_ML;
  state.tank2VolumeMl = TANK_MAX_CAPACITY_ML;
  state.purgeActive = false;
  state.purgeStartTime = 0;
  state.lastPurgeCheckTime = 0;
  state.emergencyFlushActive = false;
  state.emergencyFlushStartTime = 0;
  state.autoSprayActive = false;
  state.autoSprayStartTime = 0;
  state.lastAutoSprayTime = 0;

  // 5. Initialize DHT22 Sensor
  dht.begin();
  addSystemLog("Hardware pins & sensors initialized.");

  // 6. Connect to Wi-Fi Network
  Serial.printf("Connecting to Wi-Fi SSID: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart < 12000UL)) {
    delay(400);
    Serial.print(".");
    digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Connected successfully!");
    Serial.printf("[Wi-Fi] Assigned IP Address: %s\n", WiFi.localIP().toString().c_str());
    char msg[64];
    snprintf(msg, sizeof(msg), "WiFi Connected: %s", WiFi.localIP().toString().c_str());
    addSystemLog(msg);
  } else {
    Serial.println("\n[Wi-Fi] Connection timed out. Starting Emergency Fallback AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BroccoliCabin_AP", "biosphere123");
    Serial.printf("[Wi-Fi AP] Fallback AP Started. IP: %s\n", WiFi.softAPIP().toString().c_str());
    addSystemLog("WiFi STA timeout; Fallback AP Started.");
  }

  // 7. Register Web Server Routes & Start
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/telemetry", HTTP_GET, handleTelemetryApi);
  server.on("/api/control", HTTP_ANY, handleControlApi);

  server.begin();
  addSystemLog("Web Server listening on Port 80.");
  addSystemLog("Autonomous Climate Loop initialized.");
}

// =========================================================================================
// 11. MAIN NON-BLOCKING SUPERVISORY LOOP (ZERO delay())
// =========================================================================================

void loop() {
  // Always handle HTTP client requests immediately
  server.handleClient();

  uint32_t currentMillis = millis();

  // 1. Sensor Polling Routine (Every 2000 ms - Non-blocking DHT22 & ADC)
  if (currentMillis - lastSensorPollTime >= 2000UL) {
    lastSensorPollTime = currentMillis;
    pollSensors();
  }

  // 2. Closed-Loop Climate Control Logic (Every 1000 ms)
  if (currentMillis - lastClimateLoopTime >= 1000UL) {
    lastClimateLoopTime = currentMillis;
    executeAutomatedClimateControl();
  }

  // 3. Anti-Fungal Purge Cycle Routine (30-minute interval check, 45s blower)
  processAntiFungalPurge();

  // 4. Emergency Flush Routine (5-second Tank 1 spray)
  processEmergencyFlush();

  // 5. Tank Reservoir Depletion Integrator
  updateTankVolumes();

  // 6. System Status LED Heartbeat / Fault Signaler
  processStatusLed();

  // 7. Simulated Cultivation Day Tracker (Advances day counter every 24 hours)
  if (currentMillis - lastDayIncrementTime >= (24UL * 3600UL * 1000UL)) {
    lastDayIncrementTime = currentMillis;
    if (state.dayCounter < 10) {
      state.dayCounter++;
      if (state.dayCounter > 3) {
        state.phase = PHASE_AUTOTROPHIC_LIGHT;
      }
      char msg[40];
      snprintf(msg, sizeof(msg), "Sequencer auto-advanced to Day %u", state.dayCounter);
      addSystemLog(msg);
    }
  }
}
