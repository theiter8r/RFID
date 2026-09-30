-- Lets the enrollment admin-verification step be satisfied by the door's
-- existing "any card" override (see 004_any_card_override.sql) instead of a
-- real admin badge, so a fresh install can bootstrap its first admin card.
-- In that case enrollment_requests.admin_person_id has no person to record,
-- so the column must allow null. No-op if it is already nullable.

alter table public.enrollment_requests
  alter column admin_person_id drop not null;
