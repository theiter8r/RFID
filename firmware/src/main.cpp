#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <vector>

#include "secrets.h"

namespace Pins {
constexpr uint8_t RFID_SS = 17;
constexpr uint8_t RFID_RST = 16;
constexpr uint8_t SPI_SCK = 18;
constexpr uint8_t SPI_MISO = 19;
constexpr uint8_t SPI_MOSI = 23;
constexpr uint8_t PROXIMITY_OUT = 21;
constexpr uint8_t DOOR_CONTACT = 32;
constexpr uint8_t EXIT_BUTTON_NC = 13;
constexpr uint8_t LOCK_DRIVER = 27;
constexpr uint8_t GREEN_LED = 25;
constexpr uint8_t RED_LED = 26;
constexpr uint8_t BUZZER = 33;
constexpr uint8_t REMOTE_EXIT_IN = 4;
}  // namespace Pins

constexpr char FIRMWARE_VERSION[] = "0.1.2";
// GPIO27 drives a BC547 level shifter. HIGH turns the BC547 on, pulling the
// active-low 5 V relay module input LOW and energizing the relay.
constexpr bool LOCK_OUTPUT_ACTIVE_HIGH = false;
constexpr uint32_t DEFAULT_UNLOCK_WINDOW_MS = 5000;
constexpr uint32_t DEFAULT_HELD_OPEN_ALARM_MS = 30000;
constexpr uint32_t DOOR_CLOSED_DEBOUNCE_MS = 500;
constexpr uint32_t API_TIMEOUT_MS = 2000;
constexpr uint32_t SYNC_INTERVAL_MS = 60000;
constexpr uint32_t ENROLLMENT_IDLE_POLL_MS = 15000;
constexpr uint32_t ENROLLMENT_ACTIVE_POLL_MS = 2000;
constexpr uint32_t OFFLINE_CACHE_MAX_AGE_HOURS = 24;
constexpr uint32_t RFID_DEBOUNCE_MS = 1500;
constexpr uint32_t RFID_RELAY_RECOVERY_DELAY_MS = 350;
constexpr uint32_t RFID_HEALTH_CHECK_MS = 5000;
constexpr uint32_t PROXIMITY_CONFIRM_MS = 120;
constexpr uint32_t PROXIMITY_REARM_CLEAR_MS = 700;
constexpr uint32_t REX_COOLDOWN_MS = 3000;
constexpr uint32_t REMOTE_EXIT_CONFIRM_MS = 80;
constexpr size_t MAX_EVENT_QUEUE_BYTES = 32768;
constexpr bool EXIT_BUTTON_INSTALLED = false;  // Set true only after wiring its NC loop.
constexpr bool DOOR_SENSOR_INSTALLED = false;  // Timed relock until a contact is installed.

constexpr char CACHE_PATH[] = "/allow-list.json";
constexpr char EVENTS_PATH[] = "/events.ndjson";
constexpr char TEMP_PATH[] = "/rewrite.tmp";

MFRC522 rfid(Pins::RFID_SS, Pins::RFID_RST);
struct CachedCard {
  String uid;
  String personId;
  String name;
  bool admin;
};

std::vector<CachedCard> allowList;
time_t cacheSyncedEpoch = 0;
uint64_t cacheRevision = 0;

enum class DoorState { LOCKED, UNLOCK_WAIT_OPEN, UNLOCK_WAIT_CLOSE, HELD_OPEN_ALARM };
DoorState doorState = DoorState::LOCKED;
uint32_t unlockStartedAt = 0;
uint32_t openedAt = 0;
uint32_t closedSince = 0;
uint32_t unlockWindowMs = DEFAULT_UNLOCK_WINDOW_MS;
uint32_t heldOpenAlarmMs = DEFAULT_HELD_OPEN_ALARM_MS;

enum class EnrollmentState { NONE, WAIT_ADMIN, WAIT_CARD };
EnrollmentState enrollmentState = EnrollmentState::NONE;
String enrollmentRequestId;
String enrollmentPersonName;
uint32_t enrollmentLocalDeadline = 0;

enum class Feedback { NONE, SUCCESS, DENIED, ENROLLED };
Feedback feedback = Feedback::NONE;
uint32_t feedbackStartedAt = 0;

uint32_t lastSyncAt = 0;
uint32_t lastEnrollmentPollAt = 0;
uint32_t lastRexAt = 0;
uint32_t lastRfidAt = 0;
String lastUid;
bool rfidReady = false;
bool rfidRecoveryPending = false;
uint32_t rfidRecoveryAt = 0;
uint32_t lastRfidHealthCheckAt = 0;
uint32_t proximityDetectedSince = 0;
uint32_t proximityClearSince = 0;
bool proximityArmed = true;
bool exitButtonWasOpen = false;
uint32_t remoteExitHighSince = 0;

bool elapsed(uint32_t start, uint32_t duration) {
  return static_cast<uint32_t>(millis() - start) >= duration;
}

void setLockUnlocked(bool unlocked) {
  const bool level = unlocked ? LOCK_OUTPUT_ACTIVE_HIGH : !LOCK_OUTPUT_ACTIVE_HIGH;
  digitalWrite(Pins::LOCK_DRIVER, level ? HIGH : LOW);
  Serial.printf("[door] lock output: %s\n", unlocked ? "UNLOCK" : "LOCK");
  // A solenoid/relay transition can inject enough noise to upset an MFRC522.
  // Reset it shortly after every transition, once the switching spike has passed.
  if (rfidReady) {
    rfidRecoveryPending = true;
    rfidRecoveryAt = millis() + RFID_RELAY_RECOVERY_DELAY_MS;
  }
}

bool isDoorClosed() {
  // No sensor: deliberately use the five-second unlock timeout. Door-open and
  // held-open monitoring become available only after a contact is installed.
  return !DOOR_SENSOR_INSTALLED || digitalRead(Pins::DOOR_CONTACT) == LOW;
}

void clearIndicators() {
  digitalWrite(Pins::GREEN_LED, LOW);
  digitalWrite(Pins::RED_LED, LOW);
  digitalWrite(Pins::BUZZER, LOW);
}

void beginFeedback(Feedback next) {
  feedback = next;
  feedbackStartedAt = millis();
  // Apply the first frame immediately. A following HTTP poll must never consume
  // the whole indication window before updateFeedback() gets another loop turn.
  clearIndicators();
  if (next == Feedback::SUCCESS || next == Feedback::ENROLLED) {
    digitalWrite(Pins::GREEN_LED, HIGH);
    digitalWrite(Pins::BUZZER, HIGH);
  } else if (next == Feedback::DENIED) {
    digitalWrite(Pins::RED_LED, HIGH);
    digitalWrite(Pins::BUZZER, HIGH);
  }
}

void acknowledgeCardRead() {
  // Immediate acknowledgement only: final green/red feedback follows the
  // online authorization decision.
  digitalWrite(Pins::BUZZER, HIGH);
  delay(35);
  digitalWrite(Pins::BUZZER, LOW);
}

void updateFeedback() {
  if (doorState == DoorState::HELD_OPEN_ALARM) {
    bool on = (millis() / 250) % 2 == 0;
    digitalWrite(Pins::RED_LED, on);
    digitalWrite(Pins::GREEN_LED, LOW);
    digitalWrite(Pins::BUZZER, on);
    return;
  }

  if (feedback == Feedback::NONE) {
    clearIndicators();
    if (enrollmentState == EnrollmentState::WAIT_ADMIN) {
      digitalWrite(Pins::RED_LED, (millis() / 500) % 2);
    } else if (enrollmentState == EnrollmentState::WAIT_CARD) {
      digitalWrite(Pins::GREEN_LED, (millis() / 500) % 2);
    }
    return;
  }

  uint32_t age = millis() - feedbackStartedAt;
  clearIndicators();
  if (feedback == Feedback::SUCCESS) {
    digitalWrite(Pins::GREEN_LED, age < 1800);
    digitalWrite(Pins::BUZZER, age < 120);
    if (age >= 1800) feedback = Feedback::NONE;
  } else if (feedback == Feedback::DENIED) {
    bool pulse = (age < 180) || (age >= 320 && age < 500);
    digitalWrite(Pins::RED_LED, age < 1800);
    digitalWrite(Pins::BUZZER, pulse);
    if (age >= 1800) feedback = Feedback::NONE;
  } else if (feedback == Feedback::ENROLLED) {
    digitalWrite(Pins::GREEN_LED, (age / 250) % 2 == 0);
    digitalWrite(Pins::BUZZER, age < 100 || (age >= 180 && age < 280));
    if (age >= 3000) feedback = Feedback::NONE;
  }
}

String newUuid() {
  uint8_t bytes[16];
  for (uint8_t &byte : bytes) byte = static_cast<uint8_t>(esp_random());
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
  File file = LittleFS.open(EVENTS_PATH, FILE_APPEND);
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
  WiFiClientSecure client;
  if (ALLOW_INSECURE_TLS) {
    client.setInsecure();
  } else {
    client.setCACert(SUPABASE_ROOT_CA);
  }
  HTTPClient http;
  http.setConnectTimeout(API_TIMEOUT_MS);
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
  File file = LittleFS.open(CACHE_PATH, FILE_READ);
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
  File file = LittleFS.open(TEMP_PATH, FILE_WRITE);
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
    unlockWindowMs = result["unlock_window_ms"] | DEFAULT_UNLOCK_WINDOW_MS;
    heldOpenAlarmMs = result["held_open_alarm_ms"] | DEFAULT_HELD_OPEN_ALARM_MS;
    if (saveCache(result)) Serial.println("[sync] allow-list updated");
  } else if (status > 0) {
    Serial.printf("[sync] server rejected request: HTTP %d\n", status);
  }
}

void flushQueuedEvents() {
  if (WiFi.status() != WL_CONNECTED || !LittleFS.exists(EVENTS_PATH)) return;
  File source = LittleFS.open(EVENTS_PATH, FILE_READ);
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
    File temp = LittleFS.open(TEMP_PATH, FILE_WRITE);
    if (temp) {
      temp.print(remainder);
      temp.close();
      LittleFS.remove(EVENTS_PATH);
      LittleFS.rename(TEMP_PATH, EVENTS_PATH);
    }
    Serial.printf("[log] uploaded %u queued events\n", count);
  }
}

void requestUnlock(const char *reason) {
  if (doorState != DoorState::LOCKED) return;
  setLockUnlocked(true);
  doorState = DoorState::UNLOCK_WAIT_OPEN;
  unlockStartedAt = millis();
  openedAt = 0;
  closedSince = 0;
  Serial.printf("[door] unlock reason: %s\n", reason);
}

void updateDoorState() {
  const bool closed = isDoorClosed();
  if (doorState == DoorState::LOCKED) return;

  if (doorState == DoorState::UNLOCK_WAIT_OPEN) {
    if (!closed) {
      doorState = DoorState::UNLOCK_WAIT_CLOSE;
      openedAt = millis();
      queueEvent("door_opened", "CONTACT_OPEN", WiFi.status() != WL_CONNECTED);
    } else if (elapsed(unlockStartedAt, unlockWindowMs)) {
      setLockUnlocked(false);
      doorState = DoorState::LOCKED;
    }
    return;
  }

  if (closed) {
    if (!closedSince) closedSince = millis();
    if (elapsed(closedSince, DOOR_CLOSED_DEBOUNCE_MS)) {
      setLockUnlocked(false);
      queueEvent("door_closed", "CONTACT_CLOSED_STABLE", WiFi.status() != WL_CONNECTED);
      doorState = DoorState::LOCKED;
      feedback = Feedback::NONE;
    }
  } else {
    closedSince = 0;
    if (doorState == DoorState::UNLOCK_WAIT_CLOSE && elapsed(openedAt, heldOpenAlarmMs)) {
      doorState = DoorState::HELD_OPEN_ALARM;
      queueEvent("door_held_open", "CONTACT_OPEN_TOO_LONG", WiFi.status() != WL_CONNECTED);
    }
  }
}

void initializeRfid(const char *reason) {
  rfid.PCD_Init();
  delay(4);
  const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidReady = version != 0x00 && version != 0xFF;
  rfidRecoveryPending = false;
  lastRfidHealthCheckAt = millis();
  Serial.printf("[rfid] %s; version=0x%02X; ready=%s\n", reason, version,
                rfidReady ? "true" : "false");
}

void serviceRfidRecovery() {
  if (rfidRecoveryPending &&
      static_cast<int32_t>(millis() - rfidRecoveryAt) >= 0) {
    initializeRfid("reinitialized after relay transition");
    return;
  }

  if (!rfidReady) {
    if (elapsed(lastRfidHealthCheckAt, 1000)) initializeRfid("recovery retry");
    return;
  }

  if (elapsed(lastRfidHealthCheckAt, RFID_HEALTH_CHECK_MS)) {
    lastRfidHealthCheckAt = millis();
    const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (version == 0x00 || version == 0xFF) initializeRfid("health-check recovery");
  }
}

String readUid() {
  if (!rfidReady || rfidRecoveryPending) return "";
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

void authorizeBadge(const String &uid) {
  JsonDocument request;
  request["action"] = "authorize";
  request["uid"] = uid;
  JsonDocument result;
  int status = postApi(request, result);

  if (status == 200 && result["ok"].as<bool>()) {
    if (result["granted"].as<bool>()) {
      unlockWindowMs = result["unlock_window_ms"] | DEFAULT_UNLOCK_WINDOW_MS;
      String name = result["person"]["name"] | "authorized user";
      Serial.printf("[access] online grant: %s\n", name.c_str());
      beginFeedback(Feedback::SUCCESS);
      requestUnlock("ONLINE_BADGE");
    } else {
      Serial.printf("[access] online denial: %s\n", String(result["reason"] | "DENIED").c_str());
      beginFeedback(Feedback::DENIED);
    }
    return;
  }

  // Only transport failures and server outages use cache. Authentication/client
  // errors are explicit denials, never an excuse to fall back.
  if (status < 0 || status >= 500) {
    const CachedCard *card = cacheIsFresh() ? findCachedCard(uid) : nullptr;
    if (card) {
      Serial.printf("[access] OFFLINE grant: %s\n", card->name.c_str());
      queueEvent("granted", "OFFLINE_CACHE", true, uid, card->personId);
      beginFeedback(Feedback::SUCCESS);
      requestUnlock("OFFLINE_BADGE");
    } else {
      const char *reason = cacheIsFresh() ? "OFFLINE_UNKNOWN_CARD" : "OFFLINE_CACHE_STALE";
      Serial.printf("[access] offline denial: %s\n", reason);
      queueEvent("denied", reason, true, uid);
      beginFeedback(Feedback::DENIED);
    }
  } else {
    Serial.printf("[access] API/auth denial HTTP %d\n", status);
    beginFeedback(Feedback::DENIED);
  }
}

void pollEnrollment() {
  JsonDocument request;
  request["action"] = "enrollment_status";
  JsonDocument result;
  int status = postApi(request, result);
  lastEnrollmentPollAt = millis();
  if (status != 200 || !result["ok"].as<bool>()) return;
  if (!result["active"].as<bool>()) {
    enrollmentState = EnrollmentState::NONE;
    enrollmentRequestId = "";
    enrollmentPersonName = "";
    return;
  }
  enrollmentRequestId = String(result["enrollment"]["id"] | "");
  enrollmentPersonName = String(result["enrollment"]["person_name"] | "new person");
  String state = result["enrollment"]["status"] | "";
  enrollmentState = state == "awaiting_card" ? EnrollmentState::WAIT_CARD : EnrollmentState::WAIT_ADMIN;
  enrollmentLocalDeadline = millis() + (enrollmentState == EnrollmentState::WAIT_CARD ? 30000 : 150000);
  Serial.printf("[enroll] %s for %s\n",
                enrollmentState == EnrollmentState::WAIT_ADMIN ? "scan admin" : "scan new card",
                enrollmentPersonName.c_str());
}

void handleEnrollmentBadge(const String &uid) {
  JsonDocument request;
  request["request_id"] = enrollmentRequestId;
  request["uid"] = uid;
  request["action"] = enrollmentState == EnrollmentState::WAIT_ADMIN
                        ? "verify_enrollment_admin" : "complete_enrollment";
  JsonDocument result;
  int status = postApi(request, result);
  if (status != 200 || !result["ok"].as<bool>()) {
    beginFeedback(Feedback::DENIED);
    Serial.println("[enroll] online API required");
    return;
  }
  if (enrollmentState == EnrollmentState::WAIT_ADMIN) {
    if (result["verified"].as<bool>()) {
      enrollmentState = EnrollmentState::WAIT_CARD;
      enrollmentLocalDeadline = millis() + 30000;
      beginFeedback(Feedback::SUCCESS);
      Serial.println("[enroll] admin verified; scan new badge");
    } else {
      beginFeedback(Feedback::DENIED);
      Serial.printf("[enroll] %s\n", String(result["reason"] | "ADMIN_REQUIRED").c_str());
    }
  } else if (result["enrolled"].as<bool>()) {
    beginFeedback(Feedback::ENROLLED);
    Serial.printf("[enroll] card %s registered to %s\n", uid.c_str(), enrollmentPersonName.c_str());
    enrollmentState = EnrollmentState::NONE;
    enrollmentRequestId = "";
    syncAllowList();
  } else {
    beginFeedback(Feedback::DENIED);
    Serial.printf("[enroll] failed: %s\n", String(result["reason"] | "FAILED").c_str());
  }
}

void updateRexSensors() {
  if (EXIT_BUTTON_INSTALLED) {
    bool exitOpen = digitalRead(Pins::EXIT_BUTTON_NC) == HIGH;
    if (exitOpen && !exitButtonWasOpen && elapsed(lastRexAt, REX_COOLDOWN_MS)) {
      lastRexAt = millis();
      queueEvent("exit", "NC_EXIT_BUTTON", WiFi.status() != WL_CONNECTED);
      beginFeedback(Feedback::SUCCESS);
      requestUnlock("EXIT_BUTTON");
    }
    exitButtonWasOpen = exitOpen;
  }

  // Inside NodeMCU exit reader pulses this pin HIGH after its own online/
  // offline-cache authorization already granted the tap and logged an
  // identified 'exit' event to Supabase. Trusted blindly here, same as the
  // exit button/FC-51 above — no local identity to re-verify.
  const bool remoteExitHigh = digitalRead(Pins::REMOTE_EXIT_IN) == HIGH;
  if (remoteExitHigh) {
    if (!remoteExitHighSince) remoteExitHighSince = millis();
    if (elapsed(remoteExitHighSince, REMOTE_EXIT_CONFIRM_MS) && elapsed(lastRexAt, REX_COOLDOWN_MS)) {
      lastRexAt = millis();
      beginFeedback(Feedback::SUCCESS);
      requestUnlock("REMOTE_EXIT_NODEMCU");
      // No queueEvent() here — the NodeMCU already wrote the identified
      // exit event; logging again would double-count the same exit.
    }
  } else {
    remoteExitHighSince = 0;
  }

  // FC-51 OUT is active-low. Its onboard potentiometer physically sets the
  // close-hand distance; tune it to roughly 5–12 cm after mounting.
  const bool closeHand = digitalRead(Pins::PROXIMITY_OUT) == LOW;
  if (closeHand) {
    proximityClearSince = 0;
    if (!proximityDetectedSince) proximityDetectedSince = millis();
  } else {
    proximityDetectedSince = 0;
    if (!proximityClearSince) proximityClearSince = millis();
    if (!proximityArmed && elapsed(proximityClearSince, PROXIMITY_REARM_CLEAR_MS)) {
      proximityArmed = true;
    }
  }
  if (closeHand && proximityArmed && elapsed(proximityDetectedSince, PROXIMITY_CONFIRM_MS) &&
      elapsed(lastRexAt, REX_COOLDOWN_MS)) {
    proximityArmed = false;
    lastRexAt = millis();
    queueEvent("exit", "TOUCHLESS_REX", WiFi.status() != WL_CONNECTED);
    beginFeedback(Feedback::SUCCESS);
    requestUnlock("TOUCHLESS_REX");
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
  // Preload the safe output latch before enabling the GPIO as an output.
  digitalWrite(Pins::LOCK_DRIVER, LOCK_OUTPUT_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(Pins::LOCK_DRIVER, OUTPUT);
  pinMode(Pins::GREEN_LED, OUTPUT);
  pinMode(Pins::RED_LED, OUTPUT);
  pinMode(Pins::BUZZER, OUTPUT);
  pinMode(Pins::DOOR_CONTACT, INPUT_PULLUP);
  pinMode(Pins::EXIT_BUTTON_NC, INPUT_PULLUP);
  pinMode(Pins::PROXIMITY_OUT, INPUT_PULLUP);
  pinMode(Pins::REMOTE_EXIT_IN, INPUT_PULLDOWN);
  setLockUnlocked(false);
  clearIndicators();

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS mount failed");
  loadCache();

  SPI.begin(Pins::SPI_SCK, Pins::SPI_MISO, Pins::SPI_MOSI, Pins::RFID_SS);
  initializeRfid("initialized at startup");
  Serial.println("[sensor] FC-51 proximity ready; adjust its potentiometer to 5-12 cm");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  // Visible startup test, without moving the lock.
  digitalWrite(Pins::GREEN_LED, HIGH);
  digitalWrite(Pins::RED_LED, HIGH);
  digitalWrite(Pins::BUZZER, HIGH);
  delay(120);
  clearIndicators();
  Serial.printf("RFID controller firmware %s ready; door_closed=%s\n",
                FIRMWARE_VERSION, isDoorClosed() ? "true" : "false");
}

void loop() {
  connectWiFiNonBlocking();
  updateDoorState();
  updateRexSensors();
  serviceRfidRecovery();
  updateFeedback();

  if (enrollmentState != EnrollmentState::NONE &&
      static_cast<int32_t>(millis() - enrollmentLocalDeadline) >= 0) {
    Serial.println("[enroll] local timeout");
    enrollmentState = EnrollmentState::NONE;
    enrollmentRequestId = "";
  }

  String uid = readUid();
  const bool cardReadThisLoop = uid.length() > 0;
  if (cardReadThisLoop) {
    Serial.printf("[rfid] %s\n", uid.c_str());
    acknowledgeCardRead();
    if (enrollmentState == EnrollmentState::NONE) authorizeBadge(uid);
    else handleEnrollmentBadge(uid);
  }

  // Background cloud polling comes after the time-critical RFID path.
  const uint32_t enrollmentPollInterval = enrollmentState == EnrollmentState::NONE
                                            ? ENROLLMENT_IDLE_POLL_MS : ENROLLMENT_ACTIVE_POLL_MS;
  if (!cardReadThisLoop &&
      ((lastEnrollmentPollAt == 0 && WiFi.status() == WL_CONNECTED) ||
       elapsed(lastEnrollmentPollAt, enrollmentPollInterval))) pollEnrollment();

  if (!cardReadThisLoop &&
      ((lastSyncAt == 0 && WiFi.status() == WL_CONNECTED) || elapsed(lastSyncAt, SYNC_INTERVAL_MS))) {
    syncAllowList();
    flushQueuedEvents();
  }
  delay(20);
}
