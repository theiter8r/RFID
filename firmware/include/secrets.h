#pragma once

// Copy this file to secrets.h. Never commit secrets.h.
#define WIFI_SSID "E-cell"
#define WIFI_PASSWORD "123456789$"

// Example: https://abcdefgh.supabase.co/functions/v1/device-api
#define DEVICE_API_URL "https://mggoagfsiqhkteyknpzr.supabase.co/functions/v1/device-api"

// The publishable/anon key is only a gateway key. The per-device secret below
// performs device authentication. Never put a service-role/secret key here.
#define SUPABASE_PUBLISHABLE_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im1nZ29hZ2ZzaXFoa3RleWtucHpyIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODkxMDc1MDYsImV4cCI6MjEwNDY4MzUwNn0.btfPHtMH8FtxyiR8ZY-PhRBj5qN1aFjm1p68Wrdzh6w"
#define DEVICE_ID "70cd0062-1a10-44f6-8bbf-8a821f4d673d"
#define DEVICE_SECRET "fd3e19ad93f541429de49de5139453f12f080487cd337399e2aa9354dd8b3089"

// Paste the current PEM root CA that validates YOUR Supabase project hostname.
// Obtain and verify it during commissioning. Certificate chains can change.
static const char SUPABASE_ROOT_CA[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)PEM";

// Only set true for short, isolated bench debugging. Production must validate TLS.
constexpr bool ALLOW_INSECURE_TLS = false;

