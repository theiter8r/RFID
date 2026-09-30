#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>

#include "secrets.h"

// Standalone remote-unlock trigger. Independent Wi-Fi device with its own
// Supabase device identity (queries/06_create_remote_unlock_device.sql) under
// the SAME door as the entry ESP32. It never touches the lock, the allow-list,
// or badge auth: it only polls the dashboard's "Open door" requests and, when
// one is pending, pulses HANDSHAKE_OUT — wired (through a series diode; see
// docs/WIRING.md) onto the SAME entry-ESP32 pin the exit-reader NodeMCU
// already drives (Pins::REMOTE_EXIT_IN, GPIO4 on the entry board,
// firmware/src/main.cpp). That pin's handling already exists and ships today
// on the entry board, so this device requires ZERO changes to it.
//
// The diode on each of the two devices' output wires prevents them from ever
// contending on the shared pin: whichever one pulses HIGH wins, and an idle
// (LOW) device cannot pull the other's HIGH pulse down.

namespace Pins {
constexpr uint8_t HANDSHAKE_OUT = 5;  // D1, through a diode to entry ESP32's REMOTE_EXIT_IN (GPIO4)
constexpr uint8_t STATUS_LED = 2;     // D4, onboard LED, active LOW
}  // namespace Pins

constexpr char FIRMWARE_VERSION[] = "0.1.0";
constexpr uint32_t API_TIMEOUT_MS = 2000;
constexpr uint32_t POLL_INTERVAL_MS = 3000;
constexpr uint32_t HANDSHAKE_PULSE_MS = 200;

uint32_t lastPollAt = 0;
uint32_t ledOffAt = 0;

bool elapsed(uint32_t start, uint32_t duration) {
  return static_cast<uint32_t>(millis() - start) >= duration;
}

void pulseHandshake() {
  digitalWrite(Pins::HANDSHAKE_OUT, HIGH);
  digitalWrite(Pins::STATUS_LED, LOW);  // active-low LED on
  ledOffAt = millis() + 400;
  delay(HANDSHAKE_PULSE_MS);
  digitalWrite(Pins::HANDSHAKE_OUT, LOW);
}

void updateStatusLed() {
  if (ledOffAt && elapsed(ledOffAt - 400, 400)) {
    digitalWrite(Pins::STATUS_LED, HIGH);
    ledOffAt = 0;
  }
}

int postApi(JsonDocument &requestDoc, JsonDocument &responseDoc) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  BearSSL::WiFiClientSecure client;
  BearSSL::X509List caCert(SUPABASE_ROOT_CA);
  if (ALLOW_INSECURE_TLS) {
    client.setInsecure();
  } else {
    client.setTrustAnchors(&caCert);
  }
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  if (!http.begin(client, DEVICE_API_URL)) return -1;
  http.addHeader("content-type", "application/json");
  http.addHeader("apikey", SUPABASE_PUBLISHABLE_KEY);
  http.addHeader("x-device-id", DEVICE_ID);
  http.addHeader("x-device-secret", DEVICE_SECRET);
  http.addHeader("x-firmware-version", FIRMWARE_VERSION);
  String payload;
  serializeJson(requestDoc, payload);
  int status = http.POST(payload);
  if (status > 0) {
    DeserializationError error = deserializeJson(responseDoc, http.getString());
    if (error) Serial.printf("[api] invalid JSON: %s\n", error.c_str());
  } else {
    Serial.printf("[api] transport error: %s\n", http.errorToString(status).c_str());
  }
  http.end();
  return status;
}

void pollRemoteUnlock() {
  JsonDocument request;
  request["action"] = "remote_unlock_status";
  JsonDocument result;
  int status = postApi(request, result);
  lastPollAt = millis();
  if (status == 200 && result["ok"].as<bool>() && result["active"].as<bool>()) {
    Serial.println("[unlock] pending request found; pulsing handshake");
    pulseHandshake();
  }
}

void connectWiFiNonBlocking() {
  static uint32_t lastAttempt = 0;
  if (WiFi.status() == WL_CONNECTED || !elapsed(lastAttempt, 10000)) return;
  lastAttempt = millis();
  Serial.println("[wifi] reconnecting");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void setup() {
  Serial.begin(115200);
  pinMode(Pins::HANDSHAKE_OUT, OUTPUT);
  digitalWrite(Pins::HANDSHAKE_OUT, LOW);
  pinMode(Pins::STATUS_LED, OUTPUT);
  digitalWrite(Pins::STATUS_LED, HIGH);  // off

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  digitalWrite(Pins::STATUS_LED, LOW);
  delay(120);
  digitalWrite(Pins::STATUS_LED, HIGH);
  Serial.printf("Remote-unlock trigger firmware %s ready\n", FIRMWARE_VERSION);
}

void loop() {
  connectWiFiNonBlocking();
  updateStatusLed();

  if ((lastPollAt == 0 && WiFi.status() == WL_CONNECTED) || elapsed(lastPollAt, POLL_INTERVAL_MS)) {
    pollRemoteUnlock();
  }
  delay(20);
}
