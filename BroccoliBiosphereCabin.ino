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
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Kebun Biosfer Brokoli | Smart Microgreens Cabin</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Quicksand:wght@500;600;700&family=JetBrains+Mono:wght@500;700&display=swap" rel="stylesheet">
  <style>
    :root {
      /* Warm Pastel & Daylight Palette */
      --bg-page: #f7f4ee;
      --bg-card: #ffffff;
      --bg-card-subtle: #fcfbf9;
      --border-card: #e4decb;
      --border-subtle: #eeeadf;

      /* Pastel Accent Hues */
      --pastel-mint: #48bb78;
      --pastel-mint-light: #edf7ed;
      --pastel-mint-deep: #22543d;

      --pastel-sky: #0284c7;
      --pastel-sky-light: #e0f2fe;
      --pastel-sky-deep: #0369a1;

      --pastel-butter: #d97706;
      --pastel-butter-light: #fef3c7;
      --pastel-butter-deep: #78350f;

      --pastel-coral: #e11d48;
      --pastel-coral-light: #ffe4e6;
      --pastel-coral-deep: #881337;

      --pastel-lavender: #7e22ce;
      --pastel-lavender-light: #f3e8ff;

      /* Text Colors (High Contrast Warm Charcoal) */
      --text-main: #242c38;
      --text-sub: #526071;
      --text-muted: #7e8c9f;

      /* Radii & Shadows */
      --radius-card: 18px;
      --radius-btn: 12px;
      --radius-pill: 9999px;
      --shadow-soft: 0 2px 8px rgba(50, 40, 30, 0.05), 0 1px 2px rgba(50, 40, 30, 0.03);
      --shadow-hover: 0 6px 16px rgba(50, 40, 30, 0.08);
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      background-color: var(--bg-page);
      background-image: radial-gradient(#e5dfd2 1px, transparent 1px);
      background-size: 20px 20px;
      color: var(--text-main);
      font-family: 'Quicksand', -apple-system, sans-serif;
      min-height: 100vh;
      padding: 24px;
      line-height: 1.5;
    }

    /* --- Top Navigation Header --- */
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 16px;
      margin-bottom: 24px;
      padding: 18px 24px;
      background: var(--bg-card);
      border: 1.5px solid var(--border-card);
      border-radius: var(--radius-card);
      box-shadow: var(--shadow-soft);
    }
    .title-group h1 {
      font-size: 1.45rem;
      font-weight: 800;
      letter-spacing: -0.02em;
      color: var(--text-main);
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .title-group p {
      font-size: 0.85rem;
      color: var(--text-sub);
      font-weight: 500;
    }
    .header-badges {
      display: flex;
      gap: 10px;
      align-items: center;
      flex-wrap: wrap;
    }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 12px;
      border-radius: var(--radius-pill);
      font-size: 0.76rem;
      font-weight: 700;
      border: 1px solid currentColor;
    }
    .badge-optimal { color: var(--pastel-mint-deep); background: var(--pastel-mint-light); border-color: #a7f3d0; }
    .badge-waspada { color: var(--pastel-butter-deep); background: var(--pastel-butter-light); border-color: #fde68a; }
    .badge-kritis  { color: var(--pastel-coral-deep); background: var(--pastel-coral-light); border-color: #fecdd3; animation: pulse-crit 1.6s infinite; }
    .badge-safe    { color: #6b21a8; background: #f3e8ff; border-color: #e9d5ff; }
    .badge-sky     { color: var(--pastel-sky-deep); background: var(--pastel-sky-light); border-color: #bae6fd; }
    .badge-muted   { color: var(--text-sub); background: #f1ede4; border-color: #e2dcce; }

    .pulse-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: currentColor;
    }

    @keyframes pulse-crit {
      0%, 100% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.75; transform: scale(0.96); }
    }

    /* --- Hero Companion Section (Brocco Virtual Pet) --- */
    .hero-companion-grid {
      display: grid;
      grid-template-columns: 340px 1fr;
      gap: 20px;
      margin-bottom: 24px;
    }
    @media (max-width: 900px) {
      .hero-companion-grid { grid-template-columns: 1fr; }
    }

    .card {
      background: var(--bg-card);
      border: 1.5px solid var(--border-card);
      border-radius: var(--radius-card);
      padding: 20px;
      box-shadow: var(--shadow-soft);
      transition: transform 0.15s ease, box-shadow 0.15s ease;
    }
    .card:hover {
      box-shadow: var(--shadow-hover);
    }

    .brocco-card {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: space-between;
      background: #ffffff;
      border: 1.5px solid #d5cebe;
      position: relative;
      overflow: hidden;
    }

    .brocco-bubble-wrapper {
      min-height: 58px;
      display: flex;
      align-items: center;
      justify-content: center;
      width: 100%;
      margin-bottom: 10px;
    }
    .brocco-bubble {
      background: #ffffff;
      border: 2px solid var(--pastel-mint);
      border-radius: 16px;
      padding: 10px 16px;
      font-size: 0.82rem;
      font-weight: 700;
      color: var(--text-main);
      text-align: center;
      position: relative;
      box-shadow: 0 4px 12px rgba(72, 187, 120, 0.12);
      transition: opacity 0.15s ease;
      max-width: 95%;
      line-height: 1.4;
    }
    .brocco-bubble::after {
      content: '';
      position: absolute;
      bottom: -8px;
      left: 50%;
      transform: translateX(-50%);
      width: 0;
      height: 0;
      border-left: 8px solid transparent;
      border-right: 8px solid transparent;
      border-top: 8px solid var(--pastel-mint);
    }

    .brocco-svg-container {
      width: 190px;
      height: 195px;
      cursor: pointer;
      user-select: none;
      transition: transform 0.15s;
    }
    .brocco-svg-container:hover {
      transform: scale(1.05);
    }

    /* Brocco Actions */
    .brocco-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      width: 100%;
      margin-top: 14px;
      justify-content: center;
    }
    .btn-nurture {
      flex: 1 1 45%;
      font-size: 0.78rem;
      font-weight: 700;
      padding: 9px 12px;
      border-radius: var(--radius-btn);
      border: 1.5px solid transparent;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      transition: all 0.15s ease;
    }
    .btn-nurture:active {
      transform: translateY(2px);
    }
    .btn-nurture-water {
      background: var(--pastel-sky-light);
      color: var(--pastel-sky-deep);
      border-color: #bae6fd;
    }
    .btn-nurture-water:hover {
      background: #bae6fd;
    }
    .btn-nurture-nutrient {
      background: var(--pastel-lavender-light);
      color: var(--pastel-lavender);
      border-color: #e9d5ff;
    }
    .btn-nurture-nutrient:hover {
      background: #e9d5ff;
    }
    .btn-nurture-breeze {
      background: var(--pastel-mint-light);
      color: var(--pastel-mint-deep);
      border-color: #bbf7d0;
    }
    .btn-nurture-breeze:hover {
      background: #bbf7d0;
    }
    .btn-sound {
      font-size: 0.72rem;
      font-weight: 600;
      padding: 5px 12px;
      background: #f4efe4;
      border: 1px solid #dcd5c5;
      border-radius: var(--radius-pill);
      color: var(--text-sub);
      cursor: pointer;
      margin-top: 6px;
      transition: all 0.15s;
    }
    .btn-sound:hover {
      color: var(--text-main);
      background: #eadecb;
    }

    /* Companion Overview Card */
    .companion-overview-card {
      display: flex;
      flex-direction: column;
      justify-content: space-between;
    }
    .card-label {
      font-size: 0.78rem;
      font-weight: 700;
      color: var(--text-sub);
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }
    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 12px;
    }
    .companion-stats-list {
      display: flex;
      flex-direction: column;
      gap: 8px;
      margin: 10px 0;
    }
    .comp-stat-item {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 9px 14px;
      background: #faf8f2;
      border: 1px solid #e8e3d6;
      border-radius: 12px;
      font-size: 0.84rem;
    }
    .comp-stat-name { color: var(--text-sub); font-weight: 600; }
    .comp-stat-val { font-weight: 800; color: var(--text-main); }
    .comp-tip-box {
      background: var(--pastel-butter-light);
      border: 1.5px dashed #fcd34d;
      border-radius: 14px;
      padding: 12px 16px;
      font-size: 0.80rem;
      color: var(--pastel-butter-deep);
      line-height: 1.45;
    }

    /* --- Mascot Animations --- */
    @keyframes brocco-idle {
      0%, 100% { transform: translateY(0) scale(1, 1); }
      50% { transform: translateY(-3px) scale(0.99, 1.01); }
    }
    @keyframes brocco-shiver {
      0%, 100% { transform: translate(0, 0); }
      25% { transform: translate(-3px, 1px); }
      75% { transform: translate(3px, -1px); }
    }
    @keyframes brocco-pant {
      0%, 100% { transform: scale(1, 1); }
      50% { transform: scale(1.04, 0.95) translateY(3px); }
    }
    @keyframes brocco-droop {
      0%, 100% { transform: rotate(0deg); }
      50% { transform: rotate(-4deg) translateY(4px); }
    }
    @keyframes brocco-hop {
      0% { transform: scale(1, 1); }
      30% { transform: scale(1.12, 0.88) translateY(6px); }
      60% { transform: scale(0.92, 1.12) translateY(-16px); }
      100% { transform: scale(1, 1) translateY(0); }
    }
    @keyframes float-zzz {
      0% { opacity: 0; transform: translate(0, 0) scale(0.6); }
      50% { opacity: 1; }
      100% { opacity: 0; transform: translate(14px, -22px) scale(1.2); }
    }

    .brocco-svg-container svg {
      animation: brocco-idle 3s ease-in-out infinite;
      transform-origin: bottom center;
    }
    .brocco-bounce svg {
      animation: brocco-hop 0.45s cubic-bezier(0.16, 1, 0.3, 1) !important;
    }

    /* Mood Expression State Classes */
    .eyes-open { display: block; }
    .eyes-sleep, .eyes-sad { display: none; }
    .mouth-happy { display: block; }
    .mouth-pant, .mouth-shiver, .mouth-sad { display: none; }
    .prop-sweat, .prop-icicles, .prop-zzz { display: none; }
    .prop-sparkles { display: block; }

    .mood-cold svg { animation: brocco-shiver 0.12s linear infinite; }
    .mood-cold .mouth-happy { display: none; }
    .mood-cold .mouth-shiver { display: block; }
    .mood-cold .prop-icicles { display: block; }
    .mood-cold .prop-sparkles { display: none; }

    .mood-heat svg { animation: brocco-pant 0.6s ease-in-out infinite; }
    .mood-heat .mouth-happy { display: none; }
    .mood-heat .mouth-pant { display: block; }
    .mood-heat .prop-sweat { display: block; }

    .mood-heat-crit svg { animation: brocco-pant 0.32s ease-in-out infinite; }
    .mood-heat-crit .mouth-happy { display: none; }
    .mood-heat-crit .mouth-pant { display: block; fill: var(--pastel-coral) !important; }
    .mood-heat-crit .prop-sweat { display: block; }

    .mood-thirsty svg { animation: brocco-droop 2.5s ease-in-out infinite; }
    .mood-thirsty .eyes-open { display: none; }
    .mood-thirsty .eyes-sad { display: block; }
    .mood-thirsty .mouth-happy { display: none; }
    .mood-thirsty .mouth-sad { display: block; }
    .mood-thirsty .prop-sparkles { display: none; }

    .mood-stagnant svg { animation: brocco-droop 3s ease-in-out infinite; }
    .mood-stagnant .mouth-happy { display: none; }
    .mood-stagnant .mouth-pant { display: block; }
    .mood-stagnant .prop-sweat { display: block; }

    .mood-sleep svg { animation: brocco-idle 4s ease-in-out infinite; }
    .mood-sleep .eyes-open { display: none; }
    .mood-sleep .eyes-sleep { display: block; }
    .mood-sleep .prop-zzz { display: block; }
    .mood-sleep .zzz-item { animation: float-zzz 2.5s infinite; }
    .mood-sleep .z2 { animation-delay: 0.6s; }
    .mood-sleep .z3 { animation-delay: 1.2s; }
    .mood-sleep .prop-sparkles { display: none; }

    .mood-panic svg { animation: brocco-shiver 0.08s infinite; }
    .mood-panic .eyes-open { display: none; }
    .mood-panic .eyes-sad { display: block; }
    .mood-panic .mouth-happy { display: none; }
    .mood-panic .mouth-shiver { display: block; stroke: var(--pastel-coral); }

    /* --- 4x Primary Telemetry Cards --- */
    .telemetry-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
      gap: 20px;
      margin-bottom: 24px;
    }
    .card-target {
      font-size: 0.72rem;
      font-weight: 600;
      color: var(--text-muted);
    }
    .card-value {
      font-size: 2.3rem;
      font-weight: 800;
      letter-spacing: -0.03em;
      color: var(--text-main);
      margin: 6px 0 10px;
      display: flex;
      align-items: baseline;
      gap: 4px;
    }
    .card-unit {
      font-size: 1rem;
      font-weight: 600;
      color: var(--text-sub);
    }

    /* --- Secondary Instruments Grid --- */
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
      font-size: 0.80rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.04em;
      color: var(--text-sub);
      margin-bottom: 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .pcs-meter {
      height: 12px;
      background: #eee8db;
      border-radius: var(--radius-pill);
      overflow: hidden;
      margin: 10px 0;
    }
    .pcs-fill {
      height: 100%;
      background: linear-gradient(90deg, #f59e0b, #48bb78);
      border-radius: var(--radius-pill);
      transform-origin: left; transition: transform 0.35s cubic-bezier(0.16, 1, 0.3, 1);
    }

    .tank-row {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .tank-item {
      background: #faf8f2;
      border: 1px solid #e8e3d6;
      border-radius: 12px;
      padding: 10px 14px;
    }
    .tank-header {
      display: flex;
      justify-content: space-between;
      font-size: 0.80rem;
      font-weight: 600;
      margin-bottom: 6px;
    }
    .tank-bar {
      height: 8px;
      background: #eee8db;
      border-radius: var(--radius-pill);
      overflow: hidden;
    }
    .tank-fill-1 { height: 100%; background: #38bdf8; transform-origin: left; transition: transform 0.35s cubic-bezier(0.16, 1, 0.3, 1); }
    .tank-fill-2 { height: 100%; background: #a855f7; transform-origin: left; transition: transform 0.35s cubic-bezier(0.16, 1, 0.3, 1); }

    /* --- 6-Channel Hardware Matrix --- */
    .matrix-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
      gap: 12px;
      margin-top: 10px;
    }
    .matrix-node {
      background: #faf8f2;
      border: 1.5px solid #e6e0d3;
      border-radius: 14px;
      padding: 12px 10px;
      text-align: center;
      transition: all 0.15s;
    }
    .matrix-node.active {
      border-color: var(--pastel-mint);
      background: var(--pastel-mint-light);
    }
    .matrix-node.active-fan {
      border-color: var(--pastel-sky);
      background: var(--pastel-sky-light);
    }
    .matrix-name {
      font-size: 0.70rem;
      font-weight: 700;
      color: var(--text-sub);
      margin-bottom: 6px;
      text-transform: uppercase;
    }
    .matrix-led {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: #cbd5e1;
      margin: 0 auto 6px;
      transition: all 0.2s;
    }
    .matrix-node.active .matrix-led {
      background: var(--pastel-mint);
      box-shadow: 0 0 6px rgba(72, 187, 120, 0.6);
    }
    .matrix-node.active-fan .matrix-led {
      background: var(--pastel-sky);
      box-shadow: 0 0 6px rgba(56, 189, 248, 0.6);
    }
    .matrix-val {
      font-size: 0.76rem;
      font-weight: 800;
      color: var(--text-main);
    }

    /* --- Trend Graph --- */
    .chart-card {
      background: var(--bg-card);
      border: 1.5px solid var(--border-card);
      border-radius: var(--radius-card);
      padding: 20px;
      margin-bottom: 24px;
      box-shadow: var(--shadow-soft);
    }
    .chart-svg-container {
      width: 100%;
      height: 180px;
      background: #faf8f2;
      border: 1px solid #e8e3d6;
      border-radius: 12px;
      padding: 10px;
    }

    /* --- Controls & Terminal Grid --- */
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
      background: #ffffff;
      color: var(--text-main);
      border: 1.5px solid var(--border-card);
      border-radius: var(--radius-btn);
      padding: 9px 14px;
      font-size: 0.80rem;
      font-weight: 700;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      transition: all 0.15s ease;
    }
    .btn:hover {
      background: #f5f1e8;
      border-color: #cfc7b4;
    }
    .btn:active {
      transform: translateY(2px);
    }
    .btn-mint {
      background: var(--pastel-mint-light);
      border-color: #86efac;
      color: var(--pastel-mint-deep);
    }
    .btn-mint:hover {
      background: #86efac;
    }
    .btn-red {
      background: var(--pastel-coral-light);
      border-color: #fca5a5;
      color: var(--pastel-coral-deep);
    }
    .btn-red:hover {
      background: #fca5a5;
    }
    .toggle-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 10px 0;
      border-bottom: 1px solid #eeeadf;
    }
    .toggle-label {
      font-size: 0.84rem;
      font-weight: 700;
      color: var(--text-main);
    }

    /* --- Garden Notebook Log --- */
    .terminal-card {
      background: #ffffff;
      border: 1.5px solid var(--border-card);
      border-radius: var(--radius-card);
      padding: 18px;
      box-shadow: var(--shadow-soft);
    }
    .terminal-header {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 12px;
      padding-bottom: 8px;
      border-bottom: 1px solid #eeeadf;
      font-size: 0.74rem;
      font-weight: 800;
      color: var(--text-sub);
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }
    .term-dot { width: 9px; height: 9px; border-radius: 50%; }
    .term-red { background: #f87171; }
    .term-yellow { background: #fbbf24; }
    .term-green { background: #4ade80; }
    .terminal-body {
      display: flex;
      flex-direction: column;
      gap: 8px;
      font-size: 0.80rem;
      min-height: 100px;
    }
    .term-log {
      display: flex;
      gap: 8px;
      line-height: 1.4;
    }
    .term-time { color: var(--pastel-sky-deep); font-weight: 700; font-family: 'JetBrains Mono', monospace; font-size: 0.75rem; }
    .term-msg { color: var(--text-main); font-weight: 500; }
  </style>
</head>
<body>

  <!-- Header -->
  <div class="header">
    <div class="title-group">
      <h1>🌱 Kebun Biosfer Brokoli</h1>
      <p>Smart Monitored Microgreens Cabin (Brassica oleracea var. italica)</p>
    </div>
    <div class="header-badges">
      <div id="badgeStatus" class="badge badge-optimal">
        <span class="pulse-dot"></span>
        <span id="badgeStatusText">KONDISI PRIMA</span>
      </div>
      <div class="badge badge-sky">
        IP: <span id="valIp" style="margin-left:3px;">ESP32</span>
      </div>
      <div class="badge badge-muted">
        Aktif: <span id="valUptime" style="margin-left:3px; font-family:'JetBrains Mono',monospace;">00:00:00</span>
      </div>
    </div>
  </div>

  <!-- Hero Companion Section (Brocco Virtual Pet) -->
  <div class="hero-companion-grid">
    <!-- Card 1: Interactive Brocco Mascot -->
    <div class="card brocco-card">
      <div class="brocco-bubble-wrapper">
        <div id="broccoBubble" class="brocco-bubble">
          <span id="broccoSpeech">Halo! Aku Brocco, maskot kabin biosfermu! 🌱</span>
        </div>
      </div>

      <!-- Interactive SVG Mascot -->
      <div id="broccoMascot" class="brocco-svg-container mood-happy" onclick="handleBroccoTap()" title="Sentuh Brocco untuk berinteraksi!">
        <svg viewBox="0 0 200 210" width="100%" height="100%">
          <defs>
            <linearGradient id="stemGrad" x1="0%" y1="0%" x2="0%" y2="100%">
              <stop offset="0%" stop-color="#86efac"/>
              <stop offset="100%" stop-color="#34d399"/>
            </linearGradient>
            <linearGradient id="crownGrad" x1="0%" y1="0%" x2="100%" y2="100%">
              <stop offset="0%" stop-color="#48bb78"/>
              <stop offset="60%" stop-color="#38a169"/>
              <stop offset="100%" stop-color="#22543d"/>
            </linearGradient>
            <radialGradient id="blushGrad" cx="50%" cy="50%" r="50%">
              <stop offset="0%" stop-color="rgba(251, 113, 133, 0.8)"/>
              <stop offset="100%" stop-color="rgba(251, 113, 133, 0)"/>
            </radialGradient>
            <radialGradient id="heatBlushGrad" cx="50%" cy="50%" r="50%">
              <stop offset="0%" stop-color="rgba(225, 29, 72, 0.9)"/>
              <stop offset="100%" stop-color="rgba(225, 29, 72, 0)"/>
            </radialGradient>
          </defs>

          <!-- Shadow -->
          <ellipse cx="100" cy="195" rx="55" ry="9" fill="rgba(60, 50, 40, 0.12)"/>

          <!-- Body / Stem -->
          <g class="brocco-body-group">
            <path d="M 82,125 C 75,155 70,185 80,195 C 90,200 110,200 120,195 C 130,185 125,155 118,125 Z" fill="url(#stemGrad)" stroke="#1f513b" stroke-width="2.5"/>
            <!-- Little Leaf Hands -->
            <path class="brocco-arm-left" d="M 76,155 C 55,150 50,135 62,130 C 72,135 74,148 76,155 Z" fill="#68d391" stroke="#1f513b" stroke-width="2"/>
            <path class="brocco-arm-right" d="M 124,155 C 145,150 150,135 138,130 C 128,135 126,148 124,155 Z" fill="#68d391" stroke="#1f513b" stroke-width="2"/>
          </g>

          <!-- Crown / Florets -->
          <g class="brocco-crown-group" stroke="#1c452b" stroke-width="2.5">
            <circle cx="65" cy="85" r="38" fill="url(#crownGrad)"/>
            <circle cx="135" cy="85" r="38" fill="url(#crownGrad)"/>
            <circle cx="100" cy="60" r="42" fill="url(#crownGrad)"/>
            <circle cx="100" cy="92" r="36" fill="url(#crownGrad)"/>
            <!-- Texture Dots -->
            <circle cx="75" cy="65" r="4" fill="#a7f3d0" stroke="none"/>
            <circle cx="120" cy="60" r="3.5" fill="#a7f3d0" stroke="none"/>
            <circle cx="100" cy="78" r="4" fill="#a7f3d0" stroke="none"/>
          </g>

          <!-- Face -->
          <g class="brocco-face">
            <!-- Cheeks -->
            <ellipse class="brocco-cheek" cx="72" cy="115" rx="8" ry="5" fill="url(#blushGrad)"/>
            <ellipse class="brocco-cheek" cx="128" cy="115" rx="8" ry="5" fill="url(#blushGrad)"/>

            <!-- Eyes: Happy / Open -->
            <g class="eyes-open">
              <ellipse cx="80" cy="98" rx="8" ry="11" fill="#1c2430"/>
              <ellipse cx="120" cy="98" rx="8" ry="11" fill="#1c2430"/>
              <circle cx="82" cy="94" r="3.5" fill="#ffffff"/>
              <circle cx="78" cy="102" r="1.5" fill="#ffffff"/>
              <circle cx="122" cy="94" r="3.5" fill="#ffffff"/>
              <circle cx="118" cy="102" r="1.5" fill="#ffffff"/>
            </g>

            <!-- Eyes: Sleeping -->
            <g class="eyes-sleep">
              <path d="M 72,100 Q 80,107 88,100" fill="none" stroke="#1c2430" stroke-width="3" stroke-linecap="round"/>
              <path d="M 112,100 Q 120,107 128,100" fill="none" stroke="#1c2430" stroke-width="3" stroke-linecap="round"/>
            </g>

            <!-- Eyes: Sad / Droop -->
            <g class="eyes-sad">
              <path d="M 72,102 Q 80,95 88,102" fill="none" stroke="#1c2430" stroke-width="3" stroke-linecap="round"/>
              <path d="M 112,102 Q 120,95 128,102" fill="none" stroke="#1c2430" stroke-width="3" stroke-linecap="round"/>
              <path d="M 76,108 C 74,115 80,118 80,113 C 80,110 77,108 76,108 Z" fill="#38bdf8"/>
            </g>

            <!-- Mouth Variations -->
            <path class="mouth-happy" d="M 90,112 Q 100,124 110,112" fill="none" stroke="#1c2430" stroke-width="3" stroke-linecap="round"/>
            <path class="mouth-pant" d="M 92,112 Q 100,126 108,112 Z" fill="#f87171" stroke="#1c2430" stroke-width="2"/>
            <path class="mouth-shiver" d="M 91,114 Q 95,110 100,114 Q 105,110 109,114" fill="none" stroke="#1c2430" stroke-width="2.5" stroke-linecap="round"/>
            <path class="mouth-sad" d="M 92,118 Q 100,110 108,118" fill="none" stroke="#1c2430" stroke-width="3" stroke-linecap="round"/>
          </g>

          <!-- Props & Effects -->
          <g class="brocco-props">
            <!-- Sweat Drops -->
            <g class="prop-sweat">
              <path d="M 148,70 C 145,78 152,82 152,77 C 152,73 149,70 148,70 Z" fill="#38bdf8"/>
              <path d="M 52,75 C 49,83 56,87 56,82 C 56,78 53,75 52,75 Z" fill="#38bdf8"/>
            </g>

            <!-- Icicles (Cold) -->
            <g class="prop-icicles">
              <polygon points="62,115 65,130 68,115" fill="#38bdf8"/>
              <polygon points="98,135 101,152 104,135" fill="#38bdf8"/>
              <polygon points="132,115 135,130 138,115" fill="#38bdf8"/>
            </g>

            <!-- Zzz (Sleep) -->
            <g class="prop-zzz">
              <text x="145" y="60" fill="#0284c7" font-size="16" font-family="'Plus Jakarta Sans', sans-serif" font-weight="800" class="zzz-item z1">Z</text>
              <text x="160" y="45" fill="#16a34a" font-size="12" font-family="'Plus Jakarta Sans', sans-serif" font-weight="800" class="zzz-item z2">z</text>
              <text x="172" y="32" fill="#0284c7" font-size="9" font-family="'Plus Jakarta Sans', sans-serif" font-weight="800" class="zzz-item z3">z</text>
            </g>

            <!-- Sparkles -->
            <g class="prop-sparkles">
              <polygon points="45,50 48,42 50,50 58,52 50,54 48,62 45,54 37,52" fill="#fbbf24"/>
              <polygon points="155,95 157,90 159,95 164,96 159,97 157,102 155,97 150,96" fill="#fbbf24"/>
            </g>
          </g>
        </svg>
      </div>

      <!-- Action Buttons -->
      <div class="brocco-actions">
        <button class="btn-nurture btn-nurture-water" onclick="nurtureBrocco('water')" title="Semprot air baku 5 detik">
          <span>💧</span>
          <span>Beri Minum</span>
        </button>
        <button class="btn-nurture btn-nurture-nutrient" onclick="nurtureBrocco('nutrient')" title="Beri nutrisi mikro">
          <span>🧪</span>
          <span>Beri Vitamin</span>
        </button>
        <button class="btn-nurture btn-nurture-breeze" onclick="nurtureBrocco('breeze')" title="Putar blower sirkulasi sejuk">
          <span>💨</span>
          <span>Kipas Semilir</span>
        </button>
        <button id="btnSoundToggle" class="btn-sound" onclick="toggleAudio()" title="Aktifkan/matikan suara">
          🔊 SFX: ON
        </button>
      </div>
    </div>

    <!-- Card 2: Companion Status Brief -->
    <div class="card companion-overview-card">
      <div class="card-header">
        <span class="card-label">Kondisi & Diary Tanaman</span>
        <span id="broccoMoodBadge" class="badge badge-optimal">PRIMA</span>
      </div>
      <div class="companion-stats-list">
        <div class="comp-stat-item">
          <span class="comp-stat-name">Skor Kenyamanan (PCS):</span>
          <span id="compValPcs" class="comp-stat-val" style="color:var(--pastel-mint-deep);">--%</span>
        </div>
        <div class="comp-stat-item">
          <span class="comp-stat-name">Sensasi Tanaman:</span>
          <span id="compValNeed" class="comp-stat-val">Homeostasis Seimbang</span>
        </div>
        <div class="comp-stat-item">
          <span class="comp-stat-name">Siklus Fotoperiode:</span>
          <span id="compValLight" class="comp-stat-val" style="color:var(--pastel-sky-deep);">Fase Terang (16 Jam)</span>
        </div>
        <div class="comp-stat-item">
          <span class="comp-stat-name">Respon Brocco:</span>
          <span id="compValResponse" class="comp-stat-val" style="color:#16a34a;">Tumbuh Segar & Sehat</span>
        </div>
      </div>
      <div class="comp-tip-box">
        <span style="font-weight:700;">💡 Tips Kebun:</span>
        <span id="compAgronomiTip" style="margin-left:4px;">Jaga suhu antara 18-22°C dan kelembapan 50-65% agar terhindar dari jamur Pythium.</span>
      </div>
    </div>
  </div>

  <!-- 4x Primary Telemetry Cards -->
  <div class="telemetry-grid">
    <div class="card">
      <div class="card-header">
        <span class="card-label">Suhu Udara</span>
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
        <span class="card-label">Kelembapan Udara</span>
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
        <span class="card-label">Kelembapan Media</span>
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
        <span class="card-label">Defisit Uap (VPD)</span>
        <span class="card-target">Tgt: 0.40 - 0.80 kPa</span>
      </div>
      <div class="card-value">
        <span id="valVpd">-.--</span>
        <span class="card-unit">kPa</span>
      </div>
      <div id="badgeVpd" class="badge badge-optimal">SEIMBANG</div>
    </div>
  </div>

  <!-- Secondary Instruments Grid -->
  <div class="instruments-grid">
    <!-- Plant Comfort Score (PCS) -->
    <div class="card">
      <div class="inst-title">
        <span>Plant Comfort Score (PCS)</span>
        <span id="valPcsText" style="color:var(--pastel-mint-deep); font-weight:800;">--%</span>
      </div>
      <div class="card-value" style="font-size:2.6rem; margin-bottom:2px;">
        <span id="valPcs">--</span><span class="card-unit">%</span>
      </div>
      <div class="pcs-meter">
        <div id="pcsBar" class="pcs-fill" style="width:100%; transform:scaleX(0);"></div>
      </div>
      <div style="font-size:0.75rem; color:var(--text-sub); display:flex; justify-content:space-between;">
        <span>Risiko Patogen</span>
        <span>Homeostasis Sempurna</span>
      </div>
    </div>

    <!-- Dual-Tank Reservoirs -->
    <div class="card">
      <div class="inst-title">
        <span>Kapasitas Tangki Air</span>
        <span style="font-size:0.75rem; color:var(--text-sub);">KONSUMSI MIST</span>
      </div>
      <div class="tank-row">
        <div class="tank-item">
          <div class="tank-header">
            <span>Tangki 1 (Air Baku Murni)</span>
            <span id="valTank1Text" style="color:var(--pastel-sky-deep);">1000 mL</span>
          </div>
          <div class="tank-bar">
            <div id="tank1Bar" class="tank-fill-1" style="width:100%; transform:scaleX(1);"></div>
          </div>
        </div>
        <div class="tank-item">
          <div class="tank-header">
            <span>Tangki 2 (Booster Nutrisi)</span>
            <span id="valTank2Text" style="color:var(--pastel-lavender);">1000 mL</span>
          </div>
          <div class="tank-bar">
            <div id="tank2Bar" class="tank-fill-2" style="width:100%; transform:scaleX(1);"></div>
          </div>
        </div>
      </div>
      <div style="margin-top:10px; display:flex; gap:8px;">
        <button class="btn" style="flex:1; padding:6px 10px; font-size:0.74rem;" onclick="sendControl('action=refill&tank=1')">Isi Tangki 1</button>
        <button class="btn" style="flex:1; padding:6px 10px; font-size:0.74rem;" onclick="sendControl('action=refill&tank=2')">Isi Tangki 2</button>
      </div>
    </div>

    <!-- Growth Phase Sequencer -->
    <div class="card">
      <div class="inst-title">
        <span>Pelacak Siklus Tumbuh</span>
        <span id="valDayBadge" class="badge badge-optimal">HARI 1 / 10</span>
      </div>
      <div style="margin-bottom:10px;">
        <div style="font-size:0.74rem; color:var(--text-sub); font-weight:600; text-transform:uppercase;">Regime Saat Ini</div>
        <div id="valPhaseName" style="font-size:1.05rem; font-weight:800; color:var(--pastel-mint-deep);">Germination Blackout</div>
        <div id="valPhaseDesc" style="font-size:0.78rem; color:var(--text-sub); margin-top:3px;">
          Lampu Mati • Target RH 60-70% • Semprot Air Baku
        </div>
      </div>
      <div class="pcs-meter" style="height:8px;">
        <div id="phaseProgress" class="pcs-fill" style="width:100%; background:#38bdf8; transform:scaleX(0.1);"></div>
      </div>
      <div style="display:flex; justify-content:space-between; font-size:0.74rem; color:var(--text-sub);">
        <span>Hari 1 (Semai)</span>
        <span>Hari 10 (Panen)</span>
      </div>
    </div>
  </div>

  <!-- Hardware Relays & Valve Matrix -->
  <div class="card" style="margin-bottom:24px;">
    <div class="inst-title" style="margin-bottom:8px;">
      <span>Matriks Perangkat Keras & Aktuator</span>
      <span style="font-size:0.74rem; color:var(--text-sub);">ACTIVE-LOW RELAY & SERVO</span>
    </div>
    <div class="matrix-grid">
      <div id="nodePeltier" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH1: Peltier</div>
        <div id="stPeltier" class="matrix-val">OFF</div>
      </div>
      <div id="nodeFan" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH2: Heatsink</div>
        <div id="stFan" class="matrix-val">OFF</div>
      </div>
      <div id="nodeBlower" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH3: Blower</div>
        <div id="stBlower" class="matrix-val">OFF</div>
      </div>
      <div id="nodeLight" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH4: Grow Light</div>
        <div id="stLight" class="matrix-val">OFF</div>
      </div>
      <div id="nodeSpray1" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH5: Semprot T1</div>
        <div id="stSpray1" class="matrix-val">OFF</div>
      </div>
      <div id="nodeSpray2" class="matrix-node">
        <div class="matrix-led"></div>
        <div class="matrix-name">CH6: Semprot T2</div>
        <div id="stSpray2" class="matrix-val">OFF</div>
      </div>
      <div id="nodeValve1" class="matrix-node">
        <div class="matrix-name">Katup Servo 1</div>
        <div id="stValve1" class="matrix-val" style="color:var(--pastel-sky-deep);">0° (TUTUP)</div>
      </div>
      <div id="nodeValve2" class="matrix-node">
        <div class="matrix-name">Katup Servo 2</div>
        <div id="stValve2" class="matrix-val" style="color:var(--pastel-mint-deep);">0° (TUTUP)</div>
      </div>
    </div>
  </div>

  <!-- Real-Time Trend Graph (SVG Canvas) -->
  <div class="chart-card">
    <div class="inst-title">
      <span>Grafik Riwayat Telemetri (20 Titik Terakhir)</span>
      <div style="display:flex; gap:16px; font-size:0.76rem; text-transform:none;">
        <span style="display:flex; align-items:center; gap:6px;">
          <span style="width:12px; height:3.5px; background:#16a34a; border-radius:2px; display:inline-block;"></span> Suhu (°C)
        </span>
        <span style="display:flex; align-items:center; gap:6px;">
          <span style="width:12px; height:3.5px; background:#0284c7; border-radius:2px; display:inline-block;"></span> RH (%)
        </span>
      </div>
    </div>
    <div class="chart-svg-container">
      <svg id="trendSvg" width="100%" height="100%" viewBox="0 0 800 160" preserveAspectRatio="none">
        <line x1="40" y1="20" x2="780" y2="20" stroke="#e6e0d3" stroke-dasharray="3"/>
        <line x1="40" y1="55" x2="780" y2="55" stroke="#e6e0d3" stroke-dasharray="3"/>
        <line x1="40" y1="90" x2="780" y2="90" stroke="#e6e0d3" stroke-dasharray="3"/>
        <line x1="40" y1="125" x2="780" y2="125" stroke="#e6e0d3" stroke-dasharray="3"/>
        
        <text x="5" y="24" fill="#94a3b8" font-size="10" font-family="'JetBrains Mono', monospace">50°/100%</text>
        <text x="5" y="75" fill="#94a3b8" font-size="10" font-family="'JetBrains Mono', monospace">25°/50%</text>
        <text x="5" y="130" fill="#94a3b8" font-size="10" font-family="'JetBrains Mono', monospace">0°/0%</text>

        <polyline id="rhPolyline" fill="none" stroke="#0284c7" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" points=""/>
        <polyline id="tempPolyline" fill="none" stroke="#16a34a" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" points=""/>
      </svg>
    </div>
  </div>

  <!-- Interactive Control Panel & Garden Log -->
  <div class="controls-grid">
    <div class="card">
      <div class="inst-title">
        <span>Panel Kendali Operasional</span>
        <span id="lblMode" class="badge badge-optimal">OTOMATIS</span>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Mode Operasi</div>
          <div style="font-size:0.75rem; color:var(--text-sub);">Beralih antara Kontrol Otomatis & Manual</div>
        </div>
        <button id="btnModeToggle" class="btn btn-mint" onclick="toggleMode()">MODE MANUAL</button>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Pilihan Fase Tumbuh</div>
          <div style="font-size:0.75rem; color:var(--text-sub);">Perkecambahan (Gelap) vs Vegetatif (Terang)</div>
        </div>
        <div style="display:flex; gap:8px;">
          <button class="btn" onclick="sendControl('action=phase&val=1')">Perkecambahan</button>
          <button class="btn" onclick="sendControl('action=phase&val=2')">Vegetatif</button>
        </div>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Tangki Aktif</div>
          <div style="font-size:0.75rem; color:var(--text-sub);">Pilih Jalur Utama Semprotan</div>
        </div>
        <div style="display:flex; gap:8px;">
          <button id="btnTank1" class="btn" onclick="sendControl('action=tank&val=1')">Tangki 1 (Air)</button>
          <button id="btnTank2" class="btn" onclick="sendControl('action=tank&val=2')">Tangki 2 (Nutrisi)</button>
        </div>
      </div>

      <div class="toggle-row">
        <div>
          <div class="toggle-label">Penyetel Hari Tumbuh</div>
          <div style="font-size:0.75rem; color:var(--text-sub);">Sesuaikan Penjejak Hari Budidaya</div>
        </div>
        <div style="display:flex; gap:8px; align-items:center;">
          <button class="btn" style="padding:5px 12px;" onclick="adjustDay(-1)">-1</button>
          <span id="lblDayCount" style="font-weight:800; font-size:0.9rem;">Hari 1</span>
          <button class="btn" style="padding:5px 12px;" onclick="adjustDay(1)">+1</button>
        </div>
      </div>

      <div class="toggle-row" style="border-bottom:none; margin-top:4px;">
        <div>
          <div class="toggle-label" style="color:var(--pastel-coral-deep);">Siram Darurat (Flush)</div>
          <div style="font-size:0.75rem; color:var(--text-sub);">Semprot 5 detik air baku Tangki 1</div>
        </div>
        <button class="btn btn-red" onclick="sendControl('action=flush')">⚡ Siram Darurat</button>
      </div>

      <!-- Manual Overrides -->
      <div id="manualControls" style="display:none; margin-top:14px; padding-top:14px; border-top:1px solid #eeeadf;">
        <div class="card-label" style="margin-bottom:8px; color:var(--pastel-butter-deep);">Kontrol Langsung (Mode Manual Aktif)</div>
        <div style="display:flex; flex-wrap:wrap; gap:8px;">
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=1')">Peltier</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=2')">Kipas</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=3')">Blower</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=4')">Grow Light</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=5')">Semprot T1</button>
          <button class="btn" onclick="sendControl('action=toggleRelay&ch=6')">Semprot T2</button>
        </div>
      </div>
    </div>

    <!-- Garden Log -->
    <div class="terminal-card">
      <div class="terminal-header">
        <span class="term-dot term-red"></span>
        <span class="term-dot term-yellow"></span>
        <span class="term-dot term-green"></span>
        <span style="margin-left:4px;">Catatan Kebun & Log Sistem</span>
      </div>
      <div id="terminalLogs" class="terminal-body">
        <div class="term-log"><span class="term-time">[00:00:00]</span><span class="term-msg">Inisialisasi Pengontrol Kabin Biosfer...</span></div>
      </div>
    </div>
  </div>

  <script>
    let isAutoMode = true;
    let currentDay = 1;
    let audioCtx = null;
    let audioMuted = (localStorage.getItem('brocco_audio_muted') === '1');

    function initAudio() {
      if (!audioCtx) {
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        if (AudioContextClass) audioCtx = new AudioContextClass();
      }
      if (audioCtx && audioCtx.state === 'suspended') {
        audioCtx.resume();
      }
    }

    function playChirp(type) {
      if (audioMuted) return;
      initAudio();
      if (!audioCtx) return;

      const now = audioCtx.currentTime;
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.connect(gain);
      gain.connect(audioCtx.destination);

      if (type === 'tap') {
        osc.type = 'sine';
        osc.frequency.setValueAtTime(520, now);
        osc.frequency.exponentialRampToValueAtTime(880, now + 0.12);
        gain.gain.setValueAtTime(0.2, now);
        gain.gain.exponentialRampToValueAtTime(0.001, now + 0.12);
        osc.start(now);
        osc.stop(now + 0.12);
      } else if (type === 'water') {
        osc.type = 'triangle';
        osc.frequency.setValueAtTime(400, now);
        osc.frequency.linearRampToValueAtTime(800, now + 0.08);
        osc.frequency.linearRampToValueAtTime(600, now + 0.16);
        osc.frequency.linearRampToValueAtTime(950, now + 0.24);
        gain.gain.setValueAtTime(0.25, now);
        gain.gain.exponentialRampToValueAtTime(0.01, now + 0.28);
        osc.start(now);
        osc.stop(now + 0.28);
      } else if (type === 'nutrient') {
        osc.type = 'sine';
        osc.frequency.setValueAtTime(440, now);
        osc.frequency.setValueAtTime(554, now + 0.07);
        osc.frequency.setValueAtTime(659, now + 0.14);
        osc.frequency.setValueAtTime(880, now + 0.21);
        gain.gain.setValueAtTime(0.2, now);
        gain.gain.exponentialRampToValueAtTime(0.01, now + 0.3);
        osc.start(now);
        osc.stop(now + 0.3);
      } else if (type === 'breeze') {
        osc.type = 'sine';
        osc.frequency.setValueAtTime(280, now);
        osc.frequency.exponentialRampToValueAtTime(140, now + 0.25);
        gain.gain.setValueAtTime(0.15, now);
        gain.gain.exponentialRampToValueAtTime(0.01, now + 0.25);
        osc.start(now);
        osc.stop(now + 0.25);
      }
    }

    function toggleAudio() {
      audioMuted = !audioMuted;
      localStorage.setItem('brocco_audio_muted', audioMuted ? '1' : '0');
      const btn = document.getElementById('btnSoundToggle');
      if (btn) btn.innerText = audioMuted ? '🔇 SFX: OFF' : '🔊 SFX: ON';
    }

    const cuteQuotes = [
      "Hai! Senang dirawat sama kamu! 🌱",
      "Suhu sejuk 20°C ini pas banget buat fotosintesis!",
      "Daunku makin hijau dan renyah berkat cahaya LED!",
      "Jangan lupa cek kadar nutrisi di Tangki 2 ya!",
      "Brokoli microgreens kaya antioksidan sulforaphane lho!"
    ];

    let speechLockUntil = 0;
    function setBroccoSpeech(text, force = false) {
      const now = Date.now();
      if (!force && now < speechLockUntil) return;
      const bubble = document.getElementById('broccoSpeech');
      if (bubble && bubble.innerText !== text) {
        bubble.style.opacity = '0';
        setTimeout(() => {
          bubble.innerText = text;
          bubble.style.opacity = '1';
        }, 120);
      }
      if (force) speechLockUntil = now + 4000;
    }

    function handleBroccoTap() {
      initAudio();
      playChirp('tap');
      const mascot = document.getElementById('broccoMascot');
      if (mascot) {
        mascot.classList.remove('brocco-bounce');
        void mascot.offsetWidth;
        mascot.classList.add('brocco-bounce');
      }
      const randomQuote = cuteQuotes[Math.floor(Math.random() * cuteQuotes.length)];
      setBroccoSpeech(randomQuote, true);
    }

    function nurtureBrocco(action) {
      initAudio();
      if (action === 'water') {
        playChirp('water');
        setBroccoSpeech("Aah, segaar! Semprotan air baku Tangki 1 aktif!", true);
        sendControl('action=flush');
      } else if (action === 'nutrient') {
        playChirp('nutrient');
        setBroccoSpeech("Yummy! Booster nutrisi mikro Tangki 2 diserap!", true);
        sendControl('action=toggleRelay&ch=6');
      } else if (action === 'breeze') {
        playChirp('breeze');
        setBroccoSpeech("Wussshh! Hembusan angin blower bikin daun sejuk!", true);
        sendControl('action=toggleRelay&ch=3');
      }
    }

    function updateBroccoMood(d) {
      const mascot = document.getElementById('broccoMascot');
      const badge = document.getElementById('broccoMoodBadge');
      const compPcs = document.getElementById('compValPcs');
      const compNeed = document.getElementById('compValNeed');
      const compLight = document.getElementById('compValLight');
      const compResp = document.getElementById('compValResponse');
      const compTip = document.getElementById('compAgronomiTip');

      if (compPcs) compPcs.innerText = Math.round(d.pcs) + '%';
      if (compLight) {
        compLight.innerText = d.relays.light ? 'Fase Terang (16 Jam)' : 'Fase Gelap / Istirahat';
      }

      if (!mascot) return;

      mascot.classList.remove('mood-happy', 'mood-cold', 'mood-heat', 'mood-heat-crit', 'mood-thirsty', 'mood-stagnant', 'mood-sleep', 'mood-panic');

      if (d.status === 'SAFEMODE') {
        mascot.classList.add('mood-panic');
        if (badge) { badge.className = 'badge badge-safe'; badge.innerText = 'PANIK'; }
        if (compNeed) compNeed.innerText = 'Pemulihan Sensor Segera';
        if (compResp) compResp.innerText = 'Gemetar Takut';
        if (compTip) compTip.innerText = 'Periksa kabel koneksi sensor DHT22 GPIO 14!';
        setBroccoSpeech("Waduh sensor bermasalah! Sistem masuk Safe Mode darurat!");
      } else if (!d.relays.light && d.phase === 1) {
        mascot.classList.add('mood-sleep');
        if (badge) { badge.className = 'badge badge-optimal'; badge.innerText = 'TIDUR'; }
        if (compNeed) compNeed.innerText = 'Kegelapan Perkecambahan';
        if (compResp) compResp.innerText = 'Tidur Pulas (Zzz)';
        if (compTip) compTip.innerText = 'Fase Blackout (Hari 1-3): Jangan nyalakan lampu agar batang kecambah memanjang.';
        setBroccoSpeech("Zzz... Fase perkecambahan gelap, aku sedang tidur nyenyak...");
      } else if (!d.relays.light) {
        mascot.classList.add('mood-sleep');
        if (badge) { badge.className = 'badge badge-optimal'; badge.innerText = 'TIDUR'; }
        if (compNeed) compNeed.innerText = 'Istirahat Malam (8 Jam)';
        if (compResp) compResp.innerText = 'Respirasi Malam';
        if (compTip) compTip.innerText = 'Siklus gelap 8 jam penting untuk respirasi metabolisme tanaman.';
        setBroccoSpeech("Zzz... Siklus fotoperiode malam, istirahat dulu ya...");
      } else if (d.temp > 24.0) {
        mascot.classList.add('mood-heat-crit');
        if (badge) { badge.className = 'badge badge-kritis'; badge.innerText = 'OVERHEAT'; }
        if (compNeed) compNeed.innerText = 'Pendinginan Darurat (<22°C)';
        if (compResp) compResp.innerText = 'Terengah-engah Kepanasan';
        if (compTip) compTip.innerText = 'Bahaya kritis! Suhu > 24°C memicu patogen busuk akar Pythium!';
        setBroccoSpeech("Aduh kepanasan (>24°C)! Bahaya busuk akar Pythium! Dinginkan segera!");
      } else if (d.temp > 22.0) {
        mascot.classList.add('mood-heat');
        if (badge) { badge.className = 'badge badge-waspada'; badge.innerText = 'GERAH'; }
        if (compNeed) compNeed.innerText = 'Sirkulasi & Pendinginan';
        if (compResp) compResp.innerText = 'Sedikit Gerah';
        if (compTip) compTip.innerText = 'Peltier CH1 aktif untuk menurunkan suhu ke rentang ideal 18-22°C.';
        setBroccoSpeech("Agak gerah nih (>22°C). Peltier sedang mendinginkan kabin!");
      } else if (d.temp < 18.0) {
        mascot.classList.add('mood-cold');
        if (badge) { badge.className = 'badge badge-waspada'; badge.innerText = 'MENGGIGIL'; }
        if (compNeed) compNeed.innerText = 'Suhu Lebih Hangat (>18°C)';
        if (compResp) compResp.innerText = 'Menggigil Kedinginan';
        if (compTip) compTip.innerText = 'Suhu < 18°C memperlambat fotosintesis brokoli microgreens.';
        setBroccoSpeech("Brrr dingin banget (<18°C)! Aku menggigil kedinginan!");
      } else if (d.soil < 45.0) {
        mascot.classList.add('mood-thirsty');
        if (badge) { badge.className = 'badge badge-waspada'; badge.innerText = 'HAUS'; }
        if (compNeed) compNeed.innerText = 'Hidrasi Media Tanam';
        if (compResp) compResp.innerText = 'Layu Kehausan';
        if (compTip) compTip.innerText = 'Semprot air baku Tangki 1 untuk menaikkan kelembapan media tanam ke 45-70%.';
        setBroccoSpeech("Tanahku kering kerontang (<45%)! Butuh semprotan air!");
      } else if (d.rh > 65.0) {
        mascot.classList.add('mood-stagnant');
        if (badge) { badge.className = 'badge badge-waspada'; badge.innerText = 'PEKAT'; }
        if (compNeed) compNeed.innerText = 'Sirkulasi Udara Blower';
        if (compResp) compResp.innerText = 'Gerah Lembap';
        if (compTip) compTip.innerText = 'Kelembapan > 65% menciptakan lapisan batas stagnant; blower aktif memecah embun.';
        setBroccoSpeech("Kelembapan pekat (>65%). Blower aktif mengusir embun di daun!");
      } else {
        mascot.classList.add('mood-happy');
        if (badge) { badge.className = 'badge badge-optimal'; badge.innerText = 'PRIMA'; }
        if (compNeed) compNeed.innerText = 'Pertahankan Kondisi Saat Ini';
        if (compResp) compResp.innerText = 'Tumbuh Riang & Sehat';
        if (compTip) compTip.innerText = 'Lingkungan biosfer dalam kondisi homeostasis sempurna.';
        setBroccoSpeech("Kondisi biosfer prima! Suhu sejuk dan aku bertumbuh cepat!");
      }
    }

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
      document.getElementById('valIp').innerText = d.ip || 'ESP32';
      document.getElementById('valUptime').innerText = d.uptime || '00:00:00';

      const stBadge = document.getElementById('badgeStatus');
      const stText = document.getElementById('badgeStatusText');
      stBadge.className = 'badge ' + (d.status === 'OPTIMAL' ? 'badge-optimal' : (d.status === 'WASPADA' ? 'badge-waspada' : (d.status === 'SAFEMODE' ? 'badge-safe' : 'badge-kritis')));
      stText.innerText = d.status === 'OPTIMAL' ? 'KONDISI PRIMA' : d.status;

      document.getElementById('valTemp').innerText = d.temp.toFixed(1);
      document.getElementById('valRh').innerText = d.rh.toFixed(1);
      document.getElementById('valSoil').innerText = d.soil.toFixed(1);
      document.getElementById('valVpd').innerText = d.vpd.toFixed(2);

      updateCardBadge('badgeTemp', d.temp >= 18.0 && d.temp <= 22.0, d.temp > 24.0, 'NORMAL', 'WASPADA', 'KRITIS');
      updateCardBadge('badgeRh', d.rh >= 50.0 && d.rh <= 65.0, d.rh > 75.0, 'OPTIMAL', 'WASPADA', 'PEKAT');
      updateCardBadge('badgeSoil', d.soil >= 45.0 && d.soil <= 70.0, d.soil < 35.0, 'LEMBAP PAS', 'WASPADA', 'KERING');
      updateCardBadge('badgeVpd', d.vpd >= 0.40 && d.vpd <= 0.80, d.vpd < 0.20 || d.vpd > 1.20, 'SEIMBANG', 'DRIFT', 'KRITIS');

      document.getElementById('valPcs').innerText = Math.round(d.pcs);
      document.getElementById('valPcsText').innerText = Math.round(d.pcs) + '%';
      document.getElementById('pcsBar').style.transform = `scaleX(${Math.min(1, Math.max(0, d.pcs / 100))})`;

      document.getElementById('valTank1Text').innerText = Math.round(d.tank1Vol) + ' mL';
      document.getElementById('valTank2Text').innerText = Math.round(d.tank2Vol) + ' mL';
      document.getElementById('tank1Bar').style.transform = `scaleX(${Math.min(1, Math.max(0, d.tank1Vol / 1000))})`;
      document.getElementById('tank2Bar').style.transform = `scaleX(${Math.min(1, Math.max(0, d.tank2Vol / 1000))})`;

      currentDay = d.day;
      document.getElementById('valDayBadge').innerText = 'HARI ' + d.day + ' / 10';
      document.getElementById('lblDayCount').innerText = 'Hari ' + d.day;
      document.getElementById('phaseProgress').style.transform = `scaleX(${Math.min(1, Math.max(0, d.day / 10))})`;

      if (d.phase === 1) {
        document.getElementById('valPhaseName').innerText = 'Germination Blackout';
        document.getElementById('valPhaseDesc').innerText = 'Lampu Mati • Target RH 60-70% • Semprot Air Baku';
      } else {
        document.getElementById('valPhaseName').innerText = 'Fase Autotrofik';
        document.getElementById('valPhaseDesc').innerText = '16 Jam Terang • Target VPD 0.6-0.8 kPa • Semprot Nutrisi';
      }

      setNodeActive('nodePeltier', 'stPeltier', d.relays.peltier, false);
      setNodeActive('nodeFan', 'stFan', d.relays.fan, true);
      setNodeActive('nodeBlower', 'stBlower', d.relays.blower, false);
      setNodeActive('nodeLight', 'stLight', d.relays.light, false);
      setNodeActive('nodeSpray1', 'stSpray1', d.relays.spray1, false);
      setNodeActive('nodeSpray2', 'stSpray2', d.relays.spray2, false);

      document.getElementById('stValve1').innerText = d.valves.v1 + '° ' + (d.valves.v1 > 0 ? '(BUKA)' : '(TUTUP)');
      document.getElementById('stValve2').innerText = d.valves.v2 + '° ' + (d.valves.v2 > 0 ? '(BUKA)' : '(TUTUP)');

      isAutoMode = d.autoMode;
      document.getElementById('lblMode').innerText = isAutoMode ? 'OTOMATIS' : 'MANUAL';
      document.getElementById('lblMode').className = 'badge ' + (isAutoMode ? 'badge-optimal' : 'badge-waspada');
      document.getElementById('btnModeToggle').innerText = isAutoMode ? 'MODE MANUAL' : 'MODE OTOMATIS';
      document.getElementById('manualControls').style.display = isAutoMode ? 'none' : 'block';

      document.getElementById('btnTank1').className = 'btn ' + (d.activeTank === 1 ? 'btn-mint' : '');
      document.getElementById('btnTank2').className = 'btn ' + (d.activeTank === 2 ? 'btn-mint' : '');

      if (d.history && d.history.temp && d.history.rh) {
        renderSvgHistory(d.history.temp, d.history.rh);
      }

      if (d.logs && d.logs.length > 0) {
        let logHtml = '';
        d.logs.forEach(l => {
          logHtml += `<div class="term-log"><span class="term-time">[${l.t}]</span><span class="term-msg">${l.m}</span></div>`;
        });
        document.getElementById('terminalLogs').innerHTML = logHtml;
      }

      updateBroccoMood(d);
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
      const height = 110;
      const startX = 40;
      const startY = 20;

      let tempPts = [];
      let rhPts = [];

      const n = Math.max(temps.length, rhs.length);
      if (n < 2) return;

      for (let i = 0; i < n; i++) {
        const x = startX + (i / (n - 1)) * width;
        const tVal = Math.min(50, Math.max(0, temps[i] || 0));
        const yTemp = (startY + height) - (tVal / 50) * height;
        tempPts.push(`${x.toFixed(1)},${yTemp.toFixed(1)}`);

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

    const initBtn = document.getElementById('btnSoundToggle');
    if (initBtn) initBtn.innerText = audioMuted ? '🔇 SFX: OFF' : '🔊 SFX: ON';

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
