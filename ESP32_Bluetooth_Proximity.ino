#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <string>

// ================ PIN CONNECTIONS =================
#define TRIG_PIN   5
#define ECHO_PIN   18
#define BUZZER_PIN 23

// ================ WIFI ACCESS POINT ================
const char* WIFI_NAME = "Vehicle_Proximity";
const char* WIFI_PASS = "12345678";

// Must match the BLE advertising name
const char* TARGET_NAME = "AppuTag";

// ================ ALERT THRESHOLDS =================
const float WARNING_DISTANCE = 100.0;
const float DANGER_DISTANCE  = 50.0;

// ================ OBJECTS ==========================
WebServer server(80);
BLEScan* scanner = nullptr;

// ================ SHARED VARIABLES ================
volatile bool foundThisScan = false;
volatile bool bleDetected = false;
volatile int rssiValue = 0;

float distanceCM = -1.0;
float filteredDistance = -1.0;

String systemStatus = "WAITING FOR BLE";

unsigned long lastUltrasonicTime = 0;
unsigned long lastBleScanTime = 0;
unsigned long lastValidEchoTime = 0;
unsigned long lastPrintTime = 0;
unsigned long lastBuzzerToggle = 0;

bool buzzerState = false;
bool scanStarted = false;

const unsigned long ULTRASONIC_INTERVAL = 60;
const unsigned long BLE_SCAN_INTERVAL = 1500;
const unsigned long ECHO_TIMEOUT_MS = 250;

// ================ BLE CALLBACK =====================
class DeviceCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    if (device.haveName() &&
        device.getName() == std::string(TARGET_NAME)) {

      foundThisScan = true;
      rssiValue = device.getRSSI();

      Serial.printf("BLE found: %s | RSSI: %d dBm\n",
                    TARGET_NAME, (int)rssiValue);
    }
  }
};

// Called when the asynchronous BLE scan finishes
void scanComplete(BLEScanResults results) {
  bleDetected = foundThisScan;
  scanStarted = false;
}

// ================ ULTRASONIC SENSOR ================
float measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 25000UL);

  if (duration == 0) {
    return -1.0;
  }

  float cm = duration * 0.0343f / 2.0f;

  if (cm < 2.0 || cm > 400.0) {
    return -1.0;
  }

  return cm;
}

// ================ WEB DASHBOARD ====================
const char dashboard[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta charset="UTF-8">
<title>Vehicle Proximity Alert</title>

<style>
body {
  background: #111827;
  color: white;
  font-family: Arial, sans-serif;
  text-align: center;
  padding: 15px;
}
.container {
  max-width: 500px;
  margin: auto;
}
.card {
  background: #1f2937;
  border-radius: 14px;
  padding: 18px;
  margin: 14px 0;
}
.label {
  color: #cbd5e1;
  font-size: 15px;
}
.value {
  font-size: 27px;
  font-weight: bold;
  margin-top: 10px;
  overflow-wrap: anywhere;
}
.small {
  color: #94a3b8;
  font-size: 12px;
}
</style>
</head>

<body>
<div class="container">
<h2>Bluetooth Vehicle Proximity Alert</h2>
<p>ESP32 Live Monitoring</p>

<div class="card">
  <div class="label">Ultrasonic Distance</div>
  <div class="value" id="distance">--</div>
</div>

<div class="card">
  <div class="label">BLE Device</div>
  <div class="value" id="ble">Searching...</div>
</div>

<div class="card">
  <div class="label">Received Signal Strength</div>
  <div class="value" id="rssi">--</div>
</div>

<div class="card">
  <div class="label">System Status</div>
  <div class="value" id="status">Starting</div>
</div>

<p class="small">ESP32 local Wi-Fi dashboard</p>

<script>
async function refreshDashboard() {
  try {
    const response = await fetch('/data?_=' + Date.now(), {
      cache: 'no-store'
    });

    if (!response.ok) throw new Error('Network error');

    const data = await response.json();

    document.getElementById('distance').textContent =
      data.distance < 0 ? 'No echo' : data.distance + ' cm';

    document.getElementById('ble').textContent = data.ble;

    document.getElementById('rssi').textContent =
      data.ble === 'Detected' ? data.rssi + ' dBm' : '--';

    const status = document.getElementById('status');
    status.textContent = data.status;

    status.style.color =
      data.status.includes('DANGER') ? '#ff4d4d' :
      data.status.includes('WARNING') ? '#ffd43b' :
      data.status === 'SAFE' ? '#4ade80' : '#ffffff';

  } catch (error) {
    document.getElementById('status').textContent =
      'Connecting...';
  }
}

setInterval(refreshDashboard, 300);
refreshDashboard();
</script>
</div>
</body>
</html>
)HTML";

// ================ WEB HANDLERS =====================
void handleRoot() {
  server.send_P(200, "text/html", dashboard);
}

void handleData() {
  String json = "{";

  json += "\"distance\":" + String(distanceCM, 1) + ",";
  json += "\"ble\":\"";
  json += (bleDetected ? "Detected" : "Not detected");
  json += "\",";
  json += "\"rssi\":" + String((int)rssiValue) + ",";
  json += "\"status\":\"" + systemStatus + "\"";

  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// ================ SETUP ============================
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Start Wi-Fi access point
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(WIFI_NAME, WIFI_PASS);

  Serial.println();
  Serial.println("Vehicle Proximity Alert");
  Serial.print("Wi-Fi name: ");
  Serial.println(WIFI_NAME);
  Serial.print("Dashboard: http://");
  Serial.println(WiFi.softAPIP());

  // Initialize BLE
  BLEDevice::init("");

  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(
    new DeviceCallback(), false
  );

  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(99);

  // Start web server
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  Serial.println("Dashboard started");
  Serial.println("Searching for AppuTag...");
}

// ================ MAIN LOOP ========================
void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Start a BLE scan without waiting for it to finish
  if (!scanStarted &&
      now - lastBleScanTime >= BLE_SCAN_INTERVAL) {

    lastBleScanTime = now;
    foundThisScan = false;
    scanStarted = true;

    bool started = scanner->start(1, scanComplete, false);

    if (!started) {
      scanStarted = false;
    }
  }

  // Read the ultrasonic sensor regularly
  if (now - lastUltrasonicTime >= ULTRASONIC_INTERVAL) {
    lastUltrasonicTime = now;

    float reading = measureDistance();

    if (reading > 0) {
      lastValidEchoTime = millis();

      if (filteredDistance < 0) {
        filteredDistance = reading;
      } else {
        // Smooth fluctuations while retaining responsiveness
        filteredDistance =
          0.65f * filteredDistance + 0.35f * reading;
      }

      distanceCM = filteredDistance;
    } else if (millis() - lastValidEchoTime > ECHO_TIMEOUT_MS) {
      distanceCM = -1.0;
      filteredDistance = -1.0;
    }
  }

  // Determine status independently of BLE availability
  if (distanceCM > 0 && distanceCM <= DANGER_DISTANCE) {
    systemStatus = bleDetected ? "DANGER" : "OBSTACLE DANGER";
  } else if (distanceCM > DANGER_DISTANCE &&
             distanceCM <= WARNING_DISTANCE) {
    systemStatus = bleDetected ? "WARNING" : "OBSTACLE WARNING";
  } else if (distanceCM < 0) {
    systemStatus = "SENSOR ERROR";
  } else if (bleDetected) {
    systemStatus = "SAFE";
  } else {
    systemStatus = "WAITING FOR BLE";
  }

  // Active buzzer: danger = continuous sound
  // Warning = intermittent sound
  if (distanceCM > 0 && distanceCM <= DANGER_DISTANCE) {
    buzzerState = true;
    digitalWrite(BUZZER_PIN, HIGH);
  } else if (distanceCM > DANGER_DISTANCE &&
             distanceCM <= WARNING_DISTANCE) {

    if (now - lastBuzzerToggle >= 250) {
      lastBuzzerToggle = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }

  } else {
    buzzerState = false;
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Keep serial output readable
  if (now - lastPrintTime >= 500) {
    lastPrintTime = now;

    Serial.print("Distance: ");
    Serial.print(distanceCM);
    Serial.print(" cm | BLE: ");
    Serial.print(bleDetected ? "Detected" : "Not detected");
    Serial.print(" | RSSI: ");
    Serial.print((int)rssiValue);
    Serial.print(" dBm | Status: ");
    Serial.println(systemStatus);
  }

  server.handleClient();
}
