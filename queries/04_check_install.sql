select
  (select count(*) from public.doors) as doors,
  (select count(*) from public.profiles) as dashboard_profiles,
  (select count(*) from public.devices) as esp32_devices,
  (select count(*) from public.people) as people,
  (select count(*) from public.cards) as cards;

select code, name, enabled, unlock_window_ms
from public.doors
order by code;

