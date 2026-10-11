begin;
create function public.park_rotate_device(p_device uuid) returns jsonb
language plpgsql security definer set search_path='' as $$
declare device_token text;
begin
  if not exists(select 1 from public.park_devices d join public.park_members m on m.park_id=d.park_id
    where d.id=p_device and d.enabled and m.user_id=auth.uid() and m.role='owner') then
    raise exception 'Sem permissão' using errcode='42501';
  end if;
  device_token:=encode(extensions.gen_random_bytes(32),'hex');
  update park_private.device_credentials set token_hash=extensions.digest(device_token,'sha256') where device_id=p_device;
  return jsonb_build_object('device_id',p_device,'device_token',device_token);
end $$;
revoke all on function public.park_rotate_device(uuid) from public,anon;
grant execute on function public.park_rotate_device(uuid) to authenticated;
commit;
