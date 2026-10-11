begin;
-- Reject invalid/oversized intervals before generate_series can allocate rows.
alter function public.park_report(uuid,date,date) rename to park_report_data;
revoke all on function public.park_report_data(uuid,date,date) from public,anon;
grant execute on function public.park_report_data(uuid,date,date) to authenticated;
create function public.park_report(p_park uuid,p_start date,p_end date) returns jsonb
language plpgsql stable security invoker set search_path='' as $$
begin
  if p_start is null or p_end is null or p_end<p_start or p_end-p_start>365 then
    raise exception 'Selecione um período válido de até 366 dias';
  end if;
  return public.park_report_data(p_park,p_start,p_end);
end $$;
revoke all on function public.park_report(uuid,date,date) from public,anon;
grant execute on function public.park_report(uuid,date,date) to authenticated;
-- The underlying helper is limited too, including direct RPC calls.
create function public.park_valid_start(p_start date,p_end date) returns date
language plpgsql immutable security invoker set search_path='' as $$
begin
  if p_start is null or p_end is null or p_end<p_start or p_end-p_start>365 then
    raise exception 'Período inválido';
  end if;
  return p_start;
end $$;
revoke all on function public.park_valid_start(date,date) from public,anon;
grant execute on function public.park_valid_start(date,date) to authenticated;
-- Patch the function definition without changing policies or existing records.
do $$
declare definition text;
begin
  select pg_get_functiondef('public.park_report_data(uuid,date,date)'::regprocedure) into definition;
  definition:=replace(definition,'generate_series(p_start::timestamp,p_end::timestamp',
    'generate_series(public.park_valid_start(p_start,p_end)::timestamp,p_end::timestamp');
  execute definition;
end $$;
commit;
