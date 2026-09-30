-- Replace both values before running.
update public.profiles as p
set role = 'admin',
    display_name = 'Administrator Name'
from auth.users as u
where p.id = u.id
  and lower(u.email) = lower('ADMIN_EMAIL@example.com');

-- This must return role = admin.
select u.email, p.display_name, p.role
from public.profiles p
join auth.users u on u.id = p.id
order by u.email;

