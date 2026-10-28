-- Final application schema for a NEW Supabase project.
-- Run once as the database owner, after Supabase has created auth.users and auth.uid().
-- Do not run against the existing project: this file is not an upgrade or a data backup.
-- Users, park records and device credentials are created separately; none are included here.
begin;

-- Refuse an existing application database before creating or changing any object.
do $$
begin
  if to_regclass('public.parks') is not null
     or to_regclass('public.park_members') is not null
     or to_regclass('public.park_devices') is not null
     or to_regclass('public.visit_records') is not null
     or to_regclass('park_private.device_credentials') is not null
     or to_regclass('park_private.camera_access') is not null then
    raise exception 'Esquema do parque já existente. Use este arquivo somente em um projeto Supabase novo.';
  end if;
end $$;

-- Extensions and private objects.
create schema if not exists extensions;
create extension if not exists pgcrypto with schema extensions;
create schema if not exists park_private;
revoke all on schema park_private from public, anon, authenticated;

-- Parks, authorized accounts, devices and persisted passage records.
create table public.parks (
  id uuid primary key default gen_random_uuid(),
  name text not null check (length(name) between 1 and 100),
  access_name text not null default 'Acesso principal',
  created_at timestamptz not null default now()
);
create table public.park_members (
  park_id uuid not null references public.parks(id),
  user_id uuid not null references auth.users(id),
  role text not null check (role in ('owner','reader')),
  primary key(park_id,user_id)
);
create table public.park_devices (
  id uuid primary key default gen_random_uuid(),
  park_id uuid not null references public.parks(id),
  name text not null check(length(name) between 1 and 100),
  enabled boolean not null default true,
  last_sync timestamptz,
  storage_ok boolean,
  clock_ok boolean,
  created_at timestamptz not null default now(),
  operation jsonb not null default '{}'::jsonb
);
create table park_private.device_credentials (
  device_id uuid primary key references public.park_devices(id),
  token_hash bytea not null
);
create table public.visit_records (
  device_id uuid not null references public.park_devices(id),
  record_id text not null check(record_id ~ '^[a-f0-9]{64}$'),
  recorded_at timestamptz not null,
  entries numeric(20,0) not null check(entries between 0 and 18446744073709551615),
  exits numeric(20,0) not null check(exits between 0 and 18446744073709551615),
  kind text not null check(kind in ('movement','sample','baseline','reset')),
  received_at timestamptz not null default now(),
  primary key(device_id,record_id)
);
create table park_private.camera_access (
  device_id uuid primary key references public.park_devices(id) on delete cascade,
  address inet not null,
  access_key text not null check (access_key ~ '^[a-f0-9]{32}$'),
  updated_at timestamptz not null default now(),
  check (family(address)=4 and masklen(address)=32 and
    (address <<= inet '10.0.0.0/8' or address <<= inet '172.16.0.0/12' or
     address <<= inet '192.168.0.0/16'))
);
create index visit_records_time on public.visit_records(device_id,recorded_at);
create index park_devices_park on public.park_devices(park_id);

-- Browser access is read-only and filtered by membership through RLS.
alter table public.parks enable row level security;
alter table public.park_members enable row level security;
alter table public.park_devices enable row level security;
alter table public.visit_records enable row level security;
alter table park_private.device_credentials enable row level security;
alter table park_private.camera_access enable row level security;
revoke all on public.parks,public.park_members,public.park_devices,public.visit_records from anon,authenticated;
grant select on public.parks,public.park_members,public.park_devices,public.visit_records to authenticated;
revoke all on park_private.camera_access from public,anon,authenticated;
create policy own_memberships on public.park_members for select to authenticated using(user_id=(select auth.uid()));
create policy member_parks on public.parks for select to authenticated using(exists(select 1 from public.park_members m where m.park_id=parks.id and m.user_id=(select auth.uid())));
create policy member_devices on public.park_devices for select to authenticated using(exists(select 1 from public.park_members m where m.park_id=park_devices.park_id and m.user_id=(select auth.uid())));
create policy member_records on public.visit_records for select to authenticated using(exists(select 1 from public.park_devices d join public.park_members m on m.park_id=d.park_id where d.id=visit_records.device_id and m.user_id=(select auth.uid())));

-- Initial registration and owner-only management.
create function public.park_create(p_name text, p_access text default 'Acesso principal') returns uuid
language plpgsql security definer set search_path='' as $$
declare new_id uuid;
begin
  if auth.uid() is null then raise exception 'Login necessário' using errcode='42501'; end if;
  insert into public.parks(name,access_name) values(p_name,p_access) returning id into new_id;
  insert into public.park_members values(new_id,auth.uid(),'owner');
  return new_id;
end $$;

-- The credential is returned to the owner; only its hash is stored.
create function public.park_create_device(p_park uuid,p_name text) returns jsonb
language plpgsql security definer set search_path='' as $$
declare new_id uuid; device_token text;
begin
  if not exists(select 1 from public.park_members where park_id=p_park and user_id=auth.uid() and role='owner') then
    raise exception 'Sem permissão' using errcode='42501';
  end if;
  device_token:=encode(extensions.gen_random_bytes(32),'hex');
  insert into public.park_devices(park_id,name) values(p_park,p_name) returning id into new_id;
  insert into park_private.device_credentials values(new_id,extensions.digest(device_token,'sha256'));
  return jsonb_build_object('device_id',new_id,'device_token',device_token);
end $$;

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

create function public.park_update_identification(p_park uuid,p_name text,p_access text) returns void
language plpgsql security definer set search_path='' as $$
begin
  if not exists(select 1 from public.park_members where park_id=p_park and user_id=auth.uid() and role='owner') then
    raise exception 'Sem permissão para editar a identificação' using errcode='42501';
  end if;
  if p_name is null or p_access is null or length(btrim(p_name)) not between 1 and 80 or length(btrim(p_access)) not between 1 and 80 then
    raise exception 'Preencha nome e ponto de contagem, com até 80 caracteres';
  end if;
  update public.parks set name=btrim(p_name),access_name=btrim(p_access) where id=p_park;
end $$;

-- Authenticated device ingestion. Stable IDs make confirmed replays idempotent.
create function park_private.ingest_records(p_device uuid,p_token text,p_records jsonb,p_status jsonb default '{}'::jsonb) returns jsonb
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

-- Public RPC: validate credentials first, then store optional private LAN access.
-- Any camera validation failure rolls back the entire request, including records.
create function public.park_ingest(p_device uuid,p_token text,p_records jsonb,p_status jsonb default '{}'::jsonb) returns jsonb
language plpgsql security definer set search_path='' as $$
declare result jsonb; camera jsonb; ip inet; secret text;
begin
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

-- The owner may retrieve the last LAN address; it is not proof of reachability.
create function public.park_camera_access(p_device uuid,p_include_key boolean default false) returns jsonb
language plpgsql stable security definer set search_path='' as $$
declare camera park_private.camera_access;
begin
  if not exists(select 1 from public.park_devices d join public.park_members m on m.park_id=d.park_id
    where d.id=p_device and d.enabled and m.user_id=auth.uid() and m.role='owner') then
    raise exception 'Sem permissão' using errcode='42501';
  end if;
  select * into camera from park_private.camera_access where device_id=p_device;
  if not found then return jsonb_build_object('available',false,'fresh',false); end if;
  return jsonb_build_object('available',true,'fresh',camera.updated_at>=now()-interval '2 minutes',
    'address',host(camera.address),'updated_at',camera.updated_at)
    || case when p_include_key is true then jsonb_build_object('url','http://'||host(camera.address)||'/#'||camera.access_key)
       else '{}'::jsonb end;
end $$;

-- Reports run with the caller's RLS permissions, including direct helper calls.
create function public.park_valid_start(p_start date,p_end date) returns date
language plpgsql immutable security invoker set search_path='' as $$
begin
  if p_start is null or p_end is null or p_end<p_start or p_end-p_start>365 then
    raise exception 'Período inválido';
  end if;
  return p_start;
end $$;

create function public.park_report_data(p_park uuid,p_start date,p_end date) returns jsonb
language sql stable security invoker set search_path='' as $$
with bounds as (
 select p_start::timestamp at time zone 'America/Sao_Paulo' a,(p_end+1)::timestamp at time zone 'America/Sao_Paulo' b
 where p_end>=p_start and p_end-p_start<=365
), records as (
 select r.*, (r.recorded_at at time zone 'America/Sao_Paulo')::date as visit_day,
 extract(hour from r.recorded_at at time zone 'America/Sao_Paulo')::integer as visit_hour
 from public.visit_records r join public.park_devices d on d.id=r.device_id cross join bounds
 where d.park_id=p_park and r.recorded_at>=bounds.a and r.recorded_at<bounds.b
), daily as (
 select g::date as visit_day,coalesce(sum(r.entries),0)::text entries,coalesce(sum(r.exits),0)::text exits,count(r.record_id) samples
 from generate_series(public.park_valid_start(p_start,p_end)::timestamp,p_end::timestamp,'1 day') g left join records r on r.visit_day=g::date
 group by g
), chart as (
 select h h_order,jsonb_build_object('label',lpad(h::text,2,'0')||'h','entries',coalesce(sum(r.entries),0)::text,'exits',coalesce(sum(r.exits),0)::text,'samples',count(r.record_id)) bucket
 from generate_series(0,23) h left join records r on r.visit_hour=h group by h
), recent as (select * from records where kind='movement' order by recorded_at desc,record_id desc limit 100)
select jsonb_build_object(
 'start',p_start,'end',p_end,'entries',coalesce(sum(entries),0)::text,'exits',coalesce(sum(exits),0)::text,
 'sample_count',count(*),'has_samples',count(*)>0,'started_at',min(recorded_at),'last_record',max(recorded_at),
 'daily',(select coalesce(jsonb_agg(jsonb_build_object('date',visit_day,'entries',entries,'exits',exits,'samples',samples) order by visit_day),'[]') from daily),
 'chart',case when p_start=p_end then (select jsonb_agg(bucket order by h_order) from chart)
 else (select jsonb_agg(jsonb_build_object('label',to_char(visit_day,'DD/MM'),'entries',entries,'exits',exits,'samples',samples) order by visit_day) from daily) end,
 'history',(select coalesce(jsonb_agg(jsonb_build_object('at',recorded_at,'entries',entries::text,'exits',exits::text,'kind',kind) order by recorded_at desc,record_id desc),'[]') from recent)
) from records;
$$;

create function public.park_report(p_park uuid,p_start date,p_end date) returns jsonb
language plpgsql stable security invoker set search_path='' as $$
begin
  if p_start is null or p_end is null or p_end<p_start or p_end-p_start>365 then
    raise exception 'Selecione um período válido de até 366 dias';
  end if;
  return public.park_report_data(p_park,p_start,p_end);
end $$;

-- Only the public ingestion RPC is callable without a user session.
revoke all on function park_private.ingest_records(uuid,text,jsonb,jsonb) from public,anon,authenticated;
revoke all on function public.park_create(text,text),public.park_create_device(uuid,text),
  public.park_rotate_device(uuid),public.park_update_identification(uuid,text,text),
  public.park_ingest(uuid,text,jsonb,jsonb),public.park_camera_access(uuid,boolean),
  public.park_valid_start(date,date),public.park_report_data(uuid,date,date),
  public.park_report(uuid,date,date) from public,anon,authenticated;
grant execute on function public.park_create(text,text),public.park_create_device(uuid,text),
  public.park_rotate_device(uuid),public.park_update_identification(uuid,text,text),
  public.park_camera_access(uuid,boolean),public.park_valid_start(date,date),
  public.park_report_data(uuid,date,date),public.park_report(uuid,date,date) to authenticated;
grant execute on function public.park_ingest(uuid,text,jsonb,jsonb) to anon,authenticated;

notify pgrst,'reload schema';
commit;
