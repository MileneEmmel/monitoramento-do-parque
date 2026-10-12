-- Synthetic credentials only. All fixtures and changes roll back.
begin;
insert into auth.users(id,email) values
 ('00000000-0000-4000-8000-000000000021','camera-owner@example.invalid'),
 ('00000000-0000-4000-8000-000000000022','camera-reader@example.invalid'),
 ('00000000-0000-4000-8000-000000000023','camera-other@example.invalid');
insert into public.parks(id,name) values('00000000-0000-4000-8000-000000000024','Camera fixture');
insert into public.park_members values
 ('00000000-0000-4000-8000-000000000024','00000000-0000-4000-8000-000000000021','owner'),
 ('00000000-0000-4000-8000-000000000024','00000000-0000-4000-8000-000000000022','reader');
insert into public.park_devices(id,park_id,name) values
 ('00000000-0000-4000-8000-000000000025','00000000-0000-4000-8000-000000000024','Camera fixture');
insert into park_private.device_credentials values
 ('00000000-0000-4000-8000-000000000025',extensions.digest(repeat('a',64),'sha256'));
set local role anon;
do $$
declare result jsonb; ip text;
begin
  begin
    perform public.park_ingest('00000000-0000-4000-8000-000000000025',repeat('b',64),'[]',
      jsonb_build_object('camera',jsonb_build_object('address','192.168.1.22','access_key',repeat('c',32))));
    raise exception 'TEST FAILED: invalid token';
  exception when insufficient_privilege then null; end;
  result:=public.park_ingest('00000000-0000-4000-8000-000000000025',repeat('a',64),'[]',
    jsonb_build_object('camera',jsonb_build_object('address','192.168.1.22','access_key',repeat('c',32))));
  if result->>'accepted'<>'0' then raise exception 'TEST FAILED: ACK changed'; end if;
  -- Legacy firmware, without camera status, still works.
  perform public.park_ingest('00000000-0000-4000-8000-000000000025',repeat('a',64),'[]');
  foreach ip in array array['127.0.0.1','8.8.8.8','172.15.0.1','::1','192.168.1.22/24','javascript:x','192.168.1.22/path'] loop
    begin
      perform public.park_ingest('00000000-0000-4000-8000-000000000025',repeat('a',64),'[]',
        jsonb_build_object('camera',jsonb_build_object('address',ip,'access_key',repeat('c',32))));
      raise exception 'TEST FAILED: invalid address accepted';
    exception when others then
      if SQLERRM like 'TEST FAILED:%' then raise; end if;
      if SQLERRM<>'Endereço local inválido' then raise; end if;
    end;
  end loop;
  begin
    perform public.park_ingest('00000000-0000-4000-8000-000000000025',repeat('a',64),'[]',
      jsonb_build_object('camera',jsonb_build_object('address','10.0.0.2','access_key','invalid')));
    raise exception 'TEST FAILED: invalid key accepted';
  exception when others then
    if SQLERRM like 'TEST FAILED:%' then raise; end if;
    if SQLERRM<>'Endereço local inválido' then raise; end if;
  end;
  -- A second network replaces the endpoint, rather than keeping a stale link.
  perform public.park_ingest('00000000-0000-4000-8000-000000000025',repeat('a',64),'[]',
    jsonb_build_object('camera',jsonb_build_object('address','172.16.1.50','access_key',repeat('d',32))));
  begin
    perform public.park_camera_access('00000000-0000-4000-8000-000000000025',true);
    raise exception 'TEST FAILED: anonymous camera read';
  exception when insufficient_privilege then null; end;
  begin
    perform park_private.ingest_records('00000000-0000-4000-8000-000000000025',repeat('a',64),'[]');
    raise exception 'TEST FAILED: private ingestion exposed';
  exception when insufficient_privilege then null; end;
end $$;
reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub','00000000-0000-4000-8000-000000000022',true);
do $$ begin
  begin
    perform public.park_camera_access('00000000-0000-4000-8000-000000000025',true);
    raise exception 'TEST FAILED: reader has camera control';
  exception when insufficient_privilege then null; end;
  begin
    perform count(*) from park_private.camera_access;
    raise exception 'TEST FAILED: private table exposed';
  exception when insufficient_privilege then null; end;
end $$;
select set_config('request.jwt.claim.sub','00000000-0000-4000-8000-000000000023',true);
do $$ begin
  begin
    perform public.park_camera_access('00000000-0000-4000-8000-000000000025',true);
    raise exception 'TEST FAILED: unrelated user has camera control';
  exception when insufficient_privilege then null; end;
end $$;
select set_config('request.jwt.claim.sub','00000000-0000-4000-8000-000000000021',true);
do $$ declare result jsonb; begin
  result:=public.park_camera_access('00000000-0000-4000-8000-000000000025',false);
  if result ? 'url' or result ? 'access_key' or result->>'address'<>'172.16.1.50' then
    raise exception 'TEST FAILED: metadata leaks key or wrong network'; end if;
  result:=public.park_camera_access('00000000-0000-4000-8000-000000000025',true);
  if result->>'url'<>'http://172.16.1.50/#'||repeat('d',32) then raise exception 'TEST FAILED: owner URL'; end if;
end $$;
reset role;
update park_private.camera_access set updated_at=now()-interval '3 minutes'
 where device_id='00000000-0000-4000-8000-000000000025';
set local role authenticated;
select set_config('request.jwt.claim.sub','00000000-0000-4000-8000-000000000021',true);
do $$ declare result jsonb; begin
  result:=public.park_camera_access('00000000-0000-4000-8000-000000000025',true);
  if result ? 'url' or (result->>'fresh')::boolean then raise exception 'TEST FAILED: stale link enabled'; end if;
end $$;
reset role;
rollback;
select 'PASS: owner-only camera link; readers, strangers and anonymous denied; no key in metadata; private IPv4 validation; network updates; stale link disabled; legacy ACK preserved. Fixtures rolled back.' as result;
