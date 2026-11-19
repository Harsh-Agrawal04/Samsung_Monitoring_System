// Sensor_WiFi: MPX5100DP -> divider -> ADS1115 -> ESP32, served over Wi-Fi to vacuum_demo.html.
//
// Once per second the ESP32 averages 32 ADC samples, converts to vacuum strength in kPa,
// and stores the result in a small buffer. The demo page asks for new readings at
// http://<address>/readings?since=<last number it saw>.
//
// Open the Serial Monitor at 115200 baud to see readings and the address to type into the page.
//
// Libraries to install: Adafruit ADS1X15 (and Adafruit BusIO if asked).
// WiFi, WebServer, ESPmDNS and Wire come with the ESP32 board package.

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_ADS1X15.h>

extern "C" uint32_t esp_random(void);

// ---------------- Settings you may need to change ----------------
const char *WIFI_SSID = "Harsh’s Phone ";
const char *WIFI_PASSWORD = "harsh231";
const char *HOST_NAME = "vacuum";        // the page can also use vacuum.local on most networks

const int SDA_PIN = 21;
const int SCL_PIN = 22;
const int SAMPLES = 32;                  // readings averaged per report
const uint32_t REPORT_MS = 1000;         // one report per second
const float VOLTS_PER_COUNT = 0.000125f; // ADS1115 at +/-4.096 V
const float DIVIDER_RATIO = 2.0f;        // 47k + 47k divider; use your measured ratio
const float SENSOR_SUPPLY_V = 5.0f;      // measure the sensor's VS pin and update this
const float ZERO_OFFSET_KPA = 0.0f;      // reading with both ports open to air; for example -0.65
// ------------------------------------------------------------------

const int BUF_SIZE = 60;
uint32_t bufSeq[BUF_SIZE];
uint32_t bufMs[BUF_SIZE];
float bufKpa[BUF_SIZE];
bool bufOk[BUF_SIZE];
uint32_t seqCounter = 0;
uint32_t bootId = 0;

Adafruit_ADS1115 ads;
WebServer server(80);

bool adsReady = false;
uint32_t nextAdsTry = 0;
uint32_t nextReport = 0;
uint32_t nextWifiTry = 0;
uint32_t nextStatusPrint = 0;
bool wifiWasUp = false;
bool serverStarted = false;

bool adsResponds() {
  Wire.beginTransmission(0x48);
  return Wire.endTransmission() == 0;
}

void serviceAds(uint32_t now) {
  if (adsReady || (int32_t)(now - nextAdsTry) < 0) return;
  nextAdsTry = now + 3000;
  if (ads.begin(0x48, &Wire)) {
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_128SPS);
    adsReady = true;
    Serial.println("DBG: ADS1115 ready");
  } else {
    Serial.println("DBG: ADS1115 not found at 0x48. Check wiring.");
  }
}

void storeReading(bool ok, float kpa) {
  seqCounter++;
  int i = seqCounter % BUF_SIZE;
  bufSeq[i] = seqCounter;
  bufMs[i] = millis();
  bufKpa[i] = kpa;
  bufOk[i] = ok;
}

void takeReading() {
  if (!adsReady || !adsResponds()) {
    adsReady = false;
    nextAdsTry = millis() + 3000;
    storeReading(false, 0);
    Serial.println("raw=ERROR  sensor not responding");
    return;
  }
  long total = 0;
  for (int i = 0; i < SAMPLES; i++) {
    total += ads.readADC_SingleEnded(0);
  }
  float raw = (float)total / SAMPLES;
  float vAdc = raw * VOLTS_PER_COUNT;
  float vSensor = vAdc * DIVIDER_RATIO;
  float kpa = (vSensor / SENSOR_SUPPLY_V - 0.04f) / 0.009f - ZERO_OFFSET_KPA;
  storeReading(true, kpa);
  Serial.printf("raw=%.1f  v_adc=%.4f V  v_sensor=%.4f V  vacuum=%.2f kPa\n", raw, vAdc, vSensor, kpa);
}

void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Cache-Control", "no-store");
}

void handleOptions() {
  sendCors();
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.send(204);
}

void handleRoot() {
  sendCors();
  server.send(200, "text/plain", "Vacuum demo ESP32. Open vacuum_demo.html and enter this address. Readings: /readings");
}

void handleReadings() {
  uint32_t since = 0;
  if (server.hasArg("since")) {
    long s = server.arg("since").toInt();
    if (s > 0) since = (uint32_t)s;
  }
  String out;
  out.reserve(4096);
  out += "{\"boot\":";
  out += String(bootId);
  out += ",\"seq\":";
  out += String(seqCounter);
  out += ",\"now_ms\":";
  out += String(millis());
  out += ",\"readings\":[";

  uint32_t startSeq = seqCounter > (uint32_t)BUF_SIZE ? seqCounter - BUF_SIZE + 1 : 1;
  if (since + 1 > startSeq) startSeq = since + 1;
  bool first = true;
  for (uint32_t s = startSeq; s <= seqCounter; s++) {
    int i = s % BUF_SIZE;
    if (bufSeq[i] != s) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"seq\":";
    out += String(s);
    out += ",\"t_ms\":";
    out += String(bufMs[i]);
    if (bufOk[i]) {
      out += ",\"kpa\":";
      out += String(bufKpa[i], 3);
    } else {
      out += ",\"err\":\"ads_missing\"";
    }
    out += "}";
  }
  out += "]}";
  sendCors();
  server.send(200, "application/json", out);
}

const char *wifiStatusText(int s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "network not found (wrong name, 5 GHz only, or hotspot not visible)";
    case WL_CONNECT_FAILED: return "connection failed (usually a wrong password)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:   return "not connected yet";
    case WL_IDLE_STATUS:    return "idle";
    default:                return "unknown";
  }
}

void printVisibleNetworks() {
  Serial.println("DBG: scanning for 2.4 GHz networks (the ESP32 cannot see 5 GHz)...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("DBG: no networks found");
    return;
  }
  for (int i = 0; i < n && i < 15; i++) {
    Serial.printf("DBG:   \"%s\"  signal %d dBm  channel %d\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i));
  }
  WiFi.scanDelete();
}

int wifiAttempts = 0;

void serviceWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasUp) {
      wifiWasUp = true;
      Serial.printf("DBG: Wi-Fi connected. Enter this address in the page: %s\n", WiFi.localIP().toString().c_str());
      if (!serverStarted) {
        server.on("/", HTTP_GET, handleRoot);
        server.on("/readings", HTTP_GET, handleReadings);
        server.on("/readings", HTTP_OPTIONS, handleOptions);
        server.begin();
        serverStarted = true;
        if (MDNS.begin(HOST_NAME)) {
          Serial.printf("DBG: or use %s.local\n", HOST_NAME);
        }
      }
    }
    if ((int32_t)(now - nextStatusPrint) >= 0) {
      nextStatusPrint = now + 15000;
      Serial.printf("DBG: page address is %s\n", WiFi.localIP().toString().c_str());
    }
  } else {
    if (wifiWasUp) {
      wifiWasUp = false;
      Serial.println("DBG: Wi-Fi connection lost");
    }
    if ((int32_t)(now - nextWifiTry) >= 0) {
      nextWifiTry = now + 15000;
      wifiAttempts++;
      if (wifiAttempts > 1) Serial.printf("DBG: last result: %s\n", wifiStatusText(WiFi.status()));
      if (strcmp(WIFI_SSID, "YOUR_WIFI_NAME") == 0) Serial.println("DBG: WIFI_SSID is still the placeholder. Edit it at the top of the sketch.");
      Serial.printf("DBG: trying network \"%s\" (password is %d characters)\n", WIFI_SSID, (int)strlen(WIFI_PASSWORD));
      WiFi.disconnect();
      if (wifiAttempts == 1 || wifiAttempts % 3 == 0) printVisibleNetworks();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(SDA_PIN, SCL_PIN);
  bootId = esp_random();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.println("DBG: starting");
}

void loop() {
  uint32_t now = millis();
  serviceWifi(now);
  if (serverStarted) server.handleClient();
  serviceAds(now);

  if ((int32_t)(now - nextReport) >= 0) {
    nextReport = now + REPORT_MS;
    takeReading();
  }
  delay(2);
}