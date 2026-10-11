begin;
-- Edit the existing park only. No new park, device, or credential is created.
create or replace function public.park_update_identification(p_park uuid,p_name text,p_access text) returns void
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
revoke all on function public.park_update_identification(uuid,text,text) from public,anon;
grant execute on function public.park_update_identification(uuid,text,text) to authenticated;
commit;
