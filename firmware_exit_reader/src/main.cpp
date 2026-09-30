#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>
#include <vector>

#include "secrets.h"

// Inside/exit RFID reader. Independent Wi-Fi device with its own Supabase
// device identity (queries/05_create_exit_device.sql) under the SAME door as
// the entry ESP32. On a granted tap it (a) logs an identified 'exit' event to
// Supabase itself via action:"authorize", direction:"exit", and (b) pulses
// EXIT_HANDSHAKE_OUT so the entry ESP32 (the only board that drives the
// relay) unlocks the door. This board never touches the lock directly.
//
// Free egress is unaffected by this board's presence or absence: the entry
// ESP32's existing touchless/exit-button paths keep working even if this
// NodeMCU is powered off (see firmware/src/main.cpp updateRexSensors()).

namespace Pins {
constexpr uint8_t RFID_SS = 15;            // D8
constexpr uint8_t GREEN_LED = 4;           // D2
constexpr uint8_t RED_LED = 16;            // D0
constexpr uint8_t EXIT_HANDSHAKE_OUT = 5;  // D1, to entry ESP32's REMOTE_EXIT_IN (GPIO4)
}  // namespace Pins

constexpr char FIRMWARE_VERSION[] = "0.1.0";
constexpr uint32_t API_TIMEOUT_MS = 2000;
constexpr uint32_t SYNC_INTERVAL_MS = 60000;
constexpr uint32_t OFFLINE_CACHE_MAX_AGE_HOURS = 24;
constexpr uint32_t RFID_DEBOUNCE_MS = 1500;
constexpr uint32_t HANDSHAKE_PULSE_MS = 200;
constexpr size_t MAX_EVENT_QUEUE_BYTES = 32768;

constexpr char CACHE_PATH[] = "/allow-list.json";
constexpr char EVENTS_PATH[] = "/events.ndjson";
constexpr char TEMP_PATH[] = "/rewrite.tmp";

// RST tied directly to 3.3V (see docs) — soft-reset-only mode.
MFRC522 rfid(Pins::RFID_SS, MFRC522::UNUSED_PIN);

struct CachedCard {
  String uid;
  String personId;
  String name;
  bool admin;
};

std::vector<CachedCard> allowList;
time_t cacheSyncedEpoch = 0;
uint64_t cacheRevision = 0;

enum class Feedback { NONE, SUCCESS, DENIED };
Feedback feedback = Feedback::NONE;
uint32_t feedbackStartedAt = 0;

uint32_t lastSyncAt = 0;
uint32_t lastRfidAt = 0;
String lastUid;
bool rfidReady = false;

bool elapsed(uint32_t start, uint32_t duration) {
  return static_cast<uint32_t>(millis() - start) >= duration;
}

void clearIndicators() {
  digitalWrite(Pins::GREEN_LED, LOW);
  digitalWrite(Pins::RED_LED, LOW);
}

void beginFeedback(Feedback next) {
  feedback = next;
  feedbackStartedAt = millis();
  clearIndicators();
  if (next == Feedback::SUCCESS) digitalWrite(Pins::GREEN_LED, HIGH);
  else if (next == Feedback::DENIED) digitalWrite(Pins::RED_LED, HIGH);
}

void updateFeedback() {
  if (feedback == Feedback::NONE) return;
  if (elapsed(feedbackStartedAt, 600)) {
    clearIndicators();
    feedback = Feedback::NONE;
  }
}

void pulseHandshake() {
  digitalWrite(Pins::EXIT_HANDSHAKE_OUT, HIGH);
  delay(HANDSHAKE_PULSE_MS);
  digitalWrite(Pins::EXIT_HANDSHAKE_OUT, LOW);
}

String newUuid() {
  uint8_t bytes[16];
  for (uint8_t &byte : bytes) byte = static_cast<uint8_t>(ESP.random());
  bytes[6] = (bytes[6] & 0x0F) | 0x40;
  bytes[8] = (bytes[8] & 0x3F) | 0x80;
  char out[37];
  snprintf(out, sizeof(out),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return String(out);
}

String deviceTime() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "uptime-ms:" + String(millis());
  struct tm value;
  gmtime_r(&now, &value);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value);
  return String(buffer);
}

void queueEvent(const char *decision, const String &reason, bool offline,
                const String &uid = "", const String &personId = "") {
  File file = LittleFS.open(EVENTS_PATH, "a");
  if (!file) return;
  if (file.size() >= MAX_EVENT_QUEUE_BYTES) {
    file.close();
    Serial.println("[log] queue full; newest event dropped");
    return;
  }
  JsonDocument event;
  event["event_uuid"] = newUuid();
  event["decision"] = decision;
  event["reason"] = reason;
  event["offline"] = offline;
  event["device_time"] = deviceTime();
  if (uid.length()) event["uid"] = uid;
  if (personId.length()) event["person_id"] = personId;
  serializeJson(event, file);
  file.print('\n');
  file.close();
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

void loadCache() {
  allowList.clear();
  File file = LittleFS.open(CACHE_PATH, "r");
  if (!file) {
    Serial.println("[cache] no local allow-list");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, file)) {
    file.close();
    Serial.println("[cache] invalid allow-list");
    return;
  }
  file.close();
  cacheSyncedEpoch = doc["synced_epoch"] | 0;
  cacheRevision = doc["revision"] | 0;
  for (JsonObject card : doc["cards"].as<JsonArray>()) {
    allowList.push_back({
      String(card["uid"] | ""), String(card["person_id"] | ""),
      String(card["name"] | ""), String(card["role"] | "") == "admin",
    });
  }
  Serial.printf("[cache] loaded %u cards, revision %llu\n",
                allowList.size(), cacheRevision);
}

bool saveCache(const JsonDocument &apiDoc) {
  JsonDocument cache;
  cache["revision"] = apiDoc["revision"] | 0;
  cache["synced_epoch"] = static_cast<int64_t>(time(nullptr));
  cache["cards"] = apiDoc["cards"];
  File file = LittleFS.open(TEMP_PATH, "w");
  if (!file || serializeJson(cache, file) == 0) {
    if (file) file.close();
    return false;
  }
  file.flush();
  file.close();
  LittleFS.remove(CACHE_PATH);
  if (!LittleFS.rename(TEMP_PATH, CACHE_PATH)) return false;
  loadCache();
  return true;
}

bool cacheIsFresh() {
  time_t now = time(nullptr);
  if (now < 1700000000 || cacheSyncedEpoch < 1700000000) return false;
  return now >= cacheSyncedEpoch &&
         static_cast<uint64_t>(now - cacheSyncedEpoch) <= OFFLINE_CACHE_MAX_AGE_HOURS * 3600ULL;
}

const CachedCard *findCachedCard(const String &uid) {
  for (const CachedCard &card : allowList) if (card.uid == uid) return &card;
  return nullptr;
}

void syncAllowList() {
  JsonDocument request;
  request["action"] = "sync";
  JsonDocument result;
  int status = postApi(request, result);
  lastSyncAt = millis();
  if (status == 200 && result["ok"].as<bool>()) {
    if (saveCache(result)) Serial.println("[sync] allow-list updated");
  } else if (status > 0) {
    Serial.printf("[sync] server rejected request: HTTP %d\n", status);
  }
}

void flushQueuedEvents() {
  if (WiFi.status() != WL_CONNECTED || !LittleFS.exists(EVENTS_PATH)) return;
  File source = LittleFS.open(EVENTS_PATH, "r");
  if (!source || source.size() == 0) {
    if (source) source.close();
    return;
  }
  JsonDocument request;
  request["action"] = "log_batch";
  JsonArray events = request["events"].to<JsonArray>();
  String remainder;
  size_t count = 0;
  while (source.available()) {
    String line = source.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (count < 50) {
      JsonDocument one;
      if (!deserializeJson(one, line)) {
        events.add(one.as<JsonObject>());
        count++;
      }
    } else {
      remainder += line + "\n";
    }
  }
  source.close();
  if (!count) return;
  JsonDocument result;
  int status = postApi(request, result);
  if (status == 200 && result["ok"].as<bool>()) {
    File temp = LittleFS.open(TEMP_PATH, "w");
    if (temp) {
      temp.print(remainder);
      temp.close();
      LittleFS.remove(EVENTS_PATH);
      LittleFS.rename(TEMP_PATH, EVENTS_PATH);
    }
    Serial.printf("[log] uploaded %u queued events\n", count);
  }
}

void initializeRfid() {
  rfid.PCD_Init();
  delay(4);
  const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidReady = version != 0x00 && version != 0xFF;
  Serial.printf("[rfid] initialized; version=0x%02X; ready=%s\n", version,
                rfidReady ? "true" : "false");
}

String readUid() {
  if (!rfidReady) return "";
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return "";
  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  if (uid == lastUid && !elapsed(lastRfidAt, RFID_DEBOUNCE_MS)) return "";
  lastUid = uid;
  lastRfidAt = millis();
  return uid;
}

// Mirrors firmware/src/main.cpp's authorizeBadge(), scoped to the exit
// direction: online authorize call with direction:"exit"; on transport
// failure/server outage, fall back to the same locally-synced cache the
// entry ESP32 relies on, so this reader still lets people out when offline.
void authorizeExit(const String &uid) {
  JsonDocument request;
  request["action"] = "authorize";
  request["uid"] = uid;
  request["direction"] = "exit";
  JsonDocument result;
  int status = postApi(request, result);

  if (status == 200 && result["ok"].as<bool>()) {
    if (result["granted"].as<bool>()) {
      String name = result["person"]["name"] | "authorized user";
      Serial.printf("[access] online exit grant: %s\n", name.c_str());
      beginFeedback(Feedback::SUCCESS);
      pulseHandshake();
    } else {
      Serial.printf("[access] online exit denial: %s\n", String(result["reason"] | "DENIED").c_str());
      beginFeedback(Feedback::DENIED);
    }
    return;
  }

  if (status < 0 || status >= 500) {
    const CachedCard *card = cacheIsFresh() ? findCachedCard(uid) : nullptr;
    if (card) {
      Serial.printf("[access] OFFLINE exit grant: %s\n", card->name.c_str());
      queueEvent("exit", "OFFLINE_CACHE", true, uid, card->personId);
      beginFeedback(Feedback::SUCCESS);
      pulseHandshake();
    } else {
      const char *reason = cacheIsFresh() ? "OFFLINE_UNKNOWN_CARD" : "OFFLINE_CACHE_STALE";
      Serial.printf("[access] offline exit denial: %s\n", reason);
      queueEvent("denied", reason, true, uid);
      beginFeedback(Feedback::DENIED);
    }
  } else {
    Serial.printf("[access] API/auth denial HTTP %d\n", status);
    beginFeedback(Feedback::DENIED);
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
  pinMode(Pins::GREEN_LED, OUTPUT);
  pinMode(Pins::RED_LED, OUTPUT);
  pinMode(Pins::EXIT_HANDSHAKE_OUT, OUTPUT);
  digitalWrite(Pins::EXIT_HANDSHAKE_OUT, LOW);
  clearIndicators();

  if (!LittleFS.begin()) {
    Serial.println("[fs] LittleFS mount failed; formatting");
    LittleFS.format();
    LittleFS.begin();
  }
  loadCache();

  SPI.begin();
  initializeRfid();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  digitalWrite(Pins::GREEN_LED, HIGH);
  digitalWrite(Pins::RED_LED, HIGH);
  delay(120);
  clearIndicators();
  Serial.printf("Exit reader firmware %s ready\n", FIRMWARE_VERSION);
}

void loop() {
  connectWiFiNonBlocking();
  updateFeedback();

  String uid = readUid();
  const bool cardReadThisLoop = uid.length() > 0;
  if (cardReadThisLoop) {
    Serial.printf("[rfid] %s\n", uid.c_str());
    authorizeExit(uid);
  }

  if (!cardReadThisLoop &&
      ((lastSyncAt == 0 && WiFi.status() == WL_CONNECTED) || elapsed(lastSyncAt, SYNC_INTERVAL_MS))) {
    syncAllowList();
    flushQueuedEvents();
  }
  delay(20);
}
