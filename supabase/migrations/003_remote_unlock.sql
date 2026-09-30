-- Remote unlock: an admin-triggered unlock the entry ESP32 picks up on its
-- existing cloud poll loop. Mirrors enrollment_requests' request/poll shape.

create type public.remote_unlock_status as enum ('requested', 'completed', 'expired');

create table public.remote_unlock_requests (
  id uuid primary key default gen_random_uuid(),
  door_id uuid not null references public.doors(id) on delete cascade,
  requested_by uuid not null references public.profiles(id),
  status public.remote_unlock_status not null default 'requested',
  expires_at timestamptz not null default (now() + interval '20 seconds'),
  created_at timestamptz not null default now(),
  completed_at timestamptz
);

create unique index one_active_remote_unlock_per_door
  on public.remote_unlock_requests (door_id)
  where status = 'requested';

alter table public.remote_unlock_requests enable row level security;

create policy "admins read remote unlocks" on public.remote_unlock_requests for select to authenticated
using (public.is_admin());
create policy "admins create remote unlocks" on public.remote_unlock_requests for insert to authenticated
with check (
  public.is_admin()
  and requested_by = auth.uid()
  and status = 'requested'
  and expires_at > now()
  and expires_at <= now() + interval '1 minute'
);

grant select, insert on public.remote_unlock_requests to authenticated;

alter publication supabase_realtime add table public.remote_unlock_requests;
