'use strict';
const $=id=>document.getElementById(id);
const cfg=window.PARK_CLOUD;
const number=v=>new Intl.NumberFormat('pt-BR').format(BigInt(v||'0'));
const escapeHTML=v=>String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const momentLabel=v=>v?new Date(v).toLocaleString('pt-BR',{timeZone:'America/Sao_Paulo'}):'Nenhuma sincronização';
const today=()=>{const p=Object.fromEntries(new Intl.DateTimeFormat('en-CA',{timeZone:'America/Sao_Paulo',year:'numeric',month:'2-digit',day:'2-digit'}).formatToParts(new Date()).map(p=>[p.type,p.value]));return p.year+'-'+p.month+'-'+p.day;};
let session=null,parks=[],devices=[],park='',start='',end='',report=null,page='overview',busy=false,generation=0,days=1;
try{session=JSON.parse(sessionStorage.getItem('park-cloud-session')||'null');}catch{}
function saveSession(value){session=value;try{if(value)sessionStorage.setItem('park-cloud-session',JSON.stringify(value));else sessionStorage.removeItem('park-cloud-session');}catch{}}
async function request(path,{body,auth=true,method}={}){
  if(!cfg?.publishableKey)throw Error('A chave pública do projeto ainda não foi configurada.');
  const response=await fetch(cfg.url+path,{method:method||(body?'POST':'GET'),headers:{apikey:cfg.publishableKey,...(auth&&session?{Authorization:'Bearer '+session.access_token}:{}),...(body?{'Content-Type':'application/json'}:{})},body:body?JSON.stringify(body):undefined,signal:AbortSignal.timeout(20000)});
  const data=await response.json().catch(()=>({}));
  if(!response.ok){if(response.status===401&&auth){saveSession(null);showAuth();}throw Error(data.msg||data.message||data.error_description||'Não foi possível consultar os dados.');}return data;
}
const rpc=(name,body)=>request('/rest/v1/rpc/'+name,{body});
function error(message=''){$('error').textContent=message;$('error').hidden=!message;}
function showAuth(){$('login').hidden=!!session;$('application').hidden=!session;}
async function refreshSession(){
  if(!session)return;
  if((session.expires_at||0)>Date.now()/1000+60)return;
  try{const result=await request('/auth/v1/token?grant_type=refresh_token',{auth:false,body:{refresh_token:session.refresh_token}});saveSession({...result,expires_at:Math.floor(Date.now()/1000)+result.expires_in});}
  catch(e){saveSession(null);showAuth();throw e;}
}
$('login-form').addEventListener('submit',async event=>{
  event.preventDefault();$('login-submit').disabled=true;$('login-message').textContent='Entrando…';
  const password=$('password').value;$('password').value='';
  try{const result=await request('/auth/v1/token?grant_type=password',{auth:false,body:{email:$('email').value,password}});saveSession({...result,expires_at:Math.floor(Date.now()/1000)+result.expires_in});showAuth();$('login-message').textContent='';await loadParks();}catch(e){$('login-message').textContent=e.message;}finally{$('login-submit').disabled=false;}
});
$('sign-out').addEventListener('click',async()=>{generation++;try{await request('/auth/v1/logout',{method:'POST'});}catch{}saveSession(null);parks=[];devices=[];park='';report=null;$('device-secret').hidden=true;$('device-token').value='';render();showAuth();});
function setDates(n){days=n;end=today();const d=new Date(end+'T12:00:00-03:00');d.setUTCDate(d.getUTCDate()-n+1);start=d.toISOString().slice(0,10);$('date-start').value=start;$('date-end').value=end;document.querySelectorAll('[data-days]').forEach(b=>{const active=+b.dataset.days===n;b.classList.toggle('selected',active);b.setAttribute('aria-pressed',String(active));});}
document.querySelectorAll('[data-days]').forEach(b=>b.addEventListener('click',()=>{generation++;setDates(+b.dataset.days);refresh();}));
$('date-form').addEventListener('submit',event=>{event.preventDefault();const a=$('date-start').value,b=$('date-end').value;if(!a||!b||a>b||b>today()||(new Date(b)-new Date(a))/86400000>365){error('Selecione até 366 dias, sem datas futuras.');return;}start=a;end=b;days=0;generation++;document.querySelectorAll('[data-days]').forEach(b=>{b.classList.remove('selected');b.setAttribute('aria-pressed','false');});refresh();});
$('custom-period').addEventListener('click',()=>{const open=$('date-form').hidden;$('date-form').hidden=!open;$('custom-period').setAttribute('aria-expanded',String(open));});
async function loadParks(){
  await refreshSession();parks=await request('/rest/v1/parks?select=id,name,access_name&order=created_at');
  $('onboarding').hidden=parks.length>0;$('report-controls').hidden=!parks.length;
  $('park-select').replaceChildren(...parks.map(p=>{const o=document.createElement('option');o.value=p.id;o.textContent=p.name;return o;}));
  park=parks.some(p=>p.id===park)?park:(parks[0]?.id||'');$('park-select').value=park;
  setPage();if(park)await refresh();else{report=null;devices=[];render();}
}
$('park-select').addEventListener('change',()=>{park=$('park-select').value;generation++;refresh();});
$('park-form').addEventListener('submit',async event=>{event.preventDefault();const button=event.submitter;button.disabled=true;try{park=await rpc('park_create',{p_name:$('park-name').value,p_access:$('access-input').value});await loadParks();}catch(e){error(e.message);}finally{button.disabled=false;}});
async function refresh(){
  if(!session||!park||busy||document.hidden)return;busy=true;const version=generation;
  try{await refreshSession();if(days&&end!==today())setDates(days);const [next,ds]=await Promise.all([rpc('park_report',{p_park:park,p_start:start,p_end:end}),request('/rest/v1/park_devices?select=id,name,last_sync,storage_ok,clock_ok,enabled&park_id=eq.'+encodeURIComponent(park))]);if(version!==generation)return;report=next;devices=ds;error();render();}
  catch(e){error(e.message+' Os dados exibidos podem estar desatualizados.');}finally{busy=false;if(version!==generation)refresh();}
}
function table(rows,columns){if(!rows.length)return '<p class="empty-list">Sem entradas ou saídas neste período.</p>';return '<div class="table-scroll"><table><thead><tr>'+columns.map(c=>'<th>'+escapeHTML(c.title)+'</th>').join('')+'</tr></thead><tbody>'+rows.map(row=>'<tr>'+columns.map(c=>'<td>'+escapeHTML(c.value(row))+'</td>').join('')+'</tr>').join('')+'</tbody></table></div>';}
const historyColumns=[{title:'Registrado em',value:r=>momentLabel(r.at)},{title:'Entradas',value:r=>number(r.entries)},{title:'Saídas',value:r=>number(r.exits)}];
function render(){
  const p=parks.find(p=>p.id===park);$('access-name').textContent=p?.access_name||'';
  const last=devices.map(d=>d.last_sync).filter(Boolean).sort().at(-1);
  const stale=!last||Date.now()-new Date(last)>120000;
  $('sync-note').textContent=last?'Última sincronização: '+momentLabel(last)+(stale?'. Sem atualização recente; consulte a conexão da placa.':''):'Aguardando o primeiro envio da ESP.';
  $('device-list').innerHTML=devices.length?devices.map(d=>'<h3>'+escapeHTML(d.name)+'</h3><dl><dt>Último envio</dt><dd>'+escapeHTML(momentLabel(d.last_sync))+'</dd><dt>microSD na última leitura</dt><dd>'+(d.storage_ok===true?'Gravação disponível':d.storage_ok===false?'Falha informada':'Sem leitura')+'</dd><dt>Relógio na última leitura</dt><dd>'+(d.clock_ok===true?'Válido':d.clock_ok===false?'Sem horário válido':'Sem leitura')+'</dd></dl>').join(''):'<p>Nenhuma placa cadastrada neste parque.</p>';
  $('device-form').hidden=!park;
  if(!report){$('entries').textContent='—';$('exits').textContent='—';$('chart').replaceChildren();$('recent-history').textContent='Aguardando registros.';$('daily-history').replaceChildren();$('full-history').replaceChildren();$('period-label').textContent='';return;}
  $('entries').textContent=report.has_samples?number(report.entries):'—';$('exits').textContent=report.has_samples?number(report.exits):'—';$('period-label').textContent=start.split('-').reverse().join('/')+' a '+end.split('-').reverse().join('/');
  const movements=(report.history||[]).filter(r=>r.kind==='movement');$('recent-history').innerHTML=table(movements.slice(0,5),historyColumns);$('full-history').innerHTML=table(movements,historyColumns);
  $('daily-history').innerHTML=table(report.daily||[],[{title:'Data',value:r=>r.date.split('-').reverse().join('/')},{title:'Entradas',value:r=>r.samples?number(r.entries):'—'},{title:'Saídas',value:r=>r.samples?number(r.exits):'—'},{title:'Estado',value:r=>r.samples?'Registros disponíveis':'Sem registros'}]);
  drawChart(report.chart||[]);
}
function drawChart(buckets){
  document.querySelector('.cloud-empty')?.remove();
  if(!buckets.some(b=>b.samples)){const note=document.createElement('p');note.className='cloud-empty';note.textContent='Ainda não há registros da ESP neste período.';$('chart').parentElement.append(note);}
  $('chart-detail').hidden=true;const max=buckets.reduce((m,b)=>[m,BigInt(b.entries),BigInt(b.exits)].reduce((a,v)=>v>a?v:a),1n);
  $('chart').replaceChildren(...buckets.map(b=>{
    const button=document.createElement('button');button.className='cloud-bucket'+(!b.samples?' missing':'');button.type='button';
    const detail=()=>{$('chart-detail').textContent=b.label+(b.samples?': '+number(b.entries)+' entradas, '+number(b.exits)+' saídas.':': sem registros.');$('chart-detail').hidden=false;};
    button.setAttribute('aria-label',b.label+(b.samples?', '+number(b.entries)+' entradas e '+number(b.exits)+' saídas':', sem registros'));
    const bars=document.createElement('span');bars.className='cloud-bars';bars.setAttribute('aria-hidden','true');
    for(const [key,cls] of [['entries',''],['exits',' exit']]){const bar=document.createElement('span');bar.className='cloud-bar'+cls;bar.style.height=Number(BigInt(b[key])*22000n/max)/100+'px';bars.append(bar);}
    const label=document.createElement('span');label.textContent=b.label;button.append(bars,label);button.addEventListener('pointerenter',detail);button.addEventListener('focus',detail);button.addEventListener('click',detail);return button;
  }));
}
$('chart').addEventListener('pointerleave',()=>{$('chart-detail').hidden=true;});$('chart').addEventListener('focusout',e=>{if(!$('chart').contains(e.relatedTarget))$('chart-detail').hidden=true;});document.addEventListener('pointerdown',e=>{if(!$('chart').contains(e.target))$('chart-detail').hidden=true;});
function setPage(){page=location.hash.slice(1);const titles={overview:'Visitação',history:'Histórico',equipment:'Equipamento'};if(!titles[page])page='overview';$('page-title').textContent=titles[page];for(const id of Object.keys(titles))$('page-'+id).hidden=id!==page;document.querySelectorAll('[data-page]').forEach(a=>{const active=a.dataset.page===page;a.classList.toggle('active',active);if(active)a.setAttribute('aria-current','page');else a.removeAttribute('aria-current');});$('report-controls').hidden=!park||!['overview','history'].includes(page);$('chart-detail').hidden=true;}
window.addEventListener('hashchange',setPage);
function setMenu(open){$('park-sidebar').classList.toggle('minimized',!open);document.body.classList.toggle('menu-minimized',!open);$('toggle-menu').setAttribute('aria-expanded',String(open));$('toggle-menu').setAttribute('aria-label',open?'Minimizar menu':'Expandir menu');}
$('toggle-menu').addEventListener('click',()=>setMenu($('toggle-menu').getAttribute('aria-expanded')!=='true'));setMenu(!matchMedia('(max-width:900px)').matches);
$('export').addEventListener('click',()=>{if(!report)return;const csv='data,entradas,saidas\r\n'+report.daily.map(r=>[r.date,r.samples?r.entries:'',r.samples?r.exits:''].join(',')).join('\r\n');const url=URL.createObjectURL(new Blob(['\ufeff'+csv],{type:'text/csv;charset=utf-8'}));const a=document.createElement('a');a.href=url;a.download='visitacao-'+start+'-'+end+'.csv';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});
$('print').addEventListener('click',()=>window.print());document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh();});
$('device-form').addEventListener('submit',async event=>{event.preventDefault();const button=event.submitter;button.disabled=true;try{const result=await rpc('park_create_device',{p_park:park,p_name:$('device-name').value});$('device-id').value=result.device_id;$('device-token').value=result.device_token;$('device-secret').hidden=false;await refresh();}catch(e){error(e.message);}finally{button.disabled=false;}});
$('hide-device-secret').addEventListener('click',()=>{$('device-token').value='';$('device-id').value='';$('device-secret').hidden=true;});
setDates(1);$('date-start').max=today();$('date-end').max=today();setPage();showAuth();if(session)loadParks().catch(e=>error(e.message));setInterval(refresh,30000);
