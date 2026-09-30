-- "Any card" override: while active, every tapped UID unlocks the door
-- (skipping the normal cards/people check) and the unlock holds for
-- override_unlock_window_ms instead of the door's normal unlock_window_ms.
-- Auto-expires via override_expires_at so a forgotten toggle can't leave the
-- access list bypassed indefinitely; the admin can still turn it off early.

alter table public.doors
  add column override_mode boolean not null default false,
  add column override_unlock_window_ms integer not null default 60000
    check (override_unlock_window_ms between 5000 and 120000),
  add column override_expires_at timestamptz;

alter publication supabase_realtime add table public.doors;
