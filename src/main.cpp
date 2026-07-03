/*
  Smart Biomedical Inhaler - ESP32 Firmware
  Framework: Arduino / PlatformIO
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define ENABLE_DATABASE
#include <FirebaseClient.h>

// --- Pin Configuration ---
const int flowPin = FLOW_PIN;
const int pressurePin = PRESSURE_PIN;
const int LEDBlue = LED_BLUE;
const int LEDGreen = LED_GREEN;
const int LEDRed = LED_RED;

// --- System Configuration Constants ---
const unsigned long SENSOR_INTERVAL_MS = 2000;
const unsigned long LED_TIMEOUT_MS = 5000;
const unsigned long WIFI_TIMEOUT_MS = 10000;
const float FLOW_PULSES_PER_LITER = 7.5; // Calibration constant (YF-S201C)
const float PRESSURE_NOISE_DEADBAND = 0.5; // Ignore ADC noise below this (pressure units)

// Macro Fallbacks
#ifndef FLOW_RATE_THRESHOLD
#define FLOW_RATE_THRESHOLD 1.0f
#endif
#ifndef PRESSURE_THRESHOLD
#define PRESSURE_THRESHOLD 1.0f
#endif
#ifndef PRESSURE_SENSOR_ENABLED
#define PRESSURE_SENSOR_ENABLED 1
#endif

// --- Globals ---
WebServer server(80);
Preferences prefs;

enum SystemState { BOOTING, AP_MODE, CONNECTING, RUNNING };
SystemState currentState = BOOTING;

// Firebase Globals
FirebaseApp app;
RealtimeDatabase Database;
WiFiClientSecure sslClient;
using AsyncClient = AsyncClientClass;
AsyncClient asyncClient(sslClient);
NoAuth noAuth;

// Sensor State
volatile unsigned long pulseCount = 0;
unsigned long lastSensorMillis = 0;

// LED State Timers
unsigned long greenLedOffAt = 0;
unsigned long redLedOffAt = 0;
unsigned long lastBlueBlink = 0;

// --- Interrupt Service Routine ---
void IRAM_ATTR flow_isr() {
  pulseCount++;
}

// --- Web Server Handlers ---
String getWebPage() {
  return "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
         "<style>body{font-family:sans-serif; padding:20px;}</style></head><body>"
         "<h2>Smart Inhaler Setup</h2>"
         "<form method=\"POST\" action=\"/save\">"
         "<b>WiFi SSID:</b><br><input type=\"text\" name=\"ssid\" required><br><br>"
         "<b>Password:</b><br><input type=\"password\" name=\"password\" required><br><br>"
         "<button type=\"submit\" style=\"padding:10px 20px;\">Save & Reboot</button>"
         "</form></body></html>";
}

void handleRoot() {
  server.send(200, "text/html", getWebPage());
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("password"));
    prefs.end();
    server.send(200, "text/html", "<h2>Credentials saved. Rebooting device...</h2>");
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Error: Missing SSID or Password.");
  }
}

// --- Network Functions ---
void startAPMode() {
  currentState = AP_MODE;
  WiFi.mode(WIFI_AP);
  delay(100);

  // WPA2 requires a password of at least 8 characters; softAP() fails
  // silently otherwise. Fall back to an open AP rather than a dead one.
  const char* apSsid = WIFI_SETUP_SSID;
  const char* apPass = WIFI_SETUP_PASSWORD;
  if (strlen(apSsid) == 0) {
    apSsid = "InhalerSetup";
  }
  if (strlen(apPass) > 0 && strlen(apPass) < 8) {
    Serial.println("WARNING: AP password shorter than 8 chars - starting OPEN network.");
    apPass = nullptr;
  } else if (strlen(apPass) == 0) {
    apPass = nullptr;
  }

  bool apStarted = WiFi.softAP(apSsid, apPass, 1 /*channel*/, 0 /*visible*/, 4 /*max clients*/);

  // Many ESP32-C3 modules distort their RF output at full (20dBm) TX power,
  // so clients see the AP but the association handshake times out.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  if (apStarted) {
    Serial.printf("Setup portal '%s' started on IP: %s\n", apSsid, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("ERROR: softAP() failed to start!");
  }
}

void initFirebase() {
  sslClient.setInsecure(); // Required for ESP32 SSL without certificate
  sslClient.setHandshakeTimeout(10);
  
  initializeApp(asyncClient, app, getAuth(noAuth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(FIREBASE_DATABASE_URL);
  Serial.println("Firebase Client Initialized.");
}

void connectToWiFi(const String& ssid, const String& pass) {
  currentState = CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // C3 antenna workaround (see startAPMode)
  Serial.printf("Attempting connection to %s...\n", ssid.c_str());

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS) {
    // Blink blue LED rapidly while connecting
    if (millis() - lastBlueBlink >= 250) {
      lastBlueBlink = millis();
      digitalWrite(LEDBlue, !digitalRead(LEDBlue));
    }
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    currentState = RUNNING;
    digitalWrite(LEDBlue, HIGH); // Solid blue when connected
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    initFirebase();
  } else {
    Serial.println("WiFi connection failed. Falling back to AP Mode.");
    startAPMode();
  }
}

// --- Data Publishing ---
void publishToFirebase(float pressure, float flow, bool passed) {
  if (WiFi.status() != WL_CONNECTED || !app.isInitialized()) return;

  // Construct JSON payload
  String payload = "{";
  payload += "\"pressure\":" + String(pressure, 2) + ",";
  payload += "\"flow\":" + String(flow, 2) + ",";
  payload += "\"passed\":" + String(passed ? "true" : "false");
  payload += "}";

  String path = "/devices/inhaler_unit_1";
  
  // Fire and forget upload (handled by app.loop())
  if (Database.set<object_t>(asyncClient, path, object_t(payload))) {
    Serial.println("Data queued for Firebase upload.");
  } else {
    Serial.printf("Upload error: %s\n", asyncClient.lastError().message().c_str());
  }
}

// --- Core Logic ---
void handleSensors() {
  if (millis() - lastSensorMillis >= SENSOR_INTERVAL_MS) {
    lastSensorMillis = millis();

    // Safely extract and reset pulse count
    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    // Calculate Flow (L/min)
    float minutes = SENSOR_INTERVAL_MS / 60000.0;
    float flowLpm = ((float)pulses / FLOW_PULSES_PER_LITER) / minutes;

    // Calculate Pressure (if enabled)
    float pressure = 0.0;
    bool pressureAvailable = false;
    
    if (PRESSURE_SENSOR_ENABLED == 1) {
      int rawADC = analogRead(pressurePin);
      // Replace with actual MPRLS calibration math later
      pressure = ((float)rawADC / 4095.0) * 25.0; 
      pressureAvailable = true;
    }

    // --- IDLE CHECK ---
    // Only evaluate and publish if sensors detect actual activity.
    // Flow: any measured flow means pulses were counted this interval.
    // Pressure: use a small deadband so ADC noise doesn't cause false triggers.
    if (flowLpm > 0.0 || (pressureAvailable && pressure > PRESSURE_NOISE_DEADBAND)) {
      bool passed = (flowLpm >= (float)FLOW_RATE_THRESHOLD);
      if (pressureAvailable) {
        passed = passed && (pressure >= (float)PRESSURE_THRESHOLD);
      }

      Serial.printf("Activity Detected -> Flow: %.2f L/min | Pressure: %.2f | Status: %s\n", 
                     flowLpm, pressure, passed ? "PASS" : "FAIL");

      // Update LED Timers cleanly
      if (passed) {
        digitalWrite(LEDGreen, HIGH);
        digitalWrite(LEDRed, LOW);
        greenLedOffAt = millis() + LED_TIMEOUT_MS;
      } else {
        digitalWrite(LEDRed, HIGH);
        digitalWrite(LEDGreen, LOW);
        redLedOffAt = millis() + LED_TIMEOUT_MS;
      }

      publishToFirebase(pressure, flowLpm, passed);
    }
  }
}

void updateLEDs() {
  unsigned long currentMillis = millis();

  // AP Mode: Slow blink blue LED
  if (currentState == AP_MODE && (currentMillis - lastBlueBlink >= 1000)) {
    lastBlueBlink = currentMillis;
    digitalWrite(LEDBlue, !digitalRead(LEDBlue));
  }

  // Turn off Green/Red LEDs after timeout expires
  if (digitalRead(LEDGreen) == HIGH && currentMillis >= greenLedOffAt) {
    digitalWrite(LEDGreen, LOW);
  }
  if (digitalRead(LEDRed) == HIGH && currentMillis >= redLedOffAt) {
    digitalWrite(LEDRed, LOW);
  }
}

void handleNetwork() {
  if (currentState == AP_MODE) {
    server.handleClient();
  } 
  else if (currentState == RUNNING && WiFi.status() != WL_CONNECTED) {
    Serial.println("Connection lost. Reconnecting...");
    digitalWrite(LEDBlue, LOW);
    
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    
    connectToWiFi(ssid, pass);
  }
}

// --- Standard Arduino Functions ---
void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Native USB CDC: wait until the host opens the port (max 5s) so early
  // boot messages are not lost.
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 5000) {
    delay(10);
  }
#else
  delay(2000);
#endif
  Serial.println("\n--- Smart Inhaler Booting ---");

  // Init Pins
  pinMode(LEDBlue, OUTPUT);
  pinMode(LEDGreen, OUTPUT);
  pinMode(LEDRed, OUTPUT);
  pinMode(flowPin, INPUT_PULLUP);
  
  if (PRESSURE_SENSOR_ENABLED == 1) {
    analogReadResolution(12);
  }

  // Attach Interrupt
  attachInterrupt(digitalPinToInterrupt(flowPin), flow_isr, RISING);

  // Check Credentials
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() > 0) {
    connectToWiFi(ssid, pass);
  } else {
    Serial.println("No credentials found.");
    startAPMode();
  }
}

void loop() {
  // Process Firebase async tasks
  if (currentState == RUNNING) {
    app.loop();
  }
  
  handleNetwork();
  
  // Only process sensors and updates if we are actively running or in AP mode testing
  // (Prevents false readings while blocking for WiFi connection)
  if (currentState == RUNNING || currentState == AP_MODE) {
    handleSensors();
    updateLEDs();
  }
}
