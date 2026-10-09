/*
  SMART SOLAR BANK - Final version
  ==================================================================
  ESP32 DevKit V1 + INA219 battery monitor with a Wi-Fi dashboard

  Board in Arduino IDE : "ESP32 Dev Module"  (port used: COM11)
  Library              : "Adafruit INA219"  (Library Manager; also installs Adafruit BusIO)

  FINAL WIRING
  ------------------------------------------------------------------
  Battery pack (2S3P, 6 x ICR18650 2500 mAh, about 6.0 - 8.4 V)
    Pack +            -> INA219 VIN+   (bottom screw of green terminal)
    INA219 VIN-       -> LM2596 IN+    (top screw of green terminal)
    Pack -            -> LM2596 IN-, INA219 GND, ESP32 GND  (one common ground)
    LM2596 OUT+/OUT-  -> 5 V load (LED / USB fan), output set to 5.0 V first
  INA219 VCC -> ESP32 3V3     INA219 GND -> ESP32 GND
  INA219 SDA -> ESP32 D21     INA219 SCL -> ESP32 D22
  ESP32 is powered from the laptop USB cable.

  NEVER connect pack - to VIN-. That puts the battery straight across
  the 0.1 ohm shunt (a dead short) - the sensor then reads 3200 mA.

  USE
  ------------------------------------------------------------------
  Phone joins Wi-Fi "SmartSolarBank" (password solar1234), mobile data OFF,
  then open http://192.168.4.1
  Serial Monitor at 115200 baud prints the same readings every second.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// ---------------- Settings you may need to change ----------------
const char* AP_SSID = "SmartSolarBank";
const char* AP_PASS = "solar1234";          // must be 8+ characters

const int   CELLS_IN_SERIES     = 2;        // 2S pack measured at 7.18 V
const float PACK_CAPACITY_MAH   = 7500.0;   // 3 parallel x 2500 mAh
const float PACK_RESISTANCE_OHM = 2.9;      // measured: 0.31 V drop at 107 mA
const float CURRENT_SIGN        = 1.0;      // -1.0 if CHARGING/DISCHARGING look swapped
const float IDLE_BAND_MA        = 20.0;     // |current| below this = IDLE
const float LOW_CELL_V          = 3.30;     // per-cell rest voltage for LOW BATTERY alert
const float LM2596_MIN_IN_V     = 6.5;      // below this the LM2596 cannot hold 5 V
const float SENSOR_MAX_MA       = 3190.0;   // INA219 (32V_2A calibration) saturates ~3200 mA
const float NO_BATTERY_V        = 1.5;      // below this the sensor is floating / not connected

// ---------------- Pins ----------------
const int SDA_PIN = 21;
const int SCL_PIN = 22;

Adafruit_INA219 ina219;
WebServer server(80);

// ---------------- Live values ----------------
bool   inaOK       = false;
float  battV       = 0;    // smoothed pack terminal voltage
float  currentmA   = 0;    // smoothed current, + = discharging, - = charging
float  powerW      = 0;
float  restV       = 0;    // estimated open-circuit voltage (IR compensated)
int    soc         = 0;
double whIn        = 0;    // energy put into the pack
double whOut       = 0;    // energy taken out of the pack
bool   firstSample = true;

unsigned long lastSampleMs = 0;
unsigned long lastPrintMs  = 0;
unsigned long lastRetryMs  = 0;

const float SMOOTH = 0.3;   // 0..1, higher = faster response, lower = smoother

// Li-ion resting voltage per cell -> state of charge (%)
int socFromCellVoltage(float v) {
  const float vt[] = {3.30, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20};
  const int   pt[] = {0,    10,   20,   35,   50,   65,   80,   90,   100};
  if (v <= vt[0]) return 0;
  if (v >= vt[8]) return 100;
  for (int k = 0; k < 8; k++) {
    if (v < vt[k + 1]) {
      return pt[k] + (int)((pt[k + 1] - pt[k]) * (v - vt[k]) / (vt[k + 1] - vt[k]));
    }
  }
  return 100;
}

bool batteryPresent()  { return inaOK && battV >= NO_BATTERY_V; }
bool sensorSaturated() { return inaOK && fabs(currentmA) >= SENSOR_MAX_MA; }

const char* stateText() {
  if (!inaOK)                         return "SENSOR NOT FOUND";
  if (sensorSaturated())              return "OVERCURRENT - DISCONNECT";
  if (!batteryPresent())              return "NO BATTERY";
  if (currentmA < -IDLE_BAND_MA)      return "CHARGING";
  if (restV / CELLS_IN_SERIES < LOW_CELL_V) return "LOW BATTERY";
  if (currentmA >  IDLE_BAND_MA)      return "DISCHARGING";
  return "IDLE";
}

const char* alertText() {
  if (!inaOK)            return "Check INA219 wires: VCC-3V3, GND, SDA-D21, SCL-D22";
  if (sensorSaturated()) return "Current above sensor limit - check pack - is not on VIN-";
  if (!batteryPresent()) return "No battery voltage - check pack + on VIN+ and pack - on GND";
  if (restV / CELLS_IN_SERIES < LOW_CELL_V) return "Battery low - recharge before further use";
  if (currentmA > IDLE_BAND_MA && battV < LM2596_MIN_IN_V)
                         return "Pack below 6.5 V - LM2596 output will drop below 5 V";
  return "";
}

float hoursLeft() {
  if (!batteryPresent() || sensorSaturated() || currentmA <= IDLE_BAND_MA) return -1;
  float remainingmAh = PACK_CAPACITY_MAH * soc / 100.0;
  return remainingmAh / currentmA;
}

bool startSensor() {
  if (!ina219.begin()) return false;
  ina219.setCalibration_32V_2A();
  return true;
}

void sample() {
  unsigned long now = millis();
  float dtHours = firstSample ? 0 : (now - lastSampleMs) / 3600000.0;
  lastSampleMs = now;

  if (!inaOK) return;

  // Detect a sensor that was unplugged while running
  Wire.beginTransmission(0x40);
  if (Wire.endTransmission() != 0) {
    inaOK = false;
    Serial.println("INA219 lost - retrying...");
    return;
  }

  float busV  = ina219.getBusVoltage_V();
  float shuntV = ina219.getShuntVoltage_mV() / 1000.0;
  float rawV   = busV + shuntV;                       // voltage at VIN+ = pack terminal
  float rawI   = CURRENT_SIGN * ina219.getCurrent_mA();

  if (firstSample) { battV = rawV; currentmA = rawI; firstSample = false; }
  else {
    battV     += SMOOTH * (rawV - battV);
    currentmA += SMOOTH * (rawI - currentmA);
  }
  powerW = battV * currentmA / 1000.0;

  // Remove the voltage drop caused by the pack's internal + wiring resistance
  restV = battV + (currentmA / 1000.0) * PACK_RESISTANCE_OHM;
  soc   = batteryPresent() ? socFromCellVoltage(restV / CELLS_IN_SERIES) : 0;

  if (batteryPresent() && !sensorSaturated()) {
    if (powerW < 0) whIn  += -powerW * dtHours;
    else            whOut +=  powerW * dtHours;
  }
}

// ---------------- Web dashboard ----------------
const char PAGE[] = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Solar Bank</title>
<style>
body{font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:16px}
h1{font-size:20px;margin:0 0 12px}
.g{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.c{background:#1e293b;border-radius:10px;padding:12px}
.l{font-size:12px;color:#94a3b8}
.v{font-size:22px;margin-top:4px}
#st{font-size:18px;padding:10px;border-radius:10px;text-align:center;margin-bottom:10px;background:#334155}
#al{display:none;background:#7f1d1d;color:#fecaca;padding:10px;border-radius:10px;margin-bottom:10px;font-size:14px}
.bar{height:14px;background:#334155;border-radius:7px;overflow:hidden;margin-top:8px}
#fill{height:100%;width:0;background:#22c55e;transition:width .5s}
button{margin-top:12px;width:100%;padding:10px;border:0;border-radius:10px;background:#334155;color:#e2e8f0;font-size:14px}
</style></head><body>
<h1>Smart Solar Bank</h1>
<div id="st">Connecting...</div>
<div id="al"></div>
<div class="c" style="margin-bottom:10px">
  <div class="l">Battery charge (estimated)</div>
  <div class="v" id="soc">--</div>
  <div class="bar"><div id="fill"></div></div>
</div>
<div class="g">
  <div class="c"><div class="l">Pack voltage</div><div class="v" id="volt">--</div></div>
  <div class="c"><div class="l">Per cell (rest)</div><div class="v" id="cell">--</div></div>
  <div class="c"><div class="l">Current</div><div class="v" id="amp">--</div></div>
  <div class="c"><div class="l">Power</div><div class="v" id="pow">--</div></div>
  <div class="c"><div class="l">Energy in</div><div class="v" id="win">--</div></div>
  <div class="c"><div class="l">Energy out</div><div class="v" id="wout">--</div></div>
  <div class="c"><div class="l">Time left at this load</div><div class="v" id="hrs">--</div></div>
  <div class="c"><div class="l">Running for</div><div class="v" id="up">--</div></div>
</div>
<button onclick="fetch('/reset')">Reset energy counters</button>
<script>
const $ = id => document.getElementById(id);
async function update(){
  try{
    const d = await (await fetch('/data')).json();
    $('st').textContent = d.state;
    $('st').style.background = d.state=='CHARGING' ? '#166534' : d.state=='DISCHARGING' ? '#9a3412'
                             : d.state=='IDLE' ? '#334155' : '#7f1d1d';
    $('al').style.display = d.alert ? 'block' : 'none';
    $('al').textContent = d.alert;
    $('soc').textContent  = d.soc + ' %';
    $('fill').style.width = d.soc + '%';
    $('fill').style.background = d.soc < 20 ? '#ef4444' : d.soc < 50 ? '#f59e0b' : '#22c55e';
    $('volt').textContent = d.v.toFixed(2) + ' V';
    $('cell').textContent = d.cell.toFixed(2) + ' V';
    $('amp').textContent  = Math.abs(d.ma).toFixed(0) + ' mA';
    $('pow').textContent  = Math.abs(d.w).toFixed(2) + ' W';
    $('win').textContent  = d.whin.toFixed(3) + ' Wh';
    $('wout').textContent = d.whout.toFixed(3) + ' Wh';
    $('hrs').textContent  = d.hrs > 0 ? d.hrs.toFixed(1) + ' h' : '--';
    const m = Math.floor(d.up/60), s = d.up%60;
    $('up').textContent = m + 'm ' + s + 's';
  }catch(e){ $('st').textContent = 'Connection lost'; }
}
setInterval(update, 1000); update();
</script></body></html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", PAGE); }

void handleData() {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"v\":%.3f,\"cell\":%.3f,\"ma\":%.1f,\"w\":%.3f,\"soc\":%d,"
           "\"state\":\"%s\",\"alert\":\"%s\",\"whin\":%.4f,\"whout\":%.4f,\"hrs\":%.1f,\"up\":%lu}",
           inaOK ? "true" : "false", battV, restV / CELLS_IN_SERIES, currentmA, powerW, soc,
           stateText(), alertText(), whIn, whOut, hoursLeft(), millis() / 1000UL);
  server.send(200, "application/json", buf);
}

void handleReset() {
  whIn = 0; whOut = 0;
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nSmart Solar Bank starting...");

  Wire.begin(SDA_PIN, SCL_PIN);
  inaOK = startSensor();
  Serial.println(inaOK ? "INA219 found."
                       : "INA219 NOT found - check VCC->3V3, GND, SDA->D21, SCL->D22");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Dashboard: join Wi-Fi '");
  Serial.print(AP_SSID);
  Serial.print("' and open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/reset", handleReset);
  server.begin();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // Retry the sensor every 3 s if it was not found or got disconnected
  if (!inaOK && now - lastRetryMs >= 3000) {
    lastRetryMs = now;
    inaOK = startSensor();
    if (inaOK) { firstSample = true; Serial.println("INA219 reconnected."); }
  }

  if (firstSample || now - lastSampleMs >= 500) sample();

  if (now - lastPrintMs >= 1000) {
    lastPrintMs = now;
    if (inaOK) {
      Serial.printf("V=%.3f V  Cell=%.3f V  I=%.1f mA  P=%.3f W  SoC=%d%%  %s  In=%.4f Wh  Out=%.4f Wh\n",
                    battV, restV / CELLS_IN_SERIES, currentmA, powerW, soc, stateText(), whIn, whOut);
      const char* a = alertText();
      if (a[0]) { Serial.print("  ALERT: "); Serial.println(a); }
    } else {
      Serial.println("Waiting for INA219...");
    }
  }
}
