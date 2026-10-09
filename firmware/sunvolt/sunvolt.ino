/*
  SUNVOLT - Smart Solar Bank with live graphs
  ==================================================================
  ESP32 DevKit V1 + INA219 (battery) + optional 2nd INA219 (solar)
  Live dashboard with tiles and 4 real-time graphs, served by the ESP32
  over its own Wi-Fi. No internet needed: the graphs are drawn by the
  page itself, no chart library is downloaded.

  Board in Arduino IDE : "ESP32 Dev Module"
  Library              : "Adafruit INA219" (Library Manager; also installs Adafruit BusIO)

  WIRING - battery side (sensor #1, address 0x40, no jumper)
  ------------------------------------------------------------------
    Battery red (+)   -> INA219#1 VIN+
    INA219#1 VIN-     -> power bank board B+  AND  CN3791 BAT+
    Battery black (-) -> power bank B-, CN3791 BAT-, INA219#1 GND
  WIRING - solar side (sensor #2, OPTIONAL, address 0x41)
  ------------------------------------------------------------------
    Solder-bridge the A0 pads on the 2nd INA219 board  -> address 0x41
    Panel (+)         -> INA219#2 VIN+
    INA219#2 VIN-     -> CN3791 IN+ (SOLAR+)
    Panel (-)         -> CN3791 IN- (SOLAR-)
  BOTH sensors share the I2C bus (wire them in parallel):
    VCC -> ESP32 3V3   GND -> ESP32 GND   SDA -> D21   SCL -> D22
  Without sensor #2 everything works; solar tiles show "no sensor".

  USE
  ------------------------------------------------------------------
  Phone joins Wi-Fi "SmartSolarBank" (password solar1234), mobile data OFF,
  open http://192.168.4.1
  Graphs keep the last 30 minutes (one point every 5 s) and fill in
  immediately when the page is opened. "Download CSV" saves the history.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

// ---------------- Settings ----------------
const char* AP_SSID = "SmartSolarBank";
const char* AP_PASS = "solar1234";          // 8+ characters

const int   CELLS_IN_SERIES     = 1;        // 1S pack, 3.7 V nominal
const float PACK_CAPACITY_MAH   = 5200.0;   // printed on the pack
const float PACK_RESISTANCE_OHM = 0.15;     // re-measure: (V idle - V loaded) / A
const float CURRENT_SIGN        = 1.0;      // -1.0 if CHARGING / DISCHARGING look swapped
const float SOLAR_SIGN          = 1.0;      // -1.0 if solar current reads negative in sun
const float IDLE_BAND_MA        = 20.0;     // |battery current| below this = IDLE
const float SOLAR_ACTIVE_MA     = 20.0;     // solar current above this = sun is charging
const float LOW_CELL_V          = 3.30;     // per-cell rest voltage for LOW BATTERY
const float SENSOR_MAX_MA       = 3190.0;   // INA219 32V_2A calibration saturates ~3200 mA
const float NO_BATTERY_V        = 1.5;      // below this the battery input is floating

const uint8_t BATT_ADDR  = 0x40;
const uint8_t SOLAR_ADDR = 0x41;
const int SDA_PIN = 21, SCL_PIN = 22;

const unsigned long SAMPLE_MS = 500;
const unsigned long PRINT_MS  = 1000;
const unsigned long HIST_MS   = 5000;       // one graph point every 5 s
const int           HIST_N    = 360;        // 360 x 5 s = 30 minutes

const float SMOOTH = 0.3;                   // 0..1, higher = faster, lower = smoother

// ---------------- Objects and live values ----------------
Adafruit_INA219 inaBatt(BATT_ADDR);
Adafruit_INA219 inaSolar(SOLAR_ADDR);
WebServer server(80);

bool  battOK = false, solarOK = false;
float battV = 0, currentmA = 0, powerW = 0, restV = 0;
int   soc = 0;
float solarV = 0, solarmA = 0, solarW = 0;
double whIn = 0, whOut = 0, whSolar = 0;
bool  firstBatt = true, firstSolar = true;

unsigned long lastSampleMs = 0, lastPrintMs = 0, lastRetryMs = 0, lastHistMs = 0;

struct Point {
  uint32_t t;                 // seconds since start
  float bv, bi, bw;           // battery V, mA (+ out / - in), W
  float sv, si, sw;           // solar V, mA, W (NAN when no sensor)
  uint8_t soc;
};
Point hist[HIST_N];
int histHead = 0, histCount = 0;

// ---------------- Helpers ----------------
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

bool present(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool startSensor(Adafruit_INA219 &s, uint8_t addr) {
  if (!present(addr)) return false;
  if (!s.begin()) return false;
  s.setCalibration_32V_2A();
  return true;
}

bool batteryPresent()  { return battOK && battV >= NO_BATTERY_V; }
bool sensorSaturated() { return battOK && fabs(currentmA) >= SENSOR_MAX_MA; }
bool solarActive()     { return solarOK && solarmA > SOLAR_ACTIVE_MA; }

const char* stateText() {
  if (!battOK)                          return "SENSOR NOT FOUND";
  if (sensorSaturated())                return "OVERCURRENT - DISCONNECT";
  if (!batteryPresent())                return "NO BATTERY";
  if (currentmA < -IDLE_BAND_MA)        return solarActive() ? "SOLAR CHARGING" : "CHARGING";
  if (restV / CELLS_IN_SERIES < LOW_CELL_V) return "LOW BATTERY";
  if (currentmA >  IDLE_BAND_MA)        return "DISCHARGING";
  return "IDLE";
}

const char* alertText() {
  if (!battOK)            return "Check battery INA219 wires: VCC-3V3, GND, SDA-D21, SCL-D22";
  if (sensorSaturated())  return "Current above sensor limit - check battery - is not on VIN-";
  if (!batteryPresent())  return "No battery voltage - check battery + on VIN+ and battery - on GND";
  if (currentmA >= -IDLE_BAND_MA && restV / CELLS_IN_SERIES < LOW_CELL_V)
                          return "Battery low - recharge before further use";
  return "";
}

float hoursLeft() {
  if (!batteryPresent() || sensorSaturated() || currentmA <= IDLE_BAND_MA) return -1;
  return (PACK_CAPACITY_MAH * soc / 100.0) / currentmA;
}

// ---------------- Sampling ----------------
void sampleBattery(float dtHours) {
  if (!battOK) return;
  if (!present(BATT_ADDR)) { battOK = false; Serial.println("Battery INA219 lost - retrying..."); return; }

  float rawV = inaBatt.getBusVoltage_V() + inaBatt.getShuntVoltage_mV() / 1000.0;
  float rawI = CURRENT_SIGN * inaBatt.getCurrent_mA();
  if (firstBatt) { battV = rawV; currentmA = rawI; firstBatt = false; }
  else { battV += SMOOTH * (rawV - battV); currentmA += SMOOTH * (rawI - currentmA); }

  powerW = battV * currentmA / 1000.0;
  restV  = battV + (currentmA / 1000.0) * PACK_RESISTANCE_OHM;
  soc    = batteryPresent() ? socFromCellVoltage(restV / CELLS_IN_SERIES) : 0;

  if (batteryPresent() && !sensorSaturated()) {
    if (powerW < 0) whIn  += -powerW * dtHours;
    else            whOut +=  powerW * dtHours;
  }
}

void sampleSolar(float dtHours) {
  if (!solarOK) return;
  if (!present(SOLAR_ADDR)) { solarOK = false; Serial.println("Solar INA219 lost - retrying..."); return; }

  float rawV = inaSolar.getBusVoltage_V() + inaSolar.getShuntVoltage_mV() / 1000.0;
  float rawI = SOLAR_SIGN * inaSolar.getCurrent_mA();
  if (rawI < 0) rawI = 0;                                  // a charger never sends current back to the panel
  if (firstSolar) { solarV = rawV; solarmA = rawI; firstSolar = false; }
  else { solarV += SMOOTH * (rawV - solarV); solarmA += SMOOTH * (rawI - solarmA); }

  solarW = solarV * solarmA / 1000.0;
  whSolar += solarW * dtHours;
}

void sample() {
  unsigned long now = millis();
  float dtHours = (lastSampleMs == 0) ? 0 : (now - lastSampleMs) / 3600000.0;
  lastSampleMs = now;
  sampleBattery(dtHours);
  sampleSolar(dtHours);
}

void recordPoint() {
  Point &p = hist[histHead];
  p.t   = millis() / 1000UL;
  p.bv  = battOK ? battV : NAN;
  p.bi  = battOK ? currentmA : NAN;
  p.bw  = battOK ? powerW : NAN;
  p.sv  = solarOK ? solarV : NAN;
  p.si  = solarOK ? solarmA : NAN;
  p.sw  = solarOK ? solarW : NAN;
  p.soc = (uint8_t)soc;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

// ---------------- Web page ----------------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SUNVOLT</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:16px;background:#0f1722;color:#e2e8f0;font-family:Arial,Helvetica,sans-serif}
h1{font-size:22px;margin:0}
.sub{color:#94a3b8;font-size:13px;margin:4px 0 14px}
#st{padding:10px 14px;border-radius:10px;background:#334155;font-weight:bold;margin-bottom:8px}
#al{display:none;padding:10px 14px;border-radius:10px;background:#7f1d1d;color:#fecaca;margin-bottom:8px;font-size:14px}
.tiles{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-bottom:12px}
.tile{background:#1b2533;border-radius:10px;padding:14px}
.lbl{font-size:11px;color:#94a3b8;letter-spacing:1px;text-transform:uppercase}
.val{font-size:24px;margin-top:6px}
.unit{font-size:13px;color:#94a3b8;margin-left:2px}
.small{font-size:12px;color:#94a3b8;margin-top:4px}
.charts{display:grid;grid-template-columns:1fr;gap:12px}
@media(min-width:900px){.tiles{grid-template-columns:repeat(4,1fr)}.charts{grid-template-columns:repeat(2,1fr)}}
.card{background:#1b2533;border-radius:10px;padding:14px}
.ct{font-size:14px;color:#cbd5e1;margin-bottom:6px}
.leg{font-size:12px;color:#cbd5e1;margin-bottom:6px}
.leg span{display:inline-block;margin-right:12px}
.leg i{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:5px}
canvas{width:100%;height:220px;display:block}
.bar{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}
button,a.btn{background:#334155;color:#e2e8f0;border:0;border-radius:8px;padding:10px 14px;font-size:14px;text-decoration:none}
</style></head><body>
<h1>SUNVOLT &mdash; Live Monitoring</h1>
<div class="sub">Solar-powered power bank telemetry &middot; updates every second, graphs every 5 s</div>
<div id="st">Connecting...</div>
<div id="al"></div>

<div class="tiles">
 <div class="tile"><div class="lbl">Battery SOC</div><div class="val"><span id="t_soc">--</span><span class="unit">%</span></div></div>
 <div class="tile"><div class="lbl">Battery Voltage</div><div class="val"><span id="t_bv">--</span><span class="unit">V</span></div></div>
 <div class="tile"><div class="lbl">Battery Current</div><div class="val"><span id="t_bi">--</span><span class="unit">mA</span></div><div class="small" id="t_dir">&nbsp;</div></div>
 <div class="tile"><div class="lbl">Solar Power In</div><div class="val"><span id="t_sw">--</span><span class="unit">W</span></div><div class="small" id="t_sol">&nbsp;</div></div>
 <div class="tile"><div class="lbl">Solar Voltage</div><div class="val"><span id="t_sv">--</span><span class="unit">V</span></div></div>
 <div class="tile"><div class="lbl">Battery Power</div><div class="val"><span id="t_bw">--</span><span class="unit">W</span></div></div>
 <div class="tile"><div class="lbl">Energy In / Out</div><div class="val" style="font-size:18px"><span id="t_ein">--</span> / <span id="t_eout">--</span><span class="unit">Wh</span></div><div class="small">Solar: <span id="t_esol">--</span> Wh</div></div>
 <div class="tile"><div class="lbl">Time Left</div><div class="val"><span id="t_hrs">--</span><span class="unit">h</span></div><div class="small">Up <span id="t_up">--</span></div></div>
</div>

<div class="charts">
 <div class="card"><div class="ct">Power (W) &mdash; Solar vs Battery</div>
  <div class="leg"><span><i style="background:#f59e0b"></i>Solar</span><span><i style="background:#22c55e"></i>Battery (+ out / &minus; in)</span></div>
  <canvas id="c_pow"></canvas></div>
 <div class="card"><div class="ct">Voltage (V)</div>
  <div class="leg"><span><i style="background:#f59e0b"></i>Solar V</span><span><i style="background:#22c55e"></i>Battery V</span></div>
  <canvas id="c_volt"></canvas></div>
 <div class="card"><div class="ct">Battery State of Charge (%)</div>
  <div class="leg"><span><i style="background:#3b82f6"></i>SOC</span></div>
  <canvas id="c_soc"></canvas></div>
 <div class="card"><div class="ct">Current (mA)</div>
  <div class="leg"><span><i style="background:#f59e0b"></i>Solar mA</span><span><i style="background:#22c55e"></i>Battery mA (+ out / &minus; in)</span></div>
  <canvas id="c_cur"></canvas></div>
</div>

<div class="bar">
 <button onclick="fetch('/reset')">Reset energy counters</button>
 <a class="btn" href="/history.csv">Download CSV</a>
</div>

<script>
const $=id=>document.getElementById(id);
const SOL='#f59e0b',BAT='#22c55e',SOC='#3b82f6';
let P=[],last=0,solarPresent=false;
const MAXP=720;

function fmt(v,d){return (v==null||!isFinite(v))?'--':v.toFixed(d);}
function niceStep(x){const p=Math.pow(10,Math.floor(Math.log10(x)));const f=x/p;return (f<=1?1:f<=2?2:f<=5?5:10)*p;}

function draw(id,series,o){
  o=o||{};
  const cv=$(id),dpr=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;
  cv.width=w*dpr;cv.height=h*dpr;
  const c=cv.getContext('2d');c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
  const L=50,R=10,T=8,B=24,pw=w-L-R,ph=h-T-B;
  c.font='11px Arial';
  let lo=Infinity,hi=-Infinity;
  series.forEach(s=>P.forEach(p=>{const v=p[s.k];if(v!=null&&isFinite(v)){if(v<lo)lo=v;if(v>hi)hi=v;}}));
  if(lo===Infinity||P.length<2){c.fillStyle='#64748b';c.fillText(P.length<2?'Collecting data...':'No sensor data',L+10,T+ph/2);return;}
  if(o.min!=null)lo=o.min; if(o.max!=null)hi=o.max;
  if(o.zero){lo=Math.min(lo,0);hi=Math.max(hi,0);}
  if(hi-lo<1e-6){hi+=Math.max(Math.abs(hi)*0.1,0.05);lo-=Math.max(Math.abs(lo)*0.1,0.05);}
  const step=niceStep((hi-lo)/4);
  if(o.min==null)lo=Math.floor(lo/step)*step;
  if(o.max==null)hi=Math.ceil(hi/step)*step;
  const Y=v=>T+ph-(v-lo)/(hi-lo)*ph;
  const t0=P[0].t,t1=P[P.length-1].t,span=Math.max(t1-t0,1);
  const X=t=>L+(t-t0)/span*pw;
  const dec=step<0.01?3:step<0.1?2:step<1?1:0;
  for(let v=lo;v<=hi+step/2;v+=step){
    const y=Y(v);c.strokeStyle=Math.abs(v)<step/1e3?'#475569':'#263241';c.lineWidth=1;
    c.beginPath();c.moveTo(L,y);c.lineTo(L+pw,y);c.stroke();
    c.fillStyle='#94a3b8';c.textAlign='right';c.fillText(v.toFixed(dec),L-6,y+4);
  }
  c.textAlign='center';c.fillStyle='#94a3b8';
  [0,0.5,1].forEach(f=>{const t=t0+f*span,ago=Math.round((t1-t)/60);
    c.fillText(f===1?'now':'-'+ago+' min',L+f*pw,h-6);});
  series.forEach(s=>{
    c.strokeStyle=s.c;c.lineWidth=2;c.beginPath();let pen=false;
    P.forEach(p=>{const v=p[s.k];if(v==null||!isFinite(v)){pen=false;return;}
      const x=X(p.t),y=Y(v);if(pen)c.lineTo(x,y);else{c.moveTo(x,y);pen=true;}});
    c.stroke();
  });
}

function drawAll(){
  draw('c_pow',[{k:'sw',c:SOL},{k:'bw',c:BAT}],{zero:true});
  draw('c_volt',[{k:'sv',c:SOL},{k:'bv',c:BAT}]);
  draw('c_soc',[{k:'soc',c:SOC}],{min:0,max:100});
  draw('c_cur',[{k:'si',c:SOL},{k:'bi',c:BAT}],{zero:true});
}

async function loadHistory(){
  try{
    const d=await (await fetch('/history?since='+last)).json();
    d.p.forEach(a=>{P.push({t:a[0],bv:a[1],bi:a[2],bw:a[3],sv:a[4],si:a[5],sw:a[6],soc:a[7]});last=a[0];});
    if(P.length>MAXP)P.splice(0,P.length-MAXP);
    drawAll();
  }catch(e){}
}

async function update(){
  try{
    const d=await (await fetch('/data')).json();
    const st=$('st');st.textContent=d.state;
    st.style.background=d.state.indexOf('CHARGING')>=0?'#166534':d.state=='DISCHARGING'?'#9a3412':d.state=='IDLE'?'#334155':'#7f1d1d';
    $('al').style.display=d.alert?'block':'none';$('al').textContent=d.alert;
    $('t_soc').textContent=d.soc;
    $('t_bv').textContent=fmt(d.v,2);
    $('t_bi').textContent=fmt(Math.abs(d.ma),0);
    $('t_dir').textContent=d.ma>20?'flowing out':d.ma<-20?'flowing in':'idle';
    $('t_bw').textContent=fmt(Math.abs(d.w),2);
    if(d.sok){$('t_sw').textContent=fmt(d.sw,2);$('t_sv').textContent=fmt(d.sv,2);$('t_sol').textContent=fmt(d.si,0)+' mA';}
    else{$('t_sw').textContent='--';$('t_sv').textContent='--';$('t_sol').textContent='no solar sensor';}
    $('t_ein').textContent=fmt(d.whin,3);$('t_eout').textContent=fmt(d.whout,3);$('t_esol').textContent=fmt(d.whs,3);
    $('t_hrs').textContent=d.hrs>0?fmt(d.hrs,1):'--';
    $('t_up').textContent=Math.floor(d.up/60)+'m '+(d.up%60)+'s';
  }catch(e){$('st').textContent='Connection lost';$('st').style.background='#7f1d1d';}
}

window.addEventListener('resize',drawAll);
update();loadHistory();
setInterval(update,1000);
setInterval(loadHistory,5000);
</script></body></html>
)rawliteral";

// ---------------- Web handlers ----------------
void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
  char buf[640];
  snprintf(buf, sizeof(buf),
    "{\"ok\":%s,\"v\":%.3f,\"cell\":%.3f,\"ma\":%.1f,\"w\":%.3f,\"soc\":%d,"
    "\"state\":\"%s\",\"alert\":\"%s\",\"whin\":%.4f,\"whout\":%.4f,\"hrs\":%.1f,"
    "\"sok\":%s,\"sv\":%.3f,\"si\":%.1f,\"sw\":%.3f,\"whs\":%.4f,\"up\":%lu}",
    battOK ? "true" : "false", battV, restV / CELLS_IN_SERIES, currentmA, powerW, soc,
    stateText(), alertText(), whIn, whOut, hoursLeft(),
    solarOK ? "true" : "false", solarV, solarmA, solarW, whSolar, millis() / 1000UL);
  server.send(200, "application/json", buf);
}

// Writes a float, or "null"/"" when it is not a number
void numOrNull(char *dst, size_t n, float v, int dec, bool csv) {
  if (isnan(v)) snprintf(dst, n, "%s", csv ? "" : "null");
  else          snprintf(dst, n, "%.*f", dec, v);
}

void sendHistory(bool csv) {
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), NULL, 10) : 0;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  if (csv) {
    server.sendHeader("Content-Disposition", "attachment; filename=sunvolt_history.csv");
    server.send(200, "text/csv", "");
    server.sendContent("seconds,battery_V,battery_mA,battery_W,solar_V,solar_mA,solar_W,soc_pct\n");
  } else {
    server.send(200, "application/json", "");
    server.sendContent("{\"p\":[");
  }

  char out[1400]; size_t len = 0; bool firstPt = true;
  int start = (histHead - histCount + HIST_N) % HIST_N;
  for (int k = 0; k < histCount; k++) {
    const Point &p = hist[(start + k) % HIST_N];
    if (!csv && p.t <= since) continue;
    char bv[16], bi[16], bw[16], sv[16], si[16], sw[16];
    numOrNull(bv, 16, p.bv, 3, csv); numOrNull(bi, 16, p.bi, 1, csv); numOrNull(bw, 16, p.bw, 3, csv);
    numOrNull(sv, 16, p.sv, 3, csv); numOrNull(si, 16, p.si, 1, csv); numOrNull(sw, 16, p.sw, 3, csv);
    char line[160];
    if (csv) snprintf(line, sizeof(line), "%lu,%s,%s,%s,%s,%s,%s,%u\n",
                      (unsigned long)p.t, bv, bi, bw, sv, si, sw, p.soc);
    else     snprintf(line, sizeof(line), "%s[%lu,%s,%s,%s,%s,%s,%s,%u]", firstPt ? "" : ",",
                      (unsigned long)p.t, bv, bi, bw, sv, si, sw, p.soc);
    firstPt = false;
    size_t l = strlen(line);
    if (len + l >= sizeof(out) - 1) { out[len] = 0; server.sendContent(out); len = 0; }
    memcpy(out + len, line, l); len += l;
  }
  if (len) { out[len] = 0; server.sendContent(out); }
  if (!csv) server.sendContent("]}");
  server.sendContent("");                     // end of chunked response
}

void handleHistory()    { sendHistory(false); }
void handleHistoryCsv() { sendHistory(true); }

void handleReset() {
  whIn = 0; whOut = 0; whSolar = 0;
  server.send(200, "text/plain", "ok");
}

// ---------------- Setup and loop ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nSUNVOLT starting...");

  Wire.begin(SDA_PIN, SCL_PIN);
  battOK  = startSensor(inaBatt, BATT_ADDR);
  solarOK = startSensor(inaSolar, SOLAR_ADDR);
  Serial.println(battOK  ? "Battery INA219 (0x40) found."
                         : "Battery INA219 (0x40) NOT found - check VCC->3V3, GND, SDA->D21, SCL->D22");
  Serial.println(solarOK ? "Solar INA219 (0x41) found."
                         : "Solar INA219 (0x41) not fitted - solar graphs will show no data");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Dashboard: join Wi-Fi '");
  Serial.print(AP_SSID);
  Serial.print("' and open http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/history.csv", handleHistoryCsv);
  server.on("/reset", handleReset);
  server.begin();

  sample();
  recordPoint();
  lastHistMs = millis();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // Re-connect a sensor that was missing or unplugged
  if (now - lastRetryMs >= 3000) {
    lastRetryMs = now;
    if (!battOK && (battOK = startSensor(inaBatt, BATT_ADDR)))    { firstBatt = true;  Serial.println("Battery INA219 reconnected."); }
    if (!solarOK && (solarOK = startSensor(inaSolar, SOLAR_ADDR))) { firstSolar = true; Serial.println("Solar INA219 connected."); }
  }

  if (now - lastSampleMs >= SAMPLE_MS) sample();
  if (now - lastHistMs >= HIST_MS) { lastHistMs = now; recordPoint(); }

  if (now - lastPrintMs >= PRINT_MS) {
    lastPrintMs = now;
    if (battOK) {
      Serial.printf("BAT V=%.3f I=%.1f mA P=%.3f W SoC=%d%% %s | ",
                    battV, currentmA, powerW, soc, stateText());
      if (solarOK) Serial.printf("SOLAR V=%.2f I=%.1f mA P=%.3f W", solarV, solarmA, solarW);
      else         Serial.print("SOLAR: no sensor");
      Serial.printf(" | In=%.4f Out=%.4f Solar=%.4f Wh\n", whIn, whOut, whSolar);
      const char* a = alertText();
      if (a[0]) { Serial.print("  ALERT: "); Serial.println(a); }
    } else {
      Serial.println("Waiting for battery INA219...");
    }
  }
}
