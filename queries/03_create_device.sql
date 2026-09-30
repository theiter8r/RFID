-- Run exactly once for one ESP32. Copy the returned secret immediately.
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

