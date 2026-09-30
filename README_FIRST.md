# RFID Door System — Portable Installation

This bundle contains no personal Supabase credentials, no `.env`, no ESP32
device secret, and no registered-person data. It creates a completely new
installation owned by the new organization/account.

## What is included

- `web/`: administrator dashboard source
- `supabase/migrations/001_initial_schema.sql`: complete empty database
- `supabase/functions/device-api/index.ts`: ESP32 API
- `firmware/`: ESP32 firmware and credential template
- `queries/`: one-time administrator/device setup queries

## Before starting

Install Node.js LTS and PlatformIO on the new computer. Create/sign into the
Supabase account that should permanently own the system. Do not use a personal
account if the installation belongs to an institution.

## Step 1 — Create the new Supabase project

1. Open `https://supabase.com/dashboard`.
2. Create a new project under the correct organization.
3. Choose the free plan, a nearby region, and a strong database password.
4. Wait until the project is ready.
5. Copy the project reference. In
   `https://abcdefgh.supabase.co`, it is `abcdefgh`.

## Step 2 — Create all database tables

1. Open Supabase **SQL Editor** and create a new query.
2. Open `supabase/migrations/001_initial_schema.sql` from this bundle.
3. Copy the entire file into SQL Editor and press **Run once**.
4. Open `queries/04_check_install.sql`, run it, and confirm that it reports one
   door and all expected tables.

Do not run the initial migration repeatedly. The database already includes RLS
policies, Realtime tables, cascading card deletion, event history, and the
single-reader enrollment workflow.

## Step 3 — Create the dashboard administrator

1. In Supabase open **Authentication → Users → Add user**.
2. Create the administrator email/password and auto-confirm it if offered.
3. Open `queries/02_promote_web_admin.sql`.
4. Replace `ADMIN_EMAIL@example.com` and the display name.
5. Run it in SQL Editor. Its result must show role `admin`.

This is the web login. A physical enrollment-admin RFID badge is created later.

## Step 4 — Create the ESP32 identity

1. Run `queries/03_create_device.sql` exactly once in SQL Editor.
2. Save the returned `device_id` and `device_secret` immediately.
3. The database stores only the secret hash; the plaintext secret cannot be
   recovered later.

## Step 5 — Copy the Supabase URL and publishable key

In Supabase open **Connect** or **Settings → API Keys**. Copy:

- Project URL: `https://PROJECT_REF.supabase.co`
- Publishable key: normally starts with `sb_publishable_`

Never put a secret/service-role key in the web dashboard or ESP32.

## Step 6 — Deploy the ESP32 Edge Function

Open PowerShell in the extracted bundle root:

```powershell
npx.cmd supabase@latest init
npx.cmd supabase@latest login
npx.cmd supabase@latest link --project-ref YOUR_PROJECT_REF
npx.cmd supabase@latest functions deploy device-api --no-verify-jwt
```

If `init` says it is already initialized, continue. `--no-verify-jwt` is
required because the ESP32 uses its own UUID and 64-character secret; the
function validates both before accessing data.

The device endpoint becomes:

```text
https://YOUR_PROJECT_REF.supabase.co/functions/v1/device-api
```

## Step 7 — Configure the dashboard

```powershell
cd web
Copy-Item -LiteralPath ".env.example" -Destination ".env"
notepad .env
```

Enter only the new project's public values:

```dotenv
VITE_SUPABASE_URL=https://YOUR_PROJECT_REF.supabase.co
VITE_SUPABASE_PUBLISHABLE_KEY=sb_publishable_YOUR_KEY
```

Install once and start locally:

```powershell
npm.cmd ci
npm.cmd run dev -- --host 0.0.0.0
```

Open `http://localhost:5173`. Leave that terminal running. For every later
restart, only the `npm.cmd run dev -- --host 0.0.0.0` command is required.

## Step 8 — Deploy the dashboard permanently (optional)

The dashboard is a static site. One simple free-hosting route is Netlify:

```powershell
cd web
npm.cmd ci
npm.cmd run build
npx.cmd netlify-cli login
npx.cmd netlify-cli deploy --prod --dir=dist
```

Choose **Create and configure a new site** when prompted. Save the final HTTPS
URL. The `.env` values are compiled into the build; the publishable key is
intended for browser use, while RLS and administrator login protect the data.

If another static host is preferred, upload the generated `web/dist` folder.
Rebuild and redeploy whenever dashboard source or Supabase public values change.

## Step 9 — Configure and flash the ESP32

Copy `firmware/include/secrets.example.h` to
`firmware/include/secrets.h`, then fill:

```cpp
#define WIFI_SSID "YOUR_WIFI"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define DEVICE_API_URL "https://YOUR_PROJECT_REF.supabase.co/functions/v1/device-api"
#define SUPABASE_PUBLISHABLE_KEY "sb_publishable_YOUR_KEY"
#define DEVICE_ID "DEVICE_UUID_FROM_STEP_4"
#define DEVICE_SECRET "DEVICE_SECRET_FROM_STEP_4"
```

For isolated commissioning only, use `ALLOW_INSECURE_TLS = true`. Install the
correct CA and return it to false before permanent operation.

```powershell
cd firmware
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --baud 115200
```

## Step 10 — Create the first physical enrollment administrator

1. Scan the chosen first-admin badge while Serial Monitor is open.
2. Copy the value printed after `[rfid]`.
3. Sign into the dashboard.
4. Add the person, select **Physical enrollment admin**, paste the UID, save.
5. Scan it again; it should grant access and show green.

This one manual bootstrap is necessary. It does not require a second reader.

## Step 11 — Enroll normal people

1. Add a person with Card UID blank.
2. Click **Enroll card**.
3. Wait for the ESP32/dashboard enrollment prompt.
4. Scan the physical admin badge.
5. Remove it completely.
6. Within 30 seconds scan the new card.
7. The new UID appears in the dashboard through Realtime.

## Normal management

- **Revoke** blocks access.
- **Grant** restores access.
- **Edit** changes details or UID.
- **Delete** is enabled only after revocation and permanently removes the person
  and assigned card. Historical access events remain but the deleted person
  reference becomes empty.

