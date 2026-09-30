# Supabase and Admin Dashboard Setup

This guide provisions the hosted database, the device Edge Function, the first
web administrator, the ESP32 device identity, and the first physical admin
badge. The Supabase free plan is sufficient for the intended 50–100-person
prototype.

## 1. Create the Supabase project

1. Sign in at <https://supabase.com/dashboard> and create a new project.
2. Choose a nearby region, set a strong database password, and retain it in a
   password manager.
3. Wait for the project to finish provisioning.
4. Record the project reference from its URL. It is the value before
   `.supabase.co`.

## 2. Install the database schema

1. Open **SQL Editor** in the Supabase Dashboard and create a new query.
2. Copy the complete contents of
   `supabase/migrations/001_initial_schema.sql` into the query.
3. Run it exactly once.
4. In **Table Editor**, confirm that `profiles`, `people`, `cards`, `doors`,
   `devices`, `enrollment_requests`, `access_events`, and `access_revision`
   exist. `doors` should contain `main-door`.

The migration enables Row Level Security and adds the live dashboard tables to
the `supabase_realtime` publication.

## 3. Create the first dashboard administrator

1. In Supabase, open **Authentication → Users**.
2. Add a password user using the administrator's email address. If the
   Dashboard offers an auto-confirm option, enable it for this manually created
   account.
3. Return to SQL Editor and run the following after changing the email and
   display name:

```sql
update public.profiles as p
set role = 'admin', display_name = 'Maaz'
from auth.users as u
where p.id = u.id
  and lower(u.email) = lower('YOUR_ADMIN_EMAIL@example.com');

select u.email, p.display_name, p.role
from public.profiles p
join auth.users u on u.id = p.id;
```

The final query must show `admin`. A dashboard account in `profiles` is
separate from a physical badge holder in `people`.

## 4. Create the ESP32 device identity

Run this once in SQL Editor. It generates a 64-character random secret, stores
only its SHA-256 hash, attaches the device to `main-door`, and returns the
original secret once:

```sql
with generated as (
  select gen_random_uuid() as id,
         encode(gen_random_bytes(32), 'hex') as device_secret
), inserted as (
  insert into public.devices (id, door_id, name, secret_sha256)
  select g.id,
         d.id,
         'Main Door ESP32',
         encode(digest(g.device_secret, 'sha256'), 'hex')
  from generated g
  cross join (
    select id from public.doors where code = 'main-door'
  ) d
  returning id
)
select g.id as device_id, g.device_secret
from generated g
join inserted i on i.id = g.id;
```

Immediately save both returned values securely. Do not run it repeatedly; each
run creates another device. The plaintext device secret cannot be recovered
from the database later.

## 5. Obtain the browser/device public credentials

In the project **Connect** dialog or **Settings → API Keys**, copy:

- Project URL: `https://PROJECT_REF.supabase.co`
- Publishable key: begins with `sb_publishable_` on new projects

The publishable key is safe for the browser and firmware because RLS and the
separate per-device secret enforce authorization. Never put a secret key or
legacy `service_role` key in the browser or ESP32.

## 6. Deploy the device Edge Function

From PowerShell in the repository root:

```powershell
cd "C:\Users\Maaz Khan\Documents\ChatGPT\RFID DOOR LOCK SYSTEM"
npx supabase@latest init
npx supabase@latest login
npx supabase@latest link --project-ref YOUR_PROJECT_REF
npx supabase@latest functions deploy device-api --no-verify-jwt
```

If `init` reports that the project is already initialized, continue. The
function deliberately uses `--no-verify-jwt` because the ESP32 does not possess
a user JWT; the function performs its own device-ID and constant-time
device-secret verification. Do not remove that verification from the function.

The deployed endpoint is:

```text
https://YOUR_PROJECT_REF.supabase.co/functions/v1/device-api
```

## 7. Configure and flash the ESP32

Copy `firmware/include/secrets.example.h` to
`firmware/include/secrets.h`, then set:

```cpp
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define DEVICE_API_URL "https://YOUR_PROJECT_REF.supabase.co/functions/v1/device-api"
#define SUPABASE_PUBLISHABLE_KEY "sb_publishable_YOUR_KEY"
#define DEVICE_ID "UUID_RETURNED_BY_THE_DEVICE_SQL"
#define DEVICE_SECRET "64_CHARACTER_SECRET_RETURNED_BY_THE_DEVICE_SQL"
```

For an isolated bench test only, set `ALLOW_INSECURE_TLS` to `true`. Before
permanent deployment, install the correct CA certificate and return it to
`false`.

Build, upload, and monitor:

```powershell
cd "C:\Users\Maaz Khan\Documents\ChatGPT\RFID DOOR LOCK SYSTEM\firmware"
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --baud 115200
```

## 8. Start the local dashboard

Create its environment file and start Vite:

```powershell
cd "C:\Users\Maaz Khan\Documents\ChatGPT\RFID DOOR LOCK SYSTEM\web"
Copy-Item -LiteralPath ".env.example" -Destination ".env"
notepad .env
npm install
npm run dev -- --host 0.0.0.0
```

Set `.env` to:

```dotenv
VITE_SUPABASE_URL=https://YOUR_PROJECT_REF.supabase.co
VITE_SUPABASE_PUBLISHABLE_KEY=sb_publishable_YOUR_KEY
```

Open <http://localhost:5173> on the same computer and sign in using the Auth
administrator created in step 3. To open it from another device on the same
Wi-Fi, use `http://COMPUTER_IPV4:5173`; find the address using `ipconfig` and
allow Node/Vite through Windows Firewall only on the private network.

The local Vite server must remain running. `Ctrl+C` stops it. The dashboard is
local, but its login and data require internet access to Supabase.

## 9. Bootstrap the first physical admin badge

Enrollment normally requires an existing physical admin badge, so the first
one must be added manually:

1. Keep the ESP32 Serial Monitor open and scan the chosen admin card.
2. Copy the UID printed after `[rfid]`, without spaces or punctuation.
3. In the dashboard's **Add person** form, enter the person's details.
4. Set **Door role** to **Physical enrollment admin**.
5. Paste the UID into **Card UID** and save.
6. Scan the badge again. It should now receive a green grant and unlock.

The web administrator login and physical admin badge may represent the same
human, but they remain separate security records.

## 10. Enroll subsequent cards using the single reader

1. Add a person in the dashboard without entering a UID.
2. Click **Enroll card** for that person.
3. Wait up to 15 seconds for the ESP32's idle enrollment poll.
4. When prompted, scan the already registered physical admin badge.
5. Remove it fully from the reader.
6. Within 30 seconds, scan the new person's card.
7. The green LED/buzzer enrollment pattern lasts about three seconds and the
   dashboard updates through Realtime.

An unknown/member card cannot authorize enrollment. Internet access is
required for enrollment so that two devices cannot create conflicting records.

## 11. Routine management

- **Revoke** changes `people.access_enabled` to false. Online access is denied
  immediately on the next scan.
- **Grant** re-enables the person.
- **Edit** changes the name, class, division, year, role, notes, or UID.
- Clearing the UID while editing removes the assigned card.
- Replacing the UID moves the person to the new card.
- **Recent activity** shows grants, denials, exits, and enrollment events.

The ESP32 refreshes its offline allow-list every 60 seconds and accepts a
successfully synchronized allow-list for up to 24 hours during an outage.
Enrollment remains online-only. Revocations made while the ESP32 is already
offline cannot reach it until connectivity returns.

## 12. Quick fault interpretation

- Dashboard says account is not an administrator: the matching `profiles` row
  was not promoted to `admin`.
- Firmware receives HTTP 401 before useful JSON: redeploy the function with
  `--no-verify-jwt`, then verify device ID and secret.
- `DEVICE_AUTH_FAILED`: device ID and plaintext secret do not match the stored
  hash, or the device is disabled.
- Dashboard loads but is empty: verify the migration ran successfully and the
  logged-in profile is an admin.
- Enrollment does not start: confirm internet connectivity, device credentials,
  and that there is no older active enrollment request.
- Admin badge is rejected during enrollment: its `people.role` must be `admin`,
  its card must be active, and `access_enabled` must be true.

