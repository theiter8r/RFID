import { createClient, type RealtimeChannel, type Session } from '@supabase/supabase-js'
import './style.css'

const url = import.meta.env.VITE_SUPABASE_URL as string | undefined
const key = import.meta.env.VITE_SUPABASE_PUBLISHABLE_KEY as string | undefined
const app = document.querySelector<HTMLDivElement>('#app')!

if (!url || !key) {
  app.innerHTML = `<main class="login"><div class="card"><h1>Configuration required</h1>
    <p>Copy <span class="uid">.env.example</span> to <span class="uid">.env</span>, add the Supabase URL and publishable/anon key, then restart Vite.</p></div></main>`
  throw new Error('Missing Supabase environment variables')
}

const supabase = createClient(url, key)
let session: Session | null = null
let realtime: RealtimeChannel | null = null
let editingId: string | null = null

type CardRow = { uid: string; active: boolean }
type Person = {
  id: string
  full_name: string
  class_name: string
  division: string
  academic_year: string
  role: 'member' | 'admin'
  access_enabled: boolean
  notes: string
  cards: CardRow[] | CardRow | null
}
type Door = {
  id: string; name: string; code: string
  override_mode: boolean; override_expires_at: string | null
}
type Enrollment = {
  id: string; status: string; expires_at: string; person_id: string
  people: { full_name: string } | { full_name: string }[]
}
type AccessEvent = {
  id: number; created_at: string; decision: string; reason: string; uid: string | null; offline: boolean
  people: { full_name: string } | { full_name: string }[] | null
  devices: { name: string } | { name: string }[] | null
}

const esc = (value: unknown) => String(value ?? '')
  .replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;')
  .replaceAll('"', '&quot;').replaceAll("'", '&#039;')

const ICONS = {
  edit: '<path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M18.5 2.5a2.12 2.12 0 0 1 3 3L12 15l-4 1 1-4Z"/>',
  grant: '<path d="M20 6 9 17l-5-5"/>',
  revoke: '<circle cx="12" cy="12" r="10"/><path d="m15 9-6 6"/><path d="m9 9 6 6"/>',
  card: '<rect x="2" y="5" width="20" height="14" rx="2"/><path d="M2 10h20"/><path d="M6 15h4"/>',
  trash: '<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>',
  x: '<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
  plus: '<path d="M5 12h14"/><path d="M12 5v14"/>',
  unlock: '<rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/>',
  logout: '<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="M16 17l5-5-5-5"/><path d="M21 12H9"/>',
  bolt: '<path d="M13 2 3 14h9l-1 8 10-12h-9l1-8Z"/>',
} as const

function icon(name: keyof typeof ICONS): string {
  return `<svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name]}</svg>`
}

function personCard(person: Person | null | undefined): CardRow | null {
  if (!person?.cards) return null
  return Array.isArray(person.cards) ? person.cards[0] ?? null : person.cards
}

function notice(message: string, error = false) {
  const host = document.querySelector<HTMLDivElement>('#notice')
  if (!host) return
  host.className = `notice${error ? ' error' : ''}`
  host.textContent = message
  host.hidden = false
  window.setTimeout(() => { host.hidden = true }, 5000)
}

function renderLogin(message = '') {
  app.innerHTML = `<main class="login"><div class="card">
    <div class="brand"><div class="brand-mark">RF</div><div><h1>Door Access Admin</h1><div class="muted">Authorized administrators only</div></div></div>
    ${message ? `<p class="notice error">${esc(message)}</p>` : ''}
    <form id="login-form" class="fields">
      <label>Email<input name="email" type="email" autocomplete="username" required></label>
      <label>Password<input name="password" type="password" autocomplete="current-password" required></label>
      <button class="button" type="submit">Sign in</button>
    </form>
  </div></main>`
  document.querySelector<HTMLFormElement>('#login-form')!.addEventListener('submit', async (event) => {
    event.preventDefault()
    const data = new FormData(event.currentTarget as HTMLFormElement)
    const { error } = await supabase.auth.signInWithPassword({
      email: String(data.get('email')), password: String(data.get('password')),
    })
    if (error) renderLogin(error.message)
  })
}

async function loadDashboard() {
  if (!session) return renderLogin()
  const profileResult = await supabase.from('profiles').select('display_name, role').eq('id', session.user.id).single()
  if (profileResult.error || profileResult.data?.role !== 'admin') {
    await supabase.auth.signOut()
    return renderLogin('This account is not an administrator.')
  }

  const [peopleResult, doorsResult, enrollResult, eventsResult] = await Promise.all([
    supabase.from('people').select('*, cards:cards!cards_person_id_fkey(uid, active)').order('full_name'),
    supabase.from('doors').select('id, name, code, override_mode, override_expires_at').eq('enabled', true).order('name'),
    supabase.from('enrollment_requests')
      .select('id, status, expires_at, person_id, people:people!enrollment_requests_person_id_fkey(full_name)')
      .in('status', ['requested', 'awaiting_card']).order('created_at').limit(1).maybeSingle(),
    supabase.from('access_events').select('id, created_at, decision, reason, uid, offline, people(full_name), devices(name)')
      .order('created_at', { ascending: false }).limit(30),
  ])
  const firstError = peopleResult.error || doorsResult.error || enrollResult.error || eventsResult.error
  if (firstError) return renderShellError(firstError.message)
  renderDashboard(
    (peopleResult.data ?? []) as Person[], (doorsResult.data ?? []) as Door[],
    enrollResult.data as Enrollment | null, (eventsResult.data ?? []) as AccessEvent[],
    profileResult.data.display_name || session.user.email || 'Administrator',
  )
  subscribeRealtime()
}

function personName(row: Enrollment | AccessEvent): string {
  const relation = row.people
  if (!relation) return 'Unknown'
  return Array.isArray(relation) ? relation[0]?.full_name ?? 'Unknown' : relation.full_name
}

function deviceName(row: AccessEvent): string {
  const relation = row.devices
  if (!relation) return '—'
  return Array.isArray(relation) ? relation[0]?.name ?? '—' : relation.name
}

function renderShellError(message: string) {
  app.innerHTML = `<main class="login"><div class="card"><h1>Could not load dashboard</h1>
    <p class="notice error">${esc(message)}</p><button id="logout" class="button secondary">Sign out</button></div></main>`
  document.querySelector('#logout')?.addEventListener('click', () => supabase.auth.signOut())
}

function renderDashboard(people: Person[], doors: Door[], enrollment: Enrollment | null, events: AccessEvent[], adminName: string) {
  const enabledCount = people.filter((person) => person.access_enabled).length
  const cardCount = people.filter((person) => personCard(person)).length
  const edit = people.find((person) => person.id === editingId)
  const formPerson = edit ?? { full_name: '', class_name: '', division: '', academic_year: '', role: 'member', notes: '' }

  const door = doors[0]
  const overrideActive = Boolean(door?.override_mode && door.override_expires_at && new Date(door.override_expires_at) > new Date())

  const enrollmentHtml = enrollment ? `<div class="card enrollment">
    <h2>Enrollment in progress</h2>
    <p><strong>${esc(personName(enrollment))}</strong> — <span class="badge wait">${esc(enrollment.status.replace('_', ' '))}</span></p>
    <p class="muted">${enrollment.status === 'requested' ? 'Scan an enabled admin badge at the door.' : 'Admin verified. Scan the new badge at the same reader.'}</p>
    <div class="actions"><button class="button danger" id="cancel-enrollment" data-id="${enrollment.id}">${icon('x')}Cancel enrollment</button>
    <span class="muted">Expires ${new Date(enrollment.expires_at).toLocaleTimeString()}</span></div>
  </div>` : ''

  const overrideHtml = overrideActive ? `<div class="card override-banner">
    <h2>Any-card mode is ON <span class="badge off">ON</span></h2>
    <p class="muted">Every tapped card unlocks the door for about a minute — no authorization check. Auto-expires ${new Date(door!.override_expires_at!).toLocaleTimeString()} if left on.</p>
    <div class="actions"><button class="button danger" id="toggle-override">${icon('x')}Turn off</button></div>
  </div>` : `<div class="card">
    <h2>Any-card mode</h2>
    <p class="muted">Normally only registered, access-enabled cards work. Turn this on to let any tapped card unlock the door for about a minute — useful for an event. Auto-reverts after 2 hours if you forget.</p>
    <div class="actions"><button class="button secondary" id="toggle-override" ${door ? '' : 'disabled'}>${icon('bolt')}Turn on</button></div>
  </div>`

  const rows = people.map((person) => {
    const card = personCard(person)
    return `<tr>
      <td><strong>${esc(person.full_name)}</strong><div class="muted">${esc(person.class_name)} ${esc(person.division)} · ${esc(person.academic_year)}</div></td>
      <td><span class="badge ${person.role === 'admin' ? 'wait' : 'ok'}">${esc(person.role)}</span></td>
      <td class="uid">${card ? esc(card.uid) : '<span class="muted">No card</span>'}</td>
      <td><span class="badge ${person.access_enabled ? 'ok' : 'off'}">${person.access_enabled ? 'Granted' : 'Revoked'}</span></td>
      <td><div class="actions">
        <button class="button secondary edit-person" data-id="${person.id}">${icon('edit')}Edit</button>
        <button class="button secondary toggle-access" data-id="${person.id}" data-next="${!person.access_enabled}">${icon(person.access_enabled ? 'revoke' : 'grant')}${person.access_enabled ? 'Revoke' : 'Grant'}</button>
        <button class="button enroll-person" data-id="${person.id}" ${enrollment || !doors.length ? 'disabled' : ''}>${icon('card')}Enroll card</button>
        <button class="button danger delete-person" data-id="${person.id}" ${person.access_enabled ? 'disabled title="Revoke access before deleting"' : ''}>${icon('trash')}Delete</button>
      </div></td>
    </tr>`
  }).join('')

  const eventRows = events.map((event) => `<tr>
    <td>${new Date(event.created_at).toLocaleString()}</td>
    <td>${esc(personName(event))}</td>
    <td><span class="badge ${event.decision.includes('denied') || event.decision.includes('failed') ? 'off' : 'ok'}">${esc(event.decision)}</span></td>
    <td>${esc(event.reason)}${event.offline ? ' · offline' : ''}</td>
    <td>${esc(deviceName(event))}</td>
  </tr>`).join('')

  app.innerHTML = `<main class="shell">
    <header class="topbar"><div class="brand"><div class="brand-mark">RF</div><div><h1>Door Access Admin</h1><div class="muted">Single-reader controller</div></div></div>
      <div class="actions"><span class="muted">${esc(adminName)}</span><button id="remote-unlock" class="button" ${doors.length ? '' : 'disabled'}>${icon('unlock')}Open door</button><button id="logout" class="button secondary">${icon('logout')}Sign out</button></div></header>
    <div id="notice" class="notice" hidden></div>
    <div class="summary"><div class="stat"><strong>${people.length}</strong><span class="muted">Registered people</span></div>
      <div class="stat"><strong>${enabledCount}</strong><span class="muted">Access enabled</span></div>
      <div class="stat"><strong>${cardCount}</strong><span class="muted">Cards assigned</span></div></div>
    ${enrollmentHtml}
    ${overrideHtml}
    <div class="grid">
      <section class="card"><h2>${edit ? 'Edit person' : 'Add person'}</h2>
        <form id="person-form" class="fields">
          <label>Full name<input name="full_name" maxlength="120" value="${esc(formPerson.full_name)}" required></label>
          <label>Class<input name="class_name" maxlength="60" value="${esc(formPerson.class_name)}"></label>
          <label>Division<input name="division" maxlength="60" value="${esc(formPerson.division)}"></label>
          <label>Academic year<input name="academic_year" maxlength="30" value="${esc(formPerson.academic_year)}" placeholder="2026–27"></label>
          <label>Door role<select name="role"><option value="member" ${formPerson.role === 'member' ? 'selected' : ''}>Member</option><option value="admin" ${formPerson.role === 'admin' ? 'selected' : ''}>Physical enrollment admin</option></select></label>
          <label>Card UID (optional manual entry)<input class="uid" name="uid" value="${esc(personCard(edit)?.uid ?? '')}" placeholder="04A1B2C3"></label>
          <label>Notes<textarea name="notes" rows="3" maxlength="500">${esc(formPerson.notes)}</textarea></label>
          <div class="actions"><button class="button" type="submit">${icon(edit ? 'grant' : 'plus')}${edit ? 'Save changes' : 'Add person'}</button>${edit ? `<button class="button secondary" type="button" id="cancel-edit">${icon('x')}Cancel</button>` : ''}</div>
        </form>
      </section>
      <div class="stack">
        <section class="card"><h2>People and cards</h2><div class="table-wrap"><table><thead><tr><th>Person</th><th>Role</th><th>UID</th><th>Access</th><th>Actions</th></tr></thead>
          <tbody>${rows || '<tr><td colspan="5" class="empty">Add the first person to begin.</td></tr>'}</tbody></table></div></section>
        <section class="card"><h2>Recent activity</h2><div class="table-wrap scroll"><table><thead><tr><th>Time</th><th>Person</th><th>Event</th><th>Reason</th><th>Device</th></tr></thead>
          <tbody>${eventRows || '<tr><td colspan="5" class="empty">No access events yet.</td></tr>'}</tbody></table></div></section>
      </div>
    </div>
  </main>`

  document.querySelector('#logout')!.addEventListener('click', () => supabase.auth.signOut())
  document.querySelector<HTMLButtonElement>('#remote-unlock')?.addEventListener('click', async (event) => {
    if (!session || !doors[0]) return
    const button = event.currentTarget as HTMLButtonElement
    button.disabled = true
    button.textContent = 'Opening…'
    const { error } = await supabase.from('remote_unlock_requests').insert({
      door_id: doors[0].id, requested_by: session.user.id,
      expires_at: new Date(Date.now() + 20_000).toISOString(),
    })
    if (error) {
      notice(error.message, true)
      button.disabled = false
      button.textContent = 'Open door'
      return
    }
    notice('Unlock sent — door will open within a few seconds.')
    window.setTimeout(() => { button.disabled = false; button.textContent = 'Open door' }, 10_000)
  })
  document.querySelector<HTMLButtonElement>('#toggle-override')?.addEventListener('click', async () => {
    if (!session || !door) return
    const next = !overrideActive
    const { error } = await supabase.from('doors').update({
      override_mode: next,
      override_expires_at: next ? new Date(Date.now() + 2 * 60 * 60 * 1000).toISOString() : null,
    }).eq('id', door.id)
    if (error) return notice(error.message, true)
    notice(next ? 'Any-card mode is on.' : 'Any-card mode is off.')
    loadDashboard()
  })
  document.querySelector('#cancel-edit')?.addEventListener('click', () => { editingId = null; loadDashboard() })
  document.querySelectorAll<HTMLButtonElement>('.edit-person').forEach((button) => button.addEventListener('click', () => {
    editingId = button.dataset.id!; loadDashboard()
  }))
  document.querySelectorAll<HTMLButtonElement>('.toggle-access').forEach((button) => button.addEventListener('click', async () => {
    const { error } = await supabase.from('people').update({ access_enabled: button.dataset.next === 'true' }).eq('id', button.dataset.id!)
    error ? notice(error.message, true) : loadDashboard()
  }))
  document.querySelectorAll<HTMLButtonElement>('.delete-person').forEach((button) => button.addEventListener('click', async () => {
    const person = people.find((row) => row.id === button.dataset.id)
    if (!person || person.access_enabled) return notice('Revoke access before deleting this person.', true)
    if (!window.confirm(`Permanently delete ${person.full_name} and their assigned card?`)) return
    const { error } = await supabase.from('people').delete().eq('id', person.id)
    error ? notice(error.message, true) : loadDashboard()
  }))
  document.querySelectorAll<HTMLButtonElement>('.enroll-person').forEach((button) => button.addEventListener('click', async () => {
    if (!session || !doors[0]) return
    const { error } = await supabase.from('enrollment_requests').insert({
      person_id: button.dataset.id!, door_id: doors[0].id, requested_by: session.user.id,
      expires_at: new Date(Date.now() + 120_000).toISOString(),
    })
    error ? notice(error.message, true) : loadDashboard()
  }))
  document.querySelector<HTMLButtonElement>('#cancel-enrollment')?.addEventListener('click', async (event) => {
    const id = (event.currentTarget as HTMLButtonElement).dataset.id!
    const { error } = await supabase.from('enrollment_requests').update({ status: 'cancelled' }).eq('id', id)
    error ? notice(error.message, true) : loadDashboard()
  })

  document.querySelector<HTMLFormElement>('#person-form')!.addEventListener('submit', async (event) => {
    event.preventDefault()
    if (!session) return
    const form = new FormData(event.currentTarget as HTMLFormElement)
    const personData = {
      full_name: String(form.get('full_name')).trim(), class_name: String(form.get('class_name')).trim(),
      division: String(form.get('division')).trim(), academic_year: String(form.get('academic_year')).trim(),
      role: String(form.get('role')), notes: String(form.get('notes')).trim(),
    }
    let personId = editingId
    const personResult = editingId
      ? await supabase.from('people').update(personData).eq('id', editingId).select('id').single()
      : await supabase.from('people').insert({ ...personData, created_by: session.user.id }).select('id').single()
    if (personResult.error) return notice(personResult.error.message, true)
    personId = personResult.data.id

    const uid = String(form.get('uid')).replace(/[^0-9a-f]/gi, '').toUpperCase()
    const oldUid = personCard(edit)?.uid
    if (uid && !/^[0-9A-F]{8,20}$/.test(uid)) return notice('UID must contain 8–20 hexadecimal characters.', true)
    if (oldUid && !uid) {
      const { error } = await supabase.from('cards').delete().eq('uid', oldUid)
      if (error) return notice(error.message, true)
    } else if (oldUid && uid !== oldUid) {
      const { error } = await supabase.from('cards').update({ uid }).eq('uid', oldUid)
      if (error) return notice(`Person saved, but UID failed: ${error.message}`, true)
    } else if (!oldUid && uid) {
      const { error } = await supabase.from('cards').insert({ uid, person_id: personId, active: true, created_by: session.user.id })
      if (error) return notice(`Person saved, but UID failed: ${error.message}`, true)
    }
    editingId = null
    await loadDashboard()
    notice(edit ? 'Person updated.' : 'Person added.')
  })
}

function subscribeRealtime() {
  if (realtime) return
  realtime = supabase.channel('admin-live')
    .on('postgres_changes', { event: '*', schema: 'public', table: 'enrollment_requests' }, () => loadDashboard())
    .on('postgres_changes', { event: '*', schema: 'public', table: 'access_events' }, () => loadDashboard())
    .on('postgres_changes', { event: '*', schema: 'public', table: 'people' }, () => loadDashboard())
    .on('postgres_changes', { event: '*', schema: 'public', table: 'cards' }, () => loadDashboard())
    .on('postgres_changes', { event: '*', schema: 'public', table: 'doors' }, () => loadDashboard())
    .subscribe()
}

supabase.auth.onAuthStateChange((_event, nextSession) => {
  session = nextSession
  if (!session) {
    if (realtime) { supabase.removeChannel(realtime); realtime = null }
    editingId = null
    renderLogin()
  } else {
    window.setTimeout(loadDashboard, 0)
  }
})

supabase.auth.getSession().then(({ data }) => {
  session = data.session
  session ? loadDashboard() : renderLogin()
})
