-- Incremental health, planned sleep and owner-only discovery without a 2 min TTL.
-- No history is deleted and the ingestion/record identity contract is preserved.
begin;
alter table public.park_devices add column operation jsonb not null default '{}'::jsonb;

create or replace function park_private.ingest_records(p_device uuid,p_token text,p_records jsonb,p_status jsonb default '{}'::jsonb) returns jsonb
language plpgsql security definer set search_path='' as $$
declare item jsonb; inserted_count integer:=0; affected integer; stamp numeric; op jsonb;
begin
  if length(p_token)<>64 or not exists(
    select 1 from park_private.device_credentials c join public.park_devices d on d.id=c.device_id
    where c.device_id=p_device and d.enabled and c.token_hash=extensions.digest(p_token,'sha256')
  ) then raise exception 'Dispositivo não autorizado' using errcode='42501'; end if;
  if jsonb_typeof(p_records) is distinct from 'array' or jsonb_array_length(p_records)>32 then raise exception 'Lote inválido'; end if;
  if jsonb_typeof(p_status) is distinct from 'object' then raise exception 'Lote inválido'; end if;
  if (p_status ? 'storage_ok' and jsonb_typeof(p_status->'storage_ok') is distinct from 'boolean') or
     (p_status ? 'clock_ok' and jsonb_typeof(p_status->'clock_ok') is distinct from 'boolean') then raise exception 'Lote inválido'; end if;
  op:=p_status->'operation';
  if op is not null and (
    jsonb_typeof(op) is distinct from 'object' or octet_length(op::text)>512 or
    op->>'state' is null or op->>'state' not in ('awake','sleep_planned') or
    jsonb_typeof(op->'sleep_enabled') is distinct from 'boolean' or
    jsonb_typeof(op->'camera_ok') is distinct from 'boolean' or
    jsonb_typeof(op->'sleep_seconds') is distinct from 'number'
  ) then raise exception 'Lote inválido'; end if;
  if op is not null then
    if (op->>'sleep_seconds')::numeric not between 60 and 86400 then raise exception 'Lote inválido'; end if;
    if op ? 'expected_wake_epoch' then
      stamp:=(op->>'expected_wake_epoch')::numeric;
      if stamp is null or stamp<1704067200 or stamp>extract(epoch from now())+172800 then raise exception 'Horário inválido'; end if;
    end if;
  end if;
  for item in select * from jsonb_array_elements(p_records) loop
    stamp:=(item->>'epoch')::numeric;
    if stamp is null or stamp<>trunc(stamp) or stamp<1704067200 or stamp>4102444799 or stamp>extract(epoch from now())+300 then raise exception 'Horário inválido'; end if;
    if item->>'entries' is null or item->>'exits' is null or item->>'entries' !~ '^[0-9]{1,20}$' or item->>'exits' !~ '^[0-9]{1,20}$' then raise exception 'Contagem inválida'; end if;
    insert into public.visit_records(device_id,record_id,recorded_at,entries,exits,kind)
      values(p_device,item->>'id',to_timestamp(stamp::double precision),(item->>'entries')::numeric,(item->>'exits')::numeric,item->>'kind')
      on conflict(device_id,record_id) do nothing;
    get diagnostics affected = row_count;
    inserted_count:=inserted_count+affected;
  end loop;
  -- Absent fields retain their last confirmed value, rather than becoming NULL.
  update public.park_devices set last_sync=now(),
    storage_ok=case when p_status ? 'storage_ok' then (p_status->>'storage_ok')::boolean else storage_ok end,
    clock_ok=case when p_status ? 'clock_ok' then (p_status->>'clock_ok')::boolean else clock_ok end,
    operation=case when op is not null then op else operation end
    where id=p_device;
  return jsonb_build_object('accepted',jsonb_array_length(p_records),'inserted',inserted_count);
end $$;
revoke all on function park_private.ingest_records(uuid,text,jsonb,jsonb) from public,anon,authenticated;

create or replace function public.park_camera_access(p_device uuid,p_include_key boolean default false) returns jsonb
language plpgsql stable security definer set search_path='' as $$
declare camera park_private.camera_access;
begin
  if not exists(select 1 from public.park_devices d join public.park_members m on m.park_id=d.park_id
    where d.id=p_device and d.enabled and m.user_id=auth.uid() and m.role='owner') then
    raise exception 'Sem permissão' using errcode='42501';
  end if;
  select * into camera from park_private.camera_access where device_id=p_device;
  if not found then return jsonb_build_object('available',false,'fresh',false); end if;
  -- A cached LAN address is useful even while the device sleeps. It does not
  -- prove current reachability; the UI discloses its timestamp and uncertainty.
  return jsonb_build_object('available',true,'fresh',camera.updated_at>=now()-interval '2 minutes',
    'address',host(camera.address),'updated_at',camera.updated_at)
    || case when p_include_key is true then jsonb_build_object('url','http://'||host(camera.address)||'/#'||camera.access_key)
       else '{}'::jsonb end;
end $$;
revoke all on function public.park_camera_access(uuid,boolean) from public,anon,authenticated;
grant execute on function public.park_camera_access(uuid,boolean) to authenticated;
notify pgrst,'reload schema';
commit;
