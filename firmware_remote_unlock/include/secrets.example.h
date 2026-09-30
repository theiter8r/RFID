#pragma once

// Copy this file to secrets.h. Never commit secrets.h.
// These are a THIRD device's credentials (queries/06_create_remote_unlock_device.sql),
// distinct from both the entry ESP32's (firmware/) and the exit NodeMCU's
// (firmware_exit_reader/) secrets.h — do not reuse either device's
// DEVICE_ID/DEVICE_SECRET here.
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Example: https://abcdefgh.supabase.co/functions/v1/device-api
#define DEVICE_API_URL "https://YOUR_PROJECT_REF.supabase.co/functions/v1/device-api"

// The publishable/anon key is only a gateway key. The per-device secret below
// performs device authentication. Never put a service-role/secret key here.
#define SUPABASE_PUBLISHABLE_KEY "YOUR_PUBLISHABLE_OR_ANON_KEY"
#define DEVICE_ID "UUID_RETURNED_BY_06_CREATE_REMOTE_UNLOCK_DEVICE"
#define DEVICE_SECRET "ORIGINAL_RANDOM_DEVICE_SECRET_NOT_ITS_HASH"

// Paste the same current PEM root CA used by the other two devices' secrets.h —
// all three talk to the same Supabase hostname.
static const char SUPABASE_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
PASTE_ROOT_CA_CERTIFICATE_HERE
-----END CERTIFICATE-----
)PEM";

// Only set true for short, isolated bench debugging. Production must validate TLS.
constexpr bool ALLOW_INSECURE_TLS = false;
