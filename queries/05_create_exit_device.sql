-- Run exactly once for the inside/exit NodeMCU reader. Copy the returned
-- secret immediately; only its hash is stored and it cannot be recovered
-- later. This registers a SECOND device under the same door as the existing
-- entry ESP32 (queries/03_create_device.sql) — devices.door_id has no unique
-- constraint, so both rows coexist and are distinguished by device_id.
with generated as (
  select gen_random_uuid() as id,
         encode(gen_random_bytes(32), 'hex') as device_secret
), inserted as (
  insert into public.devices (id, door_id, name, secret_sha256)
  select g.id,
         d.id,
         'Main Door - Inside Exit Reader (NodeMCU)',
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
