-- Transactional synthetic fixtures. Nothing remains in the database after ROLLBACK.
begin;
insert into auth.users(id,email) values
 ('00000000-0000-4000-8000-000000000001','fixture-owner@example.invalid'),
 ('00000000-0000-4000-8000-000000000002','fixture-other@example.invalid');
insert into public.parks(id,name) values('00000000-0000-4000-8000-000000000003','Fixture');
insert into public.park_members values('00000000-0000-4000-8000-000000000003','00000000-0000-4000-8000-000000000001','owner');
insert into public.park_devices(id,park_id,name) values('00000000-0000-4000-8000-000000000004','00000000-0000-4000-8000-000000000003','Fixture');
insert into park_private.device_credentials values('00000000-0000-4000-8000-000000000004',extensions.digest(repeat('a',64),'sha256'));
set local role anon;
do $$
declare result jsonb;
begin
  begin
    perform public.park_ingest('00000000-0000-4000-8000-000000000004',repeat('b',64),'[]');
    raise exception 'TEST FAILED: invalid token accepted';
  exception when insufficient_privilege then null; end;
  begin
    perform count(*) from public.visit_records;
    raise exception 'TEST FAILED: anonymous read permitted';
  exception when insufficient_privilege then null; end;
  result:=public.park_ingest('00000000-0000-4000-8000-000000000004',repeat('a',64),
    '[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","epoch":1767225600,"entries":"9007199254740993","exits":"2","kind":"movement"}]');
  if result->>'inserted'<>'1' then raise exception 'TEST FAILED: insert'; end if;
  result:=public.park_ingest('00000000-0000-4000-8000-000000000004',repeat('a',64),
    '[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","epoch":1767225600,"entries":"9007199254740993","exits":"2","kind":"movement"}]');
  if result->>'inserted'<>'0' then raise exception 'TEST FAILED: duplicate'; end if;
end $$;
reset role;
set local role authenticated;
select set_config('request.jwt.claim.sub','00000000-0000-4000-8000-000000000002',true);
do $$
begin
  if exists(select 1 from public.parks where id='00000000-0000-4000-8000-000000000003') then raise exception 'TEST FAILED: other user sees park'; end if;
  if exists(select 1 from public.visit_records where device_id='00000000-0000-4000-8000-000000000004') then raise exception 'TEST FAILED: other user sees records'; end if;
end $$;
select set_config('request.jwt.claim.sub','00000000-0000-4000-8000-000000000001',true);
do $$
declare result jsonb;
begin
  result:=public.park_report('00000000-0000-4000-8000-000000000003','2025-12-31','2025-12-31');
  if result->>'entries'<>'9007199254740993' or result->>'exits'<>'2' then raise exception 'TEST FAILED: exact totals or timezone'; end if;
  begin
    perform public.park_report_data('00000000-0000-4000-8000-000000000003','2000-01-01','2099-01-01');
    raise exception 'TEST FAILED: invalid range accepted';
  exception when raise_exception then
    if SQLERRM like 'TEST FAILED:%' then raise; end if;
  end;
end $$;
reset role;
rollback;
select 'PASS: token, anonymous denial, RLS isolation, deduplication, exact integers, timezone, interval limit. All fixtures rolled back.' as result;
