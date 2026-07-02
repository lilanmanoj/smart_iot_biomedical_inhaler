/*
  Smart Biomedical Inhaler - ESP32 (Arduino)

  Notes:
    Pin usage:
      - Flow sensor (pulse): GPIO27 (interrupt-capable)
      - Pressure sensor (ADC): GPIO39 (ADC input)
      - Blue LED (status/blinking): GPIO33
      - Green LED: GPIO25
      - Red LED: GPIO32

  - WiFi setup AP credentials come from build flags `WIFI_SETUP_SSID` and
    `WIFI_SETUP_PASSWORD` (defined in `platformio.ini`).

  - WiFi credentials are saved using `Preferences` (non-volatile storage).

  - Firebase configuration values are placeholders and must be filled in
    before uploading (database URL / API key / auth as required).

  - Pressure and flow conversions are placeholders — replace with correct
    calibration/conversion formulas or driver libraries for MPRLS / YF-S201C.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoJson.h>

// --- Pin configuration (edit if you want other pins) ---------------------
const int FLOW_PIN = 27;      // Must be interrupt-capable
const int PRESSURE_PIN = 39;  // ADC input (input-only is fine)
const int LED_BLUE = 33;      // Power and connection status: blinking/solid
const int LED_GREEN = 25;     // Correct operation indicator
const int LED_RED = 32;       // Fault operation indicator

// --- WiFi / WebServer / Storage -----------------------------------------
WebServer server(80);
Preferences prefs;
bool apMode = false;

// --- Firebase objects (placeholders) ------------------------------------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig fbconfig;

// Firebase configuration: values come from build flags when available
const char *FIREBASE_DATABASE_URL_STR = FIREBASE_DATABASE_URL;
const char *FIREBASE_API_KEY_STR = FIREBASE_API_KEY;

// Thresholds: provided via build flags (platformio.ini) or fallback values
#ifndef FLOW_RATE_THRESHOLD
#define FLOW_RATE_THRESHOLD 1.0
#endif
#ifndef PRESSURE_THRESHOLD
#define PRESSURE_THRESHOLD 1.0
#endif

// Pressure sensor enable flag: can be set via build flags (platformio.ini)
#ifndef PRESSURE_SENSOR_ENABLED
#define PRESSURE_SENSOR_ENABLED 1
#endif

// --- Flow sensor variables ----------------------------------------------
volatile unsigned long pulseCount = 0;
unsigned long lastFlowMillis = 0;
float flowLpm = 0.0; // liters per minute

// --- LED timing ---------------------------------------------------------
bool ledGreenOn = false;
unsigned long ledGreenOffAt = 0;
bool ledRedOn = false;
unsigned long ledRedOffAt = 0;

// Calibration: pulses per liter (placeholder for YF-S201C)
const float FLOW_PULSES_PER_LITER = 7.5; // adjust to your sensor

void IRAM_ATTR flow_isr() {
  pulseCount++;
}

// --- Web server handlers ------------------------------------------------
String webFormPage() {
  String page = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>WiFi Setup</title></head><body>";
  page += "<h3>Configure WiFi</h3>";
  page += "<form method=\"POST\" action=\"/save\">";
  page += "SSID:<br><input name=\"ssid\" placeholder=\"SSID\"><br>";
  page += "Password:<br><input name=\"password\" type=\"password\" placeholder=\"Password\"><br><br>";
  page += "<button type=\"submit\">Save</button></form>";
  page += "</body></html>";
  return page;
}

void handleRoot() {
  server.send(200, "text/html", webFormPage());
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String ssid = server.arg("ssid");
    String pass = server.arg("password");
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    server.send(200, "text/html", "Saved credentials. Rebooting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

// Start Access Point and web server for setup
void startAPMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SETUP_SSID, WIFI_SETUP_PASSWORD);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("Started AP: %s  IP: %s\n", WIFI_SETUP_SSID, IP.toString().c_str());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// Try to connect to WiFi using stored credentials
bool tryConnectStoredWiFi(const String &ssid, const String &pass, unsigned long timeoutMs = 10000) {
  Serial.printf("Attempting to connect to SSID: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(200);
  }
  return (WiFi.status() == WL_CONNECTED);
}

// Initialize Firebase
void initFirebase() {
  fbconfig.api_key = FIREBASE_API_KEY_STR;
  fbconfig.database_url = FIREBASE_DATABASE_URL_STR;
  Firebase.begin(&fbconfig, &auth);
}

// Publish JSON with pressure, flow and passed flag
void publishSensorData(float pressure, float flow, bool passed) {
  FirebaseJson json;
  json.set("pressure", pressure);
  json.set("flow", flow);
  json.set("passed", passed);
  String path = "/devices/device1/data";
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
    Serial.println("Uploaded sensor data to Firebase");
  } else {
    Serial.printf("Firebase error: %s\n", fbdo.errorReason().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Pin modes
  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(FLOW_PIN, INPUT_PULLUP);

#if PRESSURE_SENSOR_ENABLED
  // Pressure sensor is enabled; ensure ADC pin is usable.
  analogReadResolution(12);
#endif

  // Attach flow sensor interrupt
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flow_isr, RISING);

  // Read stored WiFi creds
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  unsigned long blinkPrevious = 0;
  bool ledState = false;

  // Blink blue while attempting operations
  unsigned long startAttempt = millis();
  while (millis() - startAttempt < 5000) { // short startup window
    unsigned long now = millis();
    if (now - blinkPrevious >= 500) {
      blinkPrevious = now;
      ledState = !ledState;
      digitalWrite(LED_BLUE, ledState ? HIGH : LOW);
    }
    delay(10);
  }

  // If credentials exist, try to connect
  if (ssid.length() > 0) {
    digitalWrite(LED_BLUE, HIGH); // steady while trying
    if (tryConnectStoredWiFi(ssid, pass, 10000)) {
      Serial.println("Connected to WiFi");
      digitalWrite(LED_BLUE, HIGH); // keep steady
      initFirebase();
    } else {
      Serial.println("Failed to connect with stored credentials, entering AP mode");
      digitalWrite(LED_BLUE, LOW);
      startAPMode();
    }
  } else {
    // No credentials -> start AP mode
    Serial.println("No stored WiFi credentials, entering AP mode");
    startAPMode();
  }
}

unsigned long lastSensorMillis = 0;

void loop() {
  // If in AP mode, handle webserver requests and blink blue LED
  if (apMode) {
    server.handleClient();
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      digitalWrite(LED_BLUE, !digitalRead(LED_BLUE));
    }
    return;
  }

  // If WiFi disconnected, try stored credentials and fallback to AP
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, attempting reconnect...");
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if (ssid.length() > 0 && tryConnectStoredWiFi(ssid, pass, 10000)) {
      Serial.println("Reconnected to WiFi");
      initFirebase();
      digitalWrite(LED_BLUE, HIGH);
    } else {
      Serial.println("Reconnect failed; starting AP");
      startAPMode();
      return;
    }
  }

  // Handle LED off timing in a non-blocking way
  if (ledGreenOn && millis() >= ledGreenOffAt) {
    digitalWrite(LED_GREEN, LOW);
    ledGreenOn = false;
  }
  if (ledRedOn && millis() >= ledRedOffAt) {
    digitalWrite(LED_RED, LOW);
    ledRedOn = false;
  }

  // Normal operation: read sensors periodically and publish
  if (millis() - lastSensorMillis >= 2000) { // every 2 seconds
    lastSensorMillis = millis();

    // Flow calculation: capture and reset pulseCount safely
    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    // pulses measured over the interval (2s) -> compute L/min
    float minutes = 2.0 / 60.0; // 2 seconds in minutes
    float liters = (float)pulses / FLOW_PULSES_PER_LITER;
    flowLpm = liters / minutes;

    float pressure = 0;
    bool pressureAvailable = false;

#if PRESSURE_SENSOR_ENABLED
    // Pressure read (placeholder): convert ADC to scaled pressure
    int raw = analogRead(PRESSURE_PIN);
    pressure = ((float)raw / 4095.0) * 25.0; // placeholder: map to 0-25 units
    pressureAvailable = true;
#endif

    if (pressureAvailable) {
      Serial.printf("Flow: %.2f L/min  Pulses:%lu  Pressure: %.2f\n", flowLpm, pulses, pressure);
    } else {
      Serial.printf("Flow: %.2f L/min  Pulses:%lu  Pressure: N/A\n", flowLpm, pulses);
    }

    // Determine pass/fail using thresholds (build flags)
    float flowThreshold = (float)FLOW_RATE_THRESHOLD;
    float pressureThreshold = (float)PRESSURE_THRESHOLD;
    bool passed = (flowLpm > flowThreshold) && (!pressureAvailable || pressure > pressureThreshold);

    // Schedule LED state changes without blocking
    if (passed) {
      digitalWrite(LED_GREEN, HIGH);
      ledGreenOn = true;
      ledGreenOffAt = millis() + 5000;
      publishSensorData(pressureAvailable ? pressure : 0, flowLpm, true);
    } else {
      digitalWrite(LED_RED, HIGH);
      ledRedOn = true;
      ledRedOffAt = millis() + 5000;
      publishSensorData(pressureAvailable ? pressure : 0, flowLpm, false);
    }
  }
}
