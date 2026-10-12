-- Owner-only local camera discovery. No Wi-Fi password or image is uploaded.
begin;
create table park_private.camera_access (
  device_id uuid primary key references public.park_devices(id) on delete cascade,
  address inet not null,
  access_key text not null check (access_key ~ '^[a-f0-9]{32}$'),
  updated_at timestamptz not null default now(),
  check (family(address)=4 and masklen(address)=32 and
    (address <<= inet '10.0.0.0/8' or address <<= inet '172.16.0.0/12' or
     address <<= inet '192.168.0.0/16'))
);
alter table park_private.camera_access enable row level security;
revoke all on park_private.camera_access from public, anon, authenticated;

-- Preserve the already-tested ingestion/ACK/deduplication implementation.
alter function public.park_ingest(uuid,text,jsonb,jsonb) set schema park_private;
alter function park_private.park_ingest(uuid,text,jsonb,jsonb) rename to ingest_records;
revoke all on function park_private.ingest_records(uuid,text,jsonb,jsonb) from public,anon,authenticated;

create function public.park_ingest(p_device uuid,p_token text,p_records jsonb,p_status jsonb default '{}'::jsonb) returns jsonb
language plpgsql security definer set search_path='' as $$
declare result jsonb; camera jsonb; ip inet; secret text;
begin
  -- Authenticate before reading or changing private camera data. Any later
  -- validation failure rolls back records as well, so no unconfirmed ACK occurs.
  result:=park_private.ingest_records(p_device,p_token,p_records,p_status);
  camera:=p_status->'camera';
  if camera is not null and camera<>'null'::jsonb then
    if jsonb_typeof(camera)<>'object' or jsonb_typeof(camera->'address') is distinct from 'string'
       or jsonb_typeof(camera->'access_key') is distinct from 'string'
       or length(camera->>'address')>15 then
      raise exception 'Endereço local inválido';
    end if;
    begin ip:=(camera->>'address')::inet;
    exception when invalid_text_representation then raise exception 'Endereço local inválido'; end;
    secret:=camera->>'access_key';
    if family(ip)<>4 or masklen(ip)<>32 or not
      (ip <<= inet '10.0.0.0/8' or ip <<= inet '172.16.0.0/12' or ip <<= inet '192.168.0.0/16')
      or secret !~ '^[a-f0-9]{32}$' then raise exception 'Endereço local inválido'; end if;
    insert into park_private.camera_access(device_id,address,access_key,updated_at)
      values(p_device,ip,secret,now())
      on conflict(device_id) do update set address=excluded.address,
        access_key=excluded.access_key,updated_at=excluded.updated_at;
  end if;
  return result;
end $$;
revoke all on function public.park_ingest(uuid,text,jsonb,jsonb) from public,anon,authenticated;
grant execute on function public.park_ingest(uuid,text,jsonb,jsonb) to anon,authenticated;

create function public.park_camera_access(p_device uuid,p_include_key boolean default false) returns jsonb
language plpgsql stable security definer set search_path='' as $$
declare camera park_private.camera_access; fresh boolean;
begin
  if not exists(select 1 from public.park_devices d join public.park_members m on m.park_id=d.park_id
    where d.id=p_device and d.enabled and m.user_id=auth.uid() and m.role='owner') then
    raise exception 'Sem permissão' using errcode='42501';
  end if;
  select * into camera from park_private.camera_access where device_id=p_device;
  if not found then return jsonb_build_object('available',false,'fresh',false); end if;
  fresh:=camera.updated_at>=now()-interval '2 minutes';
  return jsonb_build_object('available',true,'fresh',fresh,'address',host(camera.address),'updated_at',camera.updated_at)
    || case when p_include_key is true and fresh then
      jsonb_build_object('url','http://'||host(camera.address)||'/#'||camera.access_key)
      else '{}'::jsonb end;
end $$;
revoke all on function public.park_camera_access(uuid,boolean) from public,anon,authenticated;
grant execute on function public.park_camera_access(uuid,boolean) to authenticated;
notify pgrst,'reload schema';
commit;
