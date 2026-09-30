import { createClient } from 'npm:@supabase/supabase-js@2'

const corsHeaders = {
  'access-control-allow-origin': '*',
  'access-control-allow-headers': 'apikey, content-type, x-device-id, x-device-secret',
  'access-control-allow-methods': 'POST, OPTIONS',
}

type Json = Record<string, unknown>

function response(body: Json, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...corsHeaders, 'content-type': 'application/json; charset=utf-8' },
  })
}

function normalizeUid(value: unknown): string {
  return String(value ?? '').replace(/[^0-9a-f]/gi, '').toUpperCase()
}

async function sha256Hex(value: string): Promise<string> {
  const bytes = new TextEncoder().encode(value)
  const digest = await crypto.subtle.digest('SHA-256', bytes)
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('')
}

function constantTimeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false
  let difference = 0
  for (let i = 0; i < a.length; i += 1) difference |= a.charCodeAt(i) ^ b.charCodeAt(i)
  return difference === 0
}

function serviceKey(): string {
  const legacy = Deno.env.get('SUPABASE_SERVICE_ROLE_KEY')
  if (legacy) return legacy
  const raw = Deno.env.get('SUPABASE_SECRET_KEYS')
  if (!raw) throw new Error('Supabase server key is unavailable')
  const parsed = JSON.parse(raw)
  const key = parsed.default ?? Object.values(parsed)[0]
  if (typeof key !== 'string') throw new Error('Supabase server key is invalid')
  return key
}

const supabase = createClient(Deno.env.get('SUPABASE_URL')!, serviceKey(), {
  auth: { persistSession: false, autoRefreshToken: false },
})

Deno.serve(async (request) => {
  if (request.method === 'OPTIONS') return new Response('ok', { headers: corsHeaders })
  if (request.method !== 'POST') return response({ ok: false, error: 'METHOD_NOT_ALLOWED' }, 405)

  try {
    const deviceId = request.headers.get('x-device-id') ?? ''
    const deviceSecret = request.headers.get('x-device-secret') ?? ''
    if (!/^[0-9a-f-]{36}$/i.test(deviceId) || deviceSecret.length < 32) {
      return response({ ok: false, error: 'DEVICE_AUTH_REQUIRED' }, 401)
    }

    const { data: device, error: deviceError } = await supabase
      .from('devices')
      .select('id, door_id, secret_sha256, enabled, doors!inner(id, enabled, unlock_window_ms, held_open_alarm_ms, override_mode, override_unlock_window_ms, override_expires_at)')
      .eq('id', deviceId)
      .maybeSingle()

    const suppliedHash = await sha256Hex(deviceSecret)
    if (
      deviceError || !device || !device.enabled ||
      !constantTimeEqual(suppliedHash, device.secret_sha256)
    ) {
      return response({ ok: false, error: 'DEVICE_AUTH_FAILED' }, 401)
    }

    const door = Array.isArray(device.doors) ? device.doors[0] : device.doors
    if (!door?.enabled) return response({ ok: false, error: 'DOOR_DISABLED' }, 403)

    let overrideActive = false
    if (door.override_mode) {
      const expired = !door.override_expires_at || new Date(door.override_expires_at) <= new Date()
      if (expired) {
        await supabase.from('doors').update({ override_mode: false }).eq('id', door.id)
      } else {
        overrideActive = true
      }
    }
    const effectiveUnlockWindowMs = overrideActive ? door.override_unlock_window_ms : door.unlock_window_ms

    await supabase.from('devices').update({
      last_seen_at: new Date().toISOString(),
      firmware_version: request.headers.get('x-firmware-version'),
    }).eq('id', device.id)

    let body: Json
    try {
      body = await request.json()
    } catch {
      return response({ ok: false, error: 'INVALID_JSON' }, 400)
    }

    const action = String(body.action ?? '')

    if (action === 'authorize') {
      const uid = normalizeUid(body.uid)
      if (!/^[0-9A-F]{8,20}$/.test(uid)) {
        return response({ ok: false, granted: false, reason: 'INVALID_UID' }, 400)
      }

      const { data: card } = await supabase
        .from('cards')
        .select('uid, active, person_id')
        .eq('uid', uid)
        .maybeSingle()

      let person: { id: string; full_name: string; role: string; access_enabled: boolean } | null = null
      if (card?.person_id) {
        const result = await supabase
          .from('people')
          .select('id, full_name, role, access_enabled')
          .eq('id', card.person_id)
          .maybeSingle()
        person = result.data
      }

      const direction = body.direction === 'exit' ? 'exit' : 'entry'
      let granted = Boolean(card?.active && person?.access_enabled)
      let reason = !card ? 'UNKNOWN_CARD'
        : !card.active ? 'CARD_DISABLED'
        : !person ? 'PERSON_MISSING'
        : !person.access_enabled ? 'ACCESS_REVOKED'
        : 'ACCESS_GRANTED'
      if (!granted && overrideActive) {
        granted = true
        reason = 'ANY_CARD_OVERRIDE'
      }
      const decision = granted ? (direction === 'exit' ? 'exit' : 'granted') : 'denied'

      await supabase.from('access_events').insert({
        door_id: device.door_id,
        device_id: device.id,
        person_id: person?.id ?? null,
        uid,
        decision,
        reason,
        offline: false,
      })

      return response({
        ok: true,
        granted,
        reason,
        direction,
        person: person ? { id: person.id, name: person.full_name, role: person.role } : null,
        unlock_window_ms: effectiveUnlockWindowMs,
        server_time: new Date().toISOString(),
      })
    }

    if (action === 'sync') {
      const [{ data: rows, error: cardsError }, { data: revision }] = await Promise.all([
        supabase
          .from('cards')
          .select('uid, people!inner(id, full_name, role, access_enabled)')
          .eq('active', true)
          .eq('people.access_enabled', true),
        supabase.from('access_revision').select('value').eq('singleton', true).single(),
      ])
      if (cardsError) throw cardsError

      const cards = (rows ?? []).map((row) => {
        const person = Array.isArray(row.people) ? row.people[0] : row.people
        return {
          uid: row.uid,
          person_id: person.id,
          name: person.full_name,
          role: person.role,
        }
      })

      return response({
        ok: true,
        revision: revision?.value ?? 0,
        cards,
        server_time: new Date().toISOString(),
        unlock_window_ms: effectiveUnlockWindowMs,
        held_open_alarm_ms: door.held_open_alarm_ms,
      })
    }

    if (action === 'enrollment_status') {
      const now = new Date().toISOString()
      await supabase
        .from('enrollment_requests')
        .update({ status: 'expired', error_message: 'Request timed out' })
        .eq('door_id', device.door_id)
        .in('status', ['requested', 'awaiting_card'])
        .lte('expires_at', now)

      const { data: enrollment } = await supabase
        .from('enrollment_requests')
        .select('id, person_id, status, expires_at, people:people!enrollment_requests_person_id_fkey!inner(full_name)')
        .eq('door_id', device.door_id)
        .in('status', ['requested', 'awaiting_card'])
        .gt('expires_at', now)
        .order('created_at', { ascending: true })
        .limit(1)
        .maybeSingle()

      if (!enrollment) return response({ ok: true, active: false })
      const target = Array.isArray(enrollment.people) ? enrollment.people[0] : enrollment.people
      return response({
        ok: true,
        active: true,
        enrollment: {
          id: enrollment.id,
          person_id: enrollment.person_id,
          person_name: target.full_name,
          status: enrollment.status,
          expires_at: enrollment.expires_at,
        },
      })
    }

    if (action === 'remote_unlock_status') {
      const now = new Date().toISOString()
      await supabase
        .from('remote_unlock_requests')
        .update({ status: 'expired' })
        .eq('door_id', device.door_id)
        .eq('status', 'requested')
        .lte('expires_at', now)

      const { data: unlock } = await supabase
        .from('remote_unlock_requests')
        .select('id')
        .eq('door_id', device.door_id)
        .eq('status', 'requested')
        .gt('expires_at', now)
        .order('created_at', { ascending: true })
        .limit(1)
        .maybeSingle()

      if (!unlock) return response({ ok: true, active: false })

      const { error, count } = await supabase
        .from('remote_unlock_requests')
        .update({ status: 'completed', completed_at: now }, { count: 'exact' })
        .eq('id', unlock.id)
        .eq('status', 'requested')
      if (error) throw error
      if (!count) return response({ ok: true, active: false })

      await supabase.from('access_events').insert({
        door_id: device.door_id, device_id: device.id,
        decision: 'granted', reason: 'REMOTE_UNLOCK', offline: false,
      })

      return response({ ok: true, active: true })
    }

    if (action === 'verify_enrollment_admin') {
      const uid = normalizeUid(body.uid)
      const requestId = String(body.request_id ?? '')
      const { data: enrollment } = await supabase
        .from('enrollment_requests')
        .select('id, status, expires_at')
        .eq('id', requestId)
        .eq('door_id', device.door_id)
        .eq('status', 'requested')
        .gt('expires_at', new Date().toISOString())
        .maybeSingle()

      if (!enrollment) return response({ ok: true, verified: false, reason: 'ENROLLMENT_NOT_READY' })

      const { data: card } = await supabase
        .from('cards').select('person_id, active').eq('uid', uid).maybeSingle()
      const { data: person } = card?.person_id
        ? await supabase.from('people').select('id, full_name, role, access_enabled').eq('id', card.person_id).maybeSingle()
        : { data: null }

      const verified = Boolean(card?.active && person?.access_enabled && person?.role === 'admin')
      if (!verified) return response({ ok: true, verified: false, reason: 'ADMIN_BADGE_REQUIRED' })

      const expiresAt = new Date(Date.now() + 30_000).toISOString()
      const { error } = await supabase.from('enrollment_requests').update({
        status: 'awaiting_card', admin_person_id: person!.id, expires_at: expiresAt,
      }).eq('id', requestId).eq('status', 'requested')
      if (error) throw error

      return response({
        ok: true, verified: true, admin_name: person!.full_name, expires_at: expiresAt,
      })
    }

    if (action === 'complete_enrollment') {
      const uid = normalizeUid(body.uid)
      const requestId = String(body.request_id ?? '')
      const { data, error } = await supabase.rpc('device_complete_enrollment', {
        p_device_id: device.id,
        p_request_id: requestId,
        p_uid: uid,
      })
      if (error) {
        await supabase.from('access_events').insert({
          door_id: device.door_id, device_id: device.id, uid,
          decision: 'enrollment_failed', reason: error.message,
        })
        return response({ ok: true, enrolled: false, reason: error.message })
      }
      await supabase.from('access_events').insert({
        door_id: device.door_id, device_id: device.id,
        person_id: data.person_id, uid: data.uid,
        decision: 'enrollment_success', reason: 'ADMIN_VERIFIED_ENROLLMENT',
      })
      return response({ ok: true, enrolled: true, uid: data.uid, person_id: data.person_id })
    }

    if (action === 'log_batch') {
      const events = Array.isArray(body.events) ? body.events.slice(0, 50) : []
      const allowed = new Set([
        'granted', 'denied', 'exit', 'door_opened', 'door_closed',
        'door_held_open', 'enrollment_success', 'enrollment_failed',
      ])
      const rows = events
        .filter((event) => event && typeof event === 'object' && allowed.has(String(event.decision)))
        .map((event) => ({
          event_uuid: String(event.event_uuid),
          door_id: device.door_id,
          device_id: device.id,
          person_id: event.person_id || null,
          uid: event.uid ? normalizeUid(event.uid) : null,
          decision: event.decision,
          reason: String(event.reason ?? '').slice(0, 200),
          offline: Boolean(event.offline),
          device_time: event.device_time ? String(event.device_time).slice(0, 80) : null,
          metadata: typeof event.metadata === 'object' ? event.metadata : {},
        }))
      if (rows.length) {
        const { error } = await supabase.from('access_events').upsert(rows, {
          onConflict: 'event_uuid', ignoreDuplicates: true,
        })
        if (error) throw error
      }
      return response({ ok: true, accepted: rows.length })
    }

    return response({ ok: false, error: 'UNKNOWN_ACTION' }, 400)
  } catch (error) {
    console.error(error)
    return response({ ok: false, error: 'INTERNAL_ERROR' }, 500)
  }
})
