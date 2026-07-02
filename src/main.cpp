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
const int FLOW_PIN = 27;
const int PRESSURE_PIN = 39;
const int LED_BLUE = 33;
const int LED_GREEN = 25;
const int LED_RED = 32;

// --- System Configuration Constants ---
const unsigned long SENSOR_INTERVAL_MS = 2000;
const unsigned long LED_TIMEOUT_MS = 5000;
const unsigned long WIFI_TIMEOUT_MS = 10000;
const float FLOW_PULSES_PER_LITER = 7.5; // Calibration constant (YF-S201C)

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
  WiFi.softAP(WIFI_SETUP_SSID, WIFI_SETUP_PASSWORD);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.printf("Setup portal started on IP: %s\n", WiFi.softAPIP().toString().c_str());
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
  Serial.printf("Attempting connection to %s...\n", ssid.c_str());

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_TIMEOUT_MS) {
    // Blink blue LED rapidly while connecting
    if (millis() - lastBlueBlink >= 250) {
      lastBlueBlink = millis();
      digitalWrite(LED_BLUE, !digitalRead(LED_BLUE));
    }
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    currentState = RUNNING;
    digitalWrite(LED_BLUE, HIGH); // Solid blue when connected
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
      int rawADC = analogRead(PRESSURE_PIN);
      // Replace with actual MPRLS calibration math later
      pressure = ((float)rawADC / 4095.0) * 25.0; 
      pressureAvailable = true;
    }

    // --- IDLE CHECK ---
    // Only evaluate and publish if sensors detect actual activity (> 0)
    // Note: If ADC noise causes false triggers, change 0.0 to a small deadband (e.g., 0.5)
    if (flowLpm > 150.0 || (pressureAvailable && pressure > 0.0)) {
      bool passed = (flowLpm >= (float)FLOW_RATE_THRESHOLD);
      if (pressureAvailable) {
        passed = passed && (pressure >= (float)PRESSURE_THRESHOLD);
      }

      Serial.printf("Activity Detected -> Flow: %.2f L/min | Pressure: %.2f | Status: %s\n", 
                     flowLpm, pressure, passed ? "PASS" : "FAIL");

      // Update LED Timers cleanly
      if (passed) {
        digitalWrite(LED_GREEN, HIGH);
        digitalWrite(LED_RED, LOW);
        greenLedOffAt = millis() + LED_TIMEOUT_MS;
      } else {
        digitalWrite(LED_RED, HIGH);
        digitalWrite(LED_GREEN, LOW);
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
    digitalWrite(LED_BLUE, !digitalRead(LED_BLUE));
  }

  // Turn off Green/Red LEDs after timeout expires
  if (digitalRead(LED_GREEN) == HIGH && currentMillis >= greenLedOffAt) {
    digitalWrite(LED_GREEN, LOW);
  }
  if (digitalRead(LED_RED) == HIGH && currentMillis >= redLedOffAt) {
    digitalWrite(LED_RED, LOW);
  }
}

void handleNetwork() {
  if (currentState == AP_MODE) {
    server.handleClient();
  } 
  else if (currentState == RUNNING && WiFi.status() != WL_CONNECTED) {
    Serial.println("Connection lost. Reconnecting...");
    digitalWrite(LED_BLUE, LOW);
    
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
  delay(500);
  Serial.println("\n--- Smart Inhaler Booting ---");

  // Init Pins
  pinMode(LED_BLUE, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  
  if (PRESSURE_SENSOR_ENABLED == 1) {
    analogReadResolution(12);
  }

  // Attach Interrupt
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flow_isr, RISING);

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
